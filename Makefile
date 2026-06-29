# Component build — fetch the pinned glyph toolkit artifact, compile the
# component binary against it, then pack a signed herald package (.hpkg).
#
# This Makefile is identical across every Lumen-ecosystem component repo; what
# the package ships is defined by src/ + pkg/ + the variables at the top of
# tools/pack.sh. Linking all four toolkit libs is harmless — a static archive
# only contributes the objects actually referenced.
#
#   make                fetch toolkit + build component.elf + pack the .hpkg
#   HERALD_KEY=<key>    signing key for the package (required)
#   MUSL_CC=<musl-gcc>  musl cross-compiler (defaults to PATH musl-gcc)
#
# Groundwork for a future native Aegis build: the only toolchain assumption is
# MUSL_CC; point it at an Aegis-native cc and the same sources build on-device.
MUSL_CC ?= musl-gcc
VERSION       := $(shell cat VERSION)
GLYPH_VERSION := $(shell cat GLYPH_VERSION)

CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall \
         -DAEGIS_VERSION=\"$(VERSION)\" -Itoolkit/include
SRCS = $(wildcard src/*.c)

all: package

# Pinned toolkit artifact (the kernel-style fetched build dependency).
toolkit/include/glyph.h:
	sh tools/fetch-glyph.sh $(GLYPH_VERSION) toolkit

component.elf: $(SRCS) toolkit/include/glyph.h
	$(MUSL_CC) $(CFLAGS) -o $@ $(SRCS) -Ltoolkit/lib -lcitadel -laudio -lauth -lglyph

package: component.elf
	sh tools/pack.sh

clean:
	rm -f component.elf *.hpkg *.hpkg.sig
	rm -rf toolkit
