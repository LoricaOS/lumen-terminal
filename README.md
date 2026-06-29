# lumen-terminal

The Terminal app for **AspisOS**, a capability-based, no-ambient-authority
operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel.

terminal is a graphical terminal emulator. It is a standalone component of the
Lumen desktop, distributed as a [herald](https://github.com/AspisOS/AspisOS)
package, and runs as an external client of the
[lumen](https://github.com/AspisOS/lumen) compositor (it connects to
`/run/lumen.sock` over the external window protocol rather than being an
in-process compositor built-in — this is the result of the subsystem-peeling
work that moved regular terminal windows out of Lumen).

## Role in the system

- A `/apps` bundle app: launched from the desktop via its `app.ini` descriptor
  (`name=Terminal`, `exec=terminal`).
- Opens a PTY, spawns `/bin/stsh` on the slave via `sys_spawn`, and bridges the
  two halves:
  - PTY master output is fed to the `glyph_term` emulator core (libglyph),
    rendered into the window backbuffer, and presented to Lumen as damage.
  - `LUMEN_EV_KEY` events are translated back into VT input on the PTY master.
- Key translation: Lumen delivers one byte per key event. Synthetic arrow codes
  (`0xF1..0xF4`) are re-expanded to VT sequences (`\033[A`..`\033[D`); everything
  else, including `^C` and bare Esc, writes through verbatim.
- Window sizing: aims for 100x48 cells at the mono font's cell metrics, clamped
  to roughly 7/8 of the framebuffer when Lumen passes `LUMEN_FB_W/LUMEN_FB_H`.
  A 500-line scrollback covers taller output.
- Signal handling reflects PTY foreground-group subtleties: SIGTERM closes
  gracefully, SIGINT/SIGQUIT are ignored (a stray `^C` must not kill the
  emulator before the shell claims the foreground), SIGPIPE is ignored.
- Tracks the shell's admin-session state via an OSC and asks Lumen to tint the
  titlebar accordingly.

## Capabilities

terminal's cap policy (`pkg/etc/aegis/caps.d/terminal`) is the baseline:

```
service
```

No elevated capabilities — it runs as an ordinary service-profile client. The
shell it spawns inherits the session's identity.

Because its herald package id (`lumen-terminal`) intentionally differs from the
bundle/binary name (`terminal`), and it installs across `/apps` and
`/etc/aegis/caps.d`, terminal is a `class=system` package: first-party and
signature-trusted, installed verbatim by herald.

## Building

terminal fetches a pinned [glyph](https://github.com/AspisOS/glyph) toolkit
artifact (the GUI libraries it links) and builds against it, then packs a signed
herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the toolkit release fetched by `tools/fetch-glyph.sh`.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption — point it
  at an Aegis-native `cc` to build on-device in the future).
- `HERALD_KEY` signs the `.hpkg`.

Output: `lumen-terminal.hpkg` (a `class=system` herald package) +
`lumen-terminal.hpkg.sig`.

## Package payload

```
/apps/terminal/terminal         the app binary
/apps/terminal/app.ini          the bundle descriptor (launcher metadata)
/etc/aegis/caps.d/terminal      its capability policy
```

## Repository layout

```
src/        terminal source
pkg/        install-tree skeleton shipped verbatim (apps bundle + caps.d)
tools/      fetch-glyph.sh (toolkit fetch) + pack.sh (build the signed .hpkg)
Makefile    fetch toolkit -> build -> pack
VERSION         this component's version
GLYPH_VERSION   the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — terminal is an external client of the compositor, so
installing it pulls [lumen](https://github.com/AspisOS/lumen) (which also
supplies the desktop fonts).
