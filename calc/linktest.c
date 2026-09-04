/*
 * ChatLink - link a TI-Nspire CX to a PC over USB.
 * Copyright (C) 2026 Christopher Willett / AnvilDevelopment.US
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. See the LICENSE file for the full text.
 */

/*
 * linktest - validates the Ndless toolchain and the two calculator-side
 * primitives ChatLink depends on: plain-file I/O and keyboard input.
 *
 * Deliberately STAGED. Each step announces itself in a message box before doing
 * anything risky, so a crash identifies the stage that caused it instead of
 * just failing silently. The first version of this program crashed the
 * calculator and gave us nothing to go on.
 *
 * File I/O goes through nuc_fopen / nuc_fwrite / nuc_fclose: syscalls into the
 * calculator OS's own Nucleus C library, not newlib. Combined with the shims in
 * stock_toolchain_compat.c, that lets this build against a stock arm-none-eabi
 * toolchain with no libc linked at all.
 */

#include <os.h>
#include <libndls.h>

/* Paths here are as the calculator sees them. Over the USB link the filesystem
 * root IS the documents folder, so the link's "/chatlink" is
 * "/documents/chatlink" to code running on the calculator. */
#define CHATLINK_DIR "/documents/chatlink"
#define TEST_PATH    CHATLINK_DIR "/handshake.txt.tns"

int main(void) {
    static const char payload[] = "hello from the calculator";
    const size_t payload_len = sizeof(payload) - 1;   /* no trailing NUL */
    static char readback[64];
    NUC_FILE *file;
    size_t written, got;

    /* Stage 1: proves crt0, the Ndless loader and libndls all work, and that
     * the compat shims (strlen/memset/memcpy, used by _show_msgbox) are sane.
     * If the calculator dies before this box appears, the fault is in startup
     * or the shims, not in file I/O. */
    show_msgbox("linktest 1/5", "Runtime OK. Next: open a file for writing.");

    file = nuc_fopen(TEST_PATH, "wb");
    if (!file) {
        show_msgbox("linktest 2/5 FAILED",
                    "nuc_fopen returned NULL for writing.\n"
                    "The chatlink folder may not exist.");
        return 1;
    }
    show_msgbox("linktest 2/5", "Opened for writing. Next: write 25 bytes.");

    written = nuc_fwrite((void *)payload, 1, payload_len, file);
    nuc_fclose(file);
    if (written != payload_len) {
        show_msgbox("linktest 3/5 FAILED", "Short write.");
        return 1;
    }
    show_msgbox("linktest 3/5", "Wrote and closed. Next: reopen and read back.");

    file = nuc_fopen(TEST_PATH, "rb");
    if (!file) {
        show_msgbox("linktest 4/5 FAILED", "Wrote the file but cannot reopen it.");
        return 1;
    }
    memset(readback, 0, sizeof(readback));
    got = nuc_fread(readback, 1, payload_len, file);
    nuc_fclose(file);

    if (got != payload_len || memcmp(readback, payload, payload_len) != 0) {
        show_msgbox("linktest 4/5 FAILED", "Read back the wrong bytes.");
        return 1;
    }
    show_msgbox("linktest 4/5", "File I/O verified. Next: press any key.");

    wait_no_key_pressed();
    wait_key_pressed();
    wait_no_key_pressed();

    show_msgbox("linktest 5/5", "Keyboard works. Toolchain is good.");
    return 0;
}
