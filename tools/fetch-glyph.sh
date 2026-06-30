#!/usr/bin/env bash
# fetch-glyph.sh — fetch + unpack the glyph toolkit artifact a component builds
# against. Canonical copy lives here; component repos carry their own copy.
#
# Resolution: local cache vendor/glyph-<ver>.tar.gz, else download the release.
# Unpacks include/ + lib/ into <dest-dir>. Mirrors LoricaOS's fetch-kernel.sh.
set -eu
VER="${1:?usage: fetch-glyph.sh <version> <dest-dir>}"
DEST="${2:?usage: fetch-glyph.sh <version> <dest-dir>}"

CACHE="vendor/glyph-${VER}.tar.gz"
URL="https://github.com/LoricaOS/glyph/releases/download/v${VER}/glyph-${VER}.tar.gz"

mkdir -p vendor "$DEST"
if [ -f "$CACHE" ]; then
    echo "[fetch-glyph] using cached toolkit $CACHE (v$VER)"
else
    echo "[fetch-glyph] downloading glyph toolkit v$VER"
    echo "[fetch-glyph]   $URL"
    if ! curl -fsSL "$URL" -o "$CACHE.tmp"; then
        echo "[fetch-glyph] ERROR: could not fetch toolkit v$VER" >&2
        echo "[fetch-glyph] (place it at $CACHE manually to build offline)" >&2
        rm -f "$CACHE.tmp"
        exit 1
    fi
    mv "$CACHE.tmp" "$CACHE"
fi

tar xzf "$CACHE" -C "$DEST" --strip-components=1
echo "[fetch-glyph] glyph v$VER -> $DEST (include/ + lib/)"
