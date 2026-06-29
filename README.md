# lumen-terminal

The Terminal app for **AspisOS**, a capability-based, no-ambient-authority
x86-64 operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel.

terminal is a graphical terminal emulator: it opens a pseudo-terminal, spawns a
shell on the slave, and bridges that PTY to a window. It is a standalone
component of the Lumen desktop, distributed as a
[herald](https://github.com/AspisOS/AspisOS) package, and runs as an **external
client** of the [lumen](https://github.com/AspisOS/lumen) compositor — it
connects to `/run/lumen.sock` over the Lumen window protocol rather than being an
in-process compositor built-in. (Regular terminal windows were moved out of
Lumen by the subsystem-peeling work; only the quake-style dropdown terminal
remains embedded in the compositor.)

## Where terminal fits

AspisOS is decomposed into independent repositories. terminal sits at the leaf
of the graphical stack:

| Repo | Role |
|------|------|
| [`AspisOS/Aegis`](https://github.com/AspisOS/Aegis) | The kernel: capability model, PTYs, `AF_UNIX` sockets, `sys_spawn`, the syscalls the desktop runs on. |
| [`AspisOS/lumen`](https://github.com/AspisOS/lumen) | The compositor / display server. Owns the framebuffer; every GUI app is one of its clients. |
| [`AspisOS/glyph`](https://github.com/AspisOS/glyph) | The GUI toolkit terminal links against: the shared terminal-emulator core (`glyph_term`), `glyph_pty_open_and_spawn`, the renderer, and the client side of the Lumen protocol (`lumen_client.h`). |
| `AspisOS/lumen-terminal` | **This repo.** The standalone terminal app. |

## What it does

Grounded in `src/main.c`:

- Connects to Lumen (`lumen_connect_retry`), opens a PTY and spawns `/bin/stsh`
  on the slave (`glyph_pty_open_and_spawn`), and bridges the two halves:
  - PTY master output is fed to glyph's `glyph_term` emulator core, rendered
    into the window backbuffer, and presented to Lumen as damage.
  - `LUMEN_EV_KEY` events are translated back to VT input on the PTY master
    (`glyph_term_translate_key`). Mouse down/move/up drive `glyph_term`
    selection.
- **Key translation:** Lumen delivers one byte per key event. Synthetic arrow
  codes (`0xF1..0xF4`) are re-expanded to VT sequences (`\033[A`..`\033[D`);
  everything else, including `^C` and bare Esc, writes through verbatim.
- **Window sizing:** aims for 100x48 cells at the mono font's cell metrics,
  clamped to roughly 7/8 of the framebuffer when Lumen passes
  `LUMEN_FB_W`/`LUMEN_FB_H` in the environment (the v1 protocol has no resize
  op). A 500-line scrollback covers taller output.
- **Event loop** is `poll(2)` over the Lumen socket and the PTY master, with a
  250 ms timeout that drives cursor blink while idle.
- **Signal handling** reflects PTY foreground-group subtleties: SIGTERM closes
  gracefully; SIGINT/SIGQUIT are ignored so a stray `^C` delivered before the
  shell claims the foreground cannot kill the emulator; SIGPIPE is ignored so a
  dead compositor socket fails with `EPIPE` rather than a signal.
- On shell exit or socket loss it tears down cleanly — `SIGHUP`s a still-live
  shell, closes the master, destroys the emulator and window.
- Tracks the shell's admin-session state via an OSC and asks Lumen
  (`lumen_window_set_admin`) to tint the titlebar accordingly.

## Capabilities

AspisOS grants a process no ambient authority; it can touch the system only
through capabilities declared for it at exec time. terminal's policy
(`pkg/etc/aegis/caps.d/terminal`) is the baseline:

```
service
```

The `service` profile and **no** elevated capabilities — terminal runs as an
ordinary client. The shell it spawns inherits the session's own identity and
authority; the terminal grants it nothing extra.

## Status

terminal is a working but deliberately minimal emulator: a single window, one
shell, fixed size (no resize op in the v1 protocol), with selection, scrollback,
and the admin-tint signal already in place. Richer behavior (tabs, configurable
fonts/profiles, resize) is expected to land as AspisOS and the Lumen protocol
mature.

## Building

terminal builds with a musl cross-compiler against a **pinned**
[glyph](https://github.com/AspisOS/glyph) toolkit artifact (the GUI libraries it
links), then packs a signed herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `make` runs `tools/fetch-glyph.sh $(GLYPH_VERSION)` to download and unpack the
  pinned toolkit into `toolkit/`, compiles `src/*.c` against it, then packs.
- `MUSL_CC` is the musl cross-compiler (defaults to `musl-gcc` on `PATH`; the
  only toolchain assumption — point it at an Aegis-native `cc` to build on-device
  in the future).
- `HERALD_KEY` is the ECDSA-P256 key that signs the `.hpkg`.
- `GLYPH_VERSION` pins the toolkit release; `VERSION` is this app's own version.

Output: `lumen-terminal.hpkg` (a `class=system` herald package) +
`lumen-terminal.hpkg.sig`.

## Package payload

`lumen-terminal.hpkg` is a **herald `class=system` package**: a manifest-first
uncompressed POSIX `ustar` archive with a detached ECDSA-P256/SHA-256 signature
(`tools/pack.sh`). Its herald id (`lumen-terminal`) deliberately differs from the
bundle/exec name (`terminal`), and it installs across two trees — which is
exactly why it is `class=system` (first-party, signature-trusted, installed
verbatim) rather than an ordinary single-prefix package:

```
/apps/terminal/terminal         the app binary
/apps/terminal/app.ini          the bundle descriptor (name=Terminal, exec=terminal)
/etc/aegis/caps.d/terminal      its capability policy
```

## Repository layout

```
src/        terminal source (main.c)
pkg/        install-tree skeleton shipped verbatim (apps bundle + caps.d)
tools/      fetch-glyph.sh (pinned toolkit fetch) + pack.sh (build the signed .hpkg)
Makefile    fetch toolkit -> build -> pack
VERSION         this app's version
GLYPH_VERSION   the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — terminal is an external client of the compositor, so
installing it pulls [lumen](https://github.com/AspisOS/lumen). lumen also ships
the desktop fonts (Inter, JetBrains Mono — the monospace font this terminal
renders with), so terminal inherits them transitively; there is no separate font
package.
