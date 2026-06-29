#!/bin/sh
# pack.sh — build the signed herald package (.hpkg) for this component.
#
# A .hpkg is a manifest-first uncompressed POSIX ustar + a detached
# ECDSA-P256/SHA-256 signature. Every Lumen-ecosystem component is class=system
# (first-party, signature-trusted: its herald id differs from the bundle/exec
# name, and it may install across /bin /apps /etc), so herald installs the
# whole payload tree verbatim.
#
# Per-component knobs are the five variables below; the payload is the built
# binary placed at DESTBIN plus everything under pkg/ (the install-tree
# skeleton: caps.d, vigil service, app.ini, ...). Nothing else changes between
# components.
set -eu
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
VER="$(cat VERSION)"
KEY="${HERALD_KEY:?set HERALD_KEY to the package signing key}"
STRIP="${STRIP:-strip}"

# ── per-component knobs ──────────────────────────────────────────────────────
ID=lumen-terminal                       # herald package id (differs from bundle/exec name)
NAME="Terminal"
DESTBIN=apps/terminal/terminal              # where the binary installs (verbatim runtime path)
DEPENDS=lumen                    # herald dependencies (space-separated)
# ─────────────────────────────────────────────────────────────────────────────

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$(dirname "$DESTBIN")"
if ! "$STRIP" -o "$STAGE/$DESTBIN" component.elf 2>/dev/null; then
    cp component.elf "$STAGE/$DESTBIN"
fi
chmod 0755 "$STAGE/$DESTBIN"
# pkg/ mirrors the install tree (caps.d / vigil / app.ini) — ship it verbatim.
[ -d pkg ] && cp -R pkg/. "$STAGE/"
printf 'id=%s\nname=%s\nversion=%s\nclass=system\ndepends=%s\n' \
    "$ID" "$NAME" "$VER" "$DEPENDS" > "$STAGE/manifest"

roots="$(cd "$STAGE" && ls -A | grep -v '^manifest$' | tr '\n' ' ')"
cd "$STAGE" && tar --format=ustar -cf "$ROOT/$ID.hpkg" manifest $roots
cd "$ROOT"
openssl dgst -sha256 -sign "$KEY" -out "$ID.hpkg.sig" "$ID.hpkg"
echo "[$ID] $ID.hpkg $VER ($(wc -c < "$ID.hpkg") bytes) + .sig"
