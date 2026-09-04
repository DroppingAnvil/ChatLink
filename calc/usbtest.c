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
 * usbtest - can the PC reach the calculator over USB WHILE an Ndless program
 * is running? This decides the message-channel architecture.
 *
 * Both idle() and msleep() mask every IRQ except the timer, and
 * wait_key_pressed() spins on idle(), so a program waiting on a key starves the
 * USB link. The question is whether the OS still services USB in the gaps, when
 * this program is inside a file syscall with interrupts enabled.
 *
 * SELF-EVIDENCING: this writes an incrementing counter to a file every ~200ms.
 * The PC does not need to guess whether it read during or after the run - if it
 * reads the file and sees the number ADVANCING, the link was served while the
 * program was live. A single unchanging value means the PC only got through
 * after the program exited.
 *
 * Runs ~60 seconds then exits on its own. It will not trap you.
 */

#include <os.h>
#include <libndls.h>

#define BEAT_PATH "/documents/chatlink/beat.tns"
#define ITERATIONS 300          /* 300 * 200ms = ~60s */

/* Render an unsigned int as decimal without pulling in printf. */
static int u_to_str(unsigned value, char *out) {
    char tmp[12];
    int n = 0, len = 0;
    if (value == 0) tmp[n++] = '0';
    while (value > 0) { tmp[n++] = (char)('0' + (value % 10)); value /= 10; }
    while (n > 0) out[len++] = tmp[--n];
    return len;
}

int main(void) {
    int i;

    show_msgbox("ChatLink USB test",
                "Writes a counter every 200ms for ~60 seconds.\n\n"
                "Press OK, then leave it completely alone.\n\n"
                "The PC will watch the counter advance.");

    for (i = 0; i < ITERATIONS; ++i) {
        NUC_FILE *f = nuc_fopen(BEAT_PATH, "wb");
        if (f) {
            char buf[16];
            int len = u_to_str((unsigned)i, buf);
            buf[len++] = '\n';
            nuc_fwrite(buf, 1, (size_t)len, f);
            nuc_fclose(f);
        }
        msleep(200);
    }

    show_msgbox("ChatLink USB test", "Done. ~60s of counting finished.");
    return 0;
}
