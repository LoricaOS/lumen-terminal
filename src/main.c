/* user/bin/terminal/main.c — Aegis Terminal (external Lumen client)
 *
 * Phase 47b subsystem peeling, terminal step: regular terminal windows
 * are no longer in-process Lumen built-ins. This binary connects to
 * /run/lumen.sock via the external window protocol (same pattern as
 * /bin/settings), opens a PTY, spawns /bin/stsh on the slave via
 * sys_spawn, and bridges the two:
 *
 *   PTY master → glyph_term emulator core (libglyph) → backbuf →
 *   shared memfd → LUMEN_OP_DAMAGE
 *
 *   LUMEN_EV_KEY → glyph_term_translate_key → PTY master
 *
 * Key translation: Lumen delivers ONE byte per LUMEN_EV_KEY. Arrow
 * keys arrive as the synthetic codes 0xF1(Up)/0xF2(Down)/0xF3(Right)/
 * 0xF4(Left) (see lumen/main.c CSI handler); we re-expand them to VT
 * sequences ("\033[A".."\033[D") for the PTY. Everything else —
 * including 0x03 (^C) and bare 0x1B (Esc) — writes through verbatim.
 *
 * Window sizing: 80x24 cells at the mono font's cell size, clamped to
 * 3/5 of the framebuffer when Lumen passes LUMEN_FB_W/LUMEN_FB_H in
 * the environment (the v1 protocol has no resize op).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

#include <glyph.h>
#include <glyph_term.h>
#include <glyph_theme.h>
#include <lumen_client.h>
#include <font.h>

#define TERM_PAD 4   /* pixels between window edge and the cell grid */

static volatile sig_atomic_t s_term_requested;

static void sigterm_handler(int sig) { (void)sig; s_term_requested = 1; }

static int
env_int(const char *name)
{
    const char *v = getenv(name);
    return v ? atoi(v) : 0;
}

/* Cell metrics for a given font size. */
static void
cell_metrics(int font_px, int *cw, int *ch)
{
    *cw = g_font_mono ? font_text_width(g_font_mono, font_px, "M") : FONT_W;
    *ch = g_font_mono ? font_height(g_font_mono, font_px) : FONT_H;
}

/* Recompute cols/rows for the window's current pixel size at font_px and resize
 * the grid (also applies scrollback + pushes TIOCSWINSZ). Used on window resize
 * and on a live font-size/scrollback change. */
static void
term_refit(glyph_term_t *term, lumen_window_t *lwin, int font_px, int scrollback)
{
    int cw, ch;
    cell_metrics(font_px, &cw, &ch);
    int cols = (lwin->w - 2 * TERM_PAD - GLYPH_TERM_SB_WIDTH) / cw;
    int rows = (lwin->h - 2 * TERM_PAD) / ch;
    if (cols < 10) cols = 10;
    if (rows < 4)  rows = 4;
    term->scrollback = scrollback;
    glyph_term_resize(term, cols, rows, cw, ch, font_px);
}

/* Apply the live glyph_theme terminal prefs to the emulator. Returns 1 if a
 * font-size or scrollback change needs a grid refit (caller does term_refit). */
static int
term_apply_prefs(glyph_term_t *term)
{
    glyph_term_set_theme(term, glyph_theme_term_scheme());
    glyph_term_set_cursor_style(term, glyph_theme_term_cursor());
    glyph_term_set_cursor_blink(term, glyph_theme_term_blink());
    return term->font_px != glyph_theme_term_font_px() ||
           term->scrollback != glyph_theme_term_scrollback();
}

/* Cheap fingerprint of the terminal prefs, to detect live Settings changes. */
static unsigned
term_prefs_sig(void)
{
    return (unsigned)(glyph_theme_term_scheme() * 131 +
                      glyph_theme_term_cursor() * 17 +
                      glyph_theme_term_blink()  * 7  +
                      glyph_theme_term_font_px() * 3 +
                      glyph_theme_term_scrollback());
}

static long
mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Top-bar app menu. The only app-level action is closing the window (which
 * hangs up the shell on the way out); clear/copy/paste live in the shell and
 * the terminal's own key/mouse handling, not here. */
enum { CMD_CLOSE = 1 };

static void
publish_menu(lumen_window_t *lwin)
{
    lumen_set_menu_t m;
    glyph_menu_reset(&m, lwin->id);
    int t = glyph_menu_add_col(&m, "Terminal");
    glyph_menu_add_item(&m, t, "Close", CMD_CLOSE);
    lumen_window_set_menu(lwin, &m);
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Connect to Lumen (retries on ECONNREFUSED). */
    int lfd = lumen_connect_retry();
    if (lfd < 0) {
        dprintf(2, "[TERM] lumen_connect failed (%d)\n", lfd);
        return 1;
    }

    /* Fonts first — cell metrics decide the window size. font size from prefs. */
    font_init();
    int font_px = glyph_theme_term_font_px();
    int cell_w, cell_h;
    cell_metrics(font_px, &cell_w, &cell_h);

    /* Large by default: aim for 100x48 but fill most (≈7/8) of the framebuffer
     * when Lumen tells us the screen size, so long output (e.g. /proc/hda) is
     * readable. The 500-line scrollback covers anything taller. */
    int cols = 100, rows = 48;
    int fb_w = env_int("LUMEN_FB_W");
    int fb_h = env_int("LUMEN_FB_H");
    if (fb_w > 0 && fb_h > 0) {
        int max_cols = (fb_w * 7 / 8) / cell_w;
        int max_rows = (fb_h * 7 / 8 - GLYPH_TITLEBAR_HEIGHT) / cell_h;
        if (cols > max_cols) cols = max_cols;
        if (rows > max_rows) rows = max_rows;
    }
    if (cols < 10) cols = 10;
    if (rows < 4) rows = 4;

    /* Client area: pad + grid + scrollbar gutter + pad. Proxy windows
     * get server-side chrome, so the shared buffer IS the client area
     * (verified: lumen_server.c handle_create_common passes w,h to
     * glyph_window_create as client dimensions). */
    int win_w = TERM_PAD + cols * cell_w + GLYPH_TERM_SB_WIDTH + TERM_PAD;
    int win_h = TERM_PAD + rows * cell_h + TERM_PAD;

    lumen_window_t *lwin = lumen_window_create_ex(lfd, "Terminal", win_w, win_h,
                                                  LUMEN_WIN_FLAG_RESIZABLE);
    if (!lwin) {
        dprintf(2, "[TERM] lumen_window_create failed\n");
        close(lfd);
        return 1;
    }

    surface_t surf = {
        .buf   = (uint32_t *)lwin->backbuf,
        .w     = lwin->w,
        .h     = lwin->h,
        .pitch = lwin->stride,
    };

    glyph_term_t *term = glyph_term_create(cols, rows, cell_w, cell_h,
                                           TERM_PAD, TERM_PAD);
    if (!term) {
        dprintf(2, "[TERM] glyph_term_create failed\n");
        lumen_window_destroy(lwin);
        close(lfd);
        return 1;
    }

    /* Apply saved terminal prefs (color scheme, cursor, blink) + establish the
     * grid at the preferred scrollback/font. */
    term_apply_prefs(term);
    term_refit(term, lwin, font_px, glyph_theme_term_scrollback());

    /* Signals: SIGTERM → graceful close. SIGINT/SIGQUIT ignored — the
     * kernel's receive-time ISIG may target the PTY's initial fg_pgrp
     * (us, the opener) before the shell claims foreground via
     * sys_setfg; a stray ^C must not kill the emulator. SIGPIPE
     * ignored so writes to a dead compositor socket fail with EPIPE
     * instead of killing us before cleanup. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    /* Open PTY + spawn the shell (master is O_NONBLOCK). */
    const char *fail_reason = NULL;
    int shell_pid = -1;
    int mfd = glyph_pty_open_and_spawn(&fail_reason, &shell_pid);
    if (mfd >= 0) {
        term->master_fd = mfd;
        glyph_term_apply_winsize(term);   /* tell the shell our real size */
    } else {
        dprintf(2, "[TERM] pty failed: %s\n",
                fail_reason ? fail_reason : "unknown");
        glyph_term_puts(term, "Terminal: PTY unavailable\n");
        if (fail_reason)
            glyph_term_puts(term, fail_reason);
        glyph_term_puts(term, "\n\nNo shell. Close this window.");
    }

    glyph_term_render(term, &surf, 0, 0, lwin->w, lwin->h);
    lumen_window_present(lwin);

    /* Publish the top-bar app menu once the window exists. */
    publish_menu(lwin);

    dprintf(2, "[TERM] connected %dx%d cols=%d rows=%d\n",
            lwin->w, lwin->h, cols, rows);

    /* ── Event loop: poll(2) over the Lumen socket and the PTY ────── */
    int done = 0;
    int shell_gone = 0;
    int conn_lost = 0;
    int last_blink = -1;
    char pty_buf[512];
    char keybuf[4];
    unsigned last_sig = term_prefs_sig();
    long last_pref_ms = mono_ms();

    while (!done && !s_term_requested) {
        struct pollfd pfd[2];
        pfd[0].fd = lfd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = mfd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        int nfds = (mfd >= 0) ? 2 : 1;

        /* 250ms timeout keeps the cursor blinking when idle. */
        int pr = poll(pfd, (nfds_t)nfds, 250);
        int dirty = 0;

        /* Live Settings changes: re-read prefs ~1×/s and apply (theme/cursor/
         * blink instantly; font-size/scrollback trigger a grid refit). */
        long tnow = mono_ms();
        if (tnow - last_pref_ms >= 1000) {
            last_pref_ms = tnow;
            glyph_theme_reload_prefs();
            unsigned sig = term_prefs_sig();
            if (sig != last_sig) {
                last_sig = sig;
                if (term_apply_prefs(term))
                    term_refit(term, lwin, glyph_theme_term_font_px(),
                               glyph_theme_term_scrollback());
                dirty = 1;
            }
        }

        if (pr > 0) {
            /* Lumen events */
            if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                lumen_event_t ev;
                int r;
                while ((r = lumen_poll_event(lfd, &ev)) == 1) {
                    if (ev.type == LUMEN_EV_CLOSE_REQUEST) {
                        done = 1;
                        break;
                    }
                    if (ev.type == LUMEN_EV_MENU_INVOKE) {
                        if (ev.menu.command == CMD_CLOSE)
                            done = 1;
                        continue;
                    }
                    if (ev.type == LUMEN_EV_KEY && ev.key.pressed && mfd >= 0) {
                        int n = glyph_term_translate_key(
                            (unsigned char)ev.key.keycode, keybuf);
                        write(mfd, keybuf, (size_t)n);
                    }
                    if (ev.type == LUMEN_EV_MOUSE) {
                        if (ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                            (ev.mouse.buttons & 1))
                            dirty |= glyph_term_mouse_down(term, ev.mouse.x,
                                                           ev.mouse.y);
                        else if (ev.mouse.evtype == LUMEN_MOUSE_MOVE)
                            dirty |= glyph_term_mouse_move(term, ev.mouse.x,
                                                           ev.mouse.y);
                        else if (ev.mouse.evtype == LUMEN_MOUSE_UP)
                            dirty |= glyph_term_mouse_up(term, ev.mouse.x,
                                                         ev.mouse.y);
                    }
                    if (ev.type == LUMEN_EV_RESIZED) {
                        if (lumen_window_apply_resize(lwin, ev.resized.new_w,
                                                      ev.resized.new_h) == 0) {
                            surf.buf   = (uint32_t *)lwin->backbuf;
                            surf.w     = lwin->w;
                            surf.h     = lwin->h;
                            surf.pitch = lwin->stride;
                            term_refit(term, lwin, term->font_px, term->scrollback);
                            dirty = 1;
                        }
                    }
                    /* LUMEN_EV_FOCUS and unknown events: ignored */
                }
                if (r < 0) {
                    conn_lost = 1;
                    done = 1;
                }
            }

            /* PTY output. Nonblocking master: -EAGAIN = no data,
             * 0 = EOF (slave side fully closed → shell exited). */
            if (mfd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n;
                while ((n = read(mfd, pty_buf, sizeof(pty_buf))) > 0) {
                    glyph_term_feed(term, pty_buf, (int)n);
                    dirty = 1;
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    shell_gone = 1;
                    done = 1;
                }
                /* The shell may have emitted the admin OSC in that output;
                 * if its admin-session state flipped, tell Lumen so it tints
                 * the titlebar red (or restores it on deadmin/exit). */
                int admin_now;
                if (glyph_term_admin_take_change(term, &admin_now))
                    lumen_window_set_admin(lwin, admin_now);
            }
        } else if (pr == 0) {
            /* Idle: repaint only when the cursor blink phase flips. */
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            int phase = (int)(now.tv_nsec / 500000000L) & 1;
            if (phase != last_blink) {
                last_blink = phase;
                if (mfd >= 0)
                    dirty = 1;
            }
        }

        if (dirty) {
            glyph_term_render(term, &surf, 0, 0, lwin->w, lwin->h);
            lumen_window_present(lwin);
        }
    }

    if (shell_gone)
        dprintf(2, "[TERM] shell exited\n");
    if (conn_lost)
        dprintf(2, "[TERM] compositor connection lost\n");

    /* Hang up the shell if it's still alive (close request / SIGTERM
     * path). Closing the master also makes the slave read EIO, so the
     * shell unblocks and exits; the PTY pair is then reclaimed. */
    if (!shell_gone && shell_pid > 0)
        kill(shell_pid, SIGHUP);
    if (mfd >= 0)
        close(mfd);

    glyph_term_destroy(term);
    lumen_window_destroy(lwin);
    close(lfd);
    dprintf(2, "[TERM] exit\n");
    return 0;
}
