#!/usr/bin/env bash
# Builds a ChatLink calculator program into a runnable .tns.
#
# Runs INSIDE the ndless-sdk container, which carries the Ndless toolchain.
# From Windows:
#   docker exec ndless-sdk bash -lc '/work/calc/build.sh linktest'
#
# Why the toolchain is built from source rather than apt-installed: Ndless's
# newlib is patched and configured for the calculator
# (--disable-newlib-supplied-syscalls, MALLOC_PROVIDED, PATH_MAX, ...). A stock
# arm-none-eabi toolchain links and packages without error but produces a broken
# binary - 1 relocation instead of 45 - which crashes the calculator before
# main() runs. Do not substitute it.

set -euo pipefail

PROG="${1:-linktest}"
SDK=/opt/Ndless/ndless-sdk
export PATH="$SDK/toolchain/install/bin:$SDK/bin:$PATH"

cd "$(dirname "${BASH_SOURCE[0]}")"

if [[ ! -f "$PROG.c" ]]; then
    echo "error: $PROG.c not found in $(pwd)" >&2
    exit 1
fi

echo "building $PROG.c"
rm -f "$PROG.o" "$PROG.elf" "$PROG.zehn" "$PROG.tns"

# -marm: the loader entry path expects ARM, not Thumb.
nspire-gcc -Wall -W -marm -Os -c "$PROG.c" -o "$PROG.o"

# nspire-ld is a shim over nspire-gcc; it pulls in libndls, libsyscalls and
# libnspireio from $SDK/lib automatically.
nspire-ld "$PROG.o" -o "$PROG.elf" -Wl,--gc-sections

# --uses-lcd-blit true: this matters on HW-W hardware revisions. Left at the
# default (false), the loader assumes the program draws to the legacy
# SCREEN_BASE_ADDRESS framebuffer and switches the LCD into compatibility mode,
# which corrupts the display for programs that only use OS drawing (message
# boxes). ChatLink programs never touch the framebuffer directly, so the LCD
# should stay in its native mode.
# --240x320-support true: declares the program is fine on portrait CX II panels.
genzehn --input "$PROG.elf" --output "$PROG.zehn" --name "$PROG"         --uses-lcd-blit true --240x320-support true
make-prg "$PROG.zehn" "$PROG.tns" 2>/dev/null || make-prg "$PROG.zehn" "$PROG.tns"

rm -f "$PROG.zehn"

echo
echo "built: $(pwd)/$PROG.tns ($(stat -c%s "$PROG.tns") bytes)"
echo "sanity check - a healthy build has dozens of relocations, not 1:"
genzehn --info --input <(tail -c +493 "$PROG.tns") 2>/dev/null | grep -i reloc || true
