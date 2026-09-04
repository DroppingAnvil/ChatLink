/*
 * chatlink - the calculator side of the link.
 *
 * ONE TURN PER LAUNCH:
 *   1. if a reply to the previous request is waiting, show it
 *   2. prompt for a new message
 *   3. write it to req.tns
 *   4. exit immediately
 *
 * WHY IT EXITS INSTEAD OF WAITING
 * -------------------------------
 * A resident Ndless program blocks the USB link - observed repeatedly: the PC
 * could only read req.tns once the program was escaped. idle() and msleep()
 * mask every IRQ except the timer, so waiting on the calculator starves the
 * very link the PC needs to deliver the reply. Exiting between turns keeps the
 * calculator idle exactly when the PC needs it, and means this program can
 * never appear hung. The cost is one relaunch per turn.
 *
 * WHY ITS OWN CONSOLE INSTEAD OF THE OS DIALOG
 * --------------------------------------------
 * show_msg_user_input() drives the OS's dialog machinery, which on OS 4.2
 * interrupts typing with a Document Settings popup. nspireio gives us a plain
 * text console we fully control, so nothing else can steal the keyboard. It
 * draws through lcd_init/lcd_blit - the modern API - so it needs no LCD
 * compatibility mode and does not corrupt the display on HW-W hardware.
 *
 * Framing matches src/link/protocol.h:
 *     CHATLINK/1 <seq> <byte-length>\n<payload>
 */

#include <os.h>
#include <libndls.h>
#include <nspireio/nspireio.h>

#define CHATLINK_DIR "/documents/chatlink"
#define REQ_PATH     CHATLINK_DIR "/req.tns"
#define RSP_PATH     CHATLINK_DIR "/rsp.tns"
#define MAGIC        "CHATLINK/1"

#define MAX_PROMPT   512
#define MAX_PAYLOAD  16384

static char g_request[MAX_PROMPT + 128];
static char g_raw[MAX_PAYLOAD + 128];
static char g_payload[MAX_PAYLOAD + 1];
static char g_input[MAX_PROMPT + 2];

static nio_console g_console;

/* ---------- helpers -------------------------------------------------------- */

static int u_to_str(unsigned v, char *out) {
    char tmp[12];
    int n = 0, len = 0;
    if (v == 0) tmp[n++] = (char)48;
    while (v > 0) { tmp[n++] = (char)(48 + (v % 10)); v /= 10; }
    while (n > 0) out[len++] = tmp[--n];
    return len;
}

static int str_to_u(const char *s, int max, unsigned *out) {
    int i = 0;
    unsigned v = 0;
    while (i < max && s[i] >= 48 && s[i] <= 57) {
        v = v * 10 + (unsigned)(s[i] - 48);
        ++i;
    }
    *out = v;
    return i;
}

static int read_file(const char *path, char *buf, int max) {
    NUC_FILE *f = nuc_fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = nuc_fread(buf, 1, (size_t)max, f);
    nuc_fclose(f);
    return (int)got;
}

static int write_file(const char *path, const char *data, int len) {
    NUC_FILE *f = nuc_fopen(path, "wb");
    size_t put;
    if (!f) return -1;
    put = nuc_fwrite((void *)data, 1, (size_t)len, f);
    nuc_fclose(f);
    return (put == (size_t)len) ? 0 : -1;
}

/* ---------- framing -------------------------------------------------------- */

static int encode(unsigned seq, const char *payload, int payload_len,
                  char *out, int out_max) {
    int n = 0, i;
    const int magic_len = (int)strlen(MAGIC);
    if (magic_len + 32 + payload_len > out_max) return -1;

    memcpy(out + n, MAGIC, (size_t)magic_len); n += magic_len;
    out[n++] = (char)32;
    n += u_to_str(seq, out + n);
    out[n++] = (char)32;
    n += u_to_str((unsigned)payload_len, out + n);
    out[n++] = (char)10;
    for (i = 0; i < payload_len; ++i) out[n++] = payload[i];
    return n;
}

/* 1 = complete message, 0 = not (yet) whole, -1 = not ours. */
static int decode(const char *raw, int raw_len, unsigned *seq,
                  char *payload, int payload_max) {
    const int magic_len = (int)strlen(MAGIC);
    int pos, used;
    unsigned length = 0;

    if (raw_len <= 0) return 0;
    if (raw_len < magic_len)
        return (memcmp(raw, MAGIC, (size_t)raw_len) == 0) ? 0 : -1;
    if (memcmp(raw, MAGIC, (size_t)magic_len) != 0) return -1;

    pos = magic_len;
    if (pos >= raw_len) return 0;
    if (raw[pos] != (char)32) return -1;
    ++pos;

    used = str_to_u(raw + pos, raw_len - pos, seq);
    if (used == 0) return (pos >= raw_len) ? 0 : -1;
    pos += used;
    if (pos >= raw_len) return 0;
    if (raw[pos] != (char)32) return -1;
    ++pos;

    used = str_to_u(raw + pos, raw_len - pos, &length);
    if (used == 0) return (pos >= raw_len) ? 0 : -1;
    pos += used;
    if (pos >= raw_len) return 0;
    if (raw[pos] != (char)10) return -1;
    ++pos;

    if ((int)length > payload_max) return -1;
    if (raw_len - pos < (int)length) return 0;

    memcpy(payload, raw + pos, (size_t)length);
    payload[length] = (char)0;
    return 1;
}

/* ---------- main ----------------------------------------------------------- */

int main(void) {
    unsigned req_seq = 0, rsp_seq = 0;
    int have_reply = 0, raw_len, input_len, encoded_len, i;

    /* What did we ask last time? */
    raw_len = read_file(REQ_PATH, g_raw, MAX_PAYLOAD);
    if (raw_len > 0) {
        unsigned s = 0;
        if (decode(g_raw, raw_len, &s, g_payload, MAX_PAYLOAD) == 1) req_seq = s;
    }

    /* Has the PC answered it? The reply carries the same sequence number. */
    if (req_seq > 0) {
        raw_len = read_file(RSP_PATH, g_raw, MAX_PAYLOAD);
        if (raw_len > 0
                && decode(g_raw, raw_len, &rsp_seq, g_payload, MAX_PAYLOAD) == 1
                && rsp_seq == req_seq) {
            have_reply = 1;
        }
    }

    if (!nio_init(&g_console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0,
                  NIO_COLOR_BLACK, NIO_COLOR_WHITE, true)) {
        show_msgbox("ChatLink", "Could not create the console.");
        return 1;
    }
    nio_set_default(&g_console);
    nio_clear(&g_console);

    nio_printf("ChatLink\n");
    nio_printf("--------\n\n");

    if (have_reply) {
        nio_printf("Reply to #%u:\n\n%s\n\n", rsp_seq, g_payload);
    } else if (req_seq > 0) {
        nio_printf("No reply yet for #%u.\n"
                   "Leave the calculator idle a moment so the PC can read it,\n"
                   "then run ChatLink again.\n\n", req_seq);
    } else {
        nio_printf("No messages yet.\n\n");
    }

    nio_printf("Message (empty to quit):\n> ");
    nio_fflush(&g_console);

    g_input[0] = (char)0;
    nio_fgets(g_input, (int)sizeof(g_input), &g_console);

    /* Strip the trailing newline nio_fgets leaves in place. */
    input_len = (int)strlen(g_input);
    while (input_len > 0
           && (g_input[input_len - 1] == (char)10 || g_input[input_len - 1] == (char)13)) {
        g_input[--input_len] = (char)0;
    }

    /* Blank line means quit. */
    for (i = 0; i < input_len; ++i) {
        if (g_input[i] != (char)32) break;
    }
    if (input_len == 0 || i == input_len) {
        nio_free(&g_console);
        return 0;
    }

    encoded_len = encode(req_seq + 1, g_input, input_len,
                         g_request, (int)sizeof(g_request));
    if (encoded_len < 0) {
        nio_printf("\nMessage too long.\n");
        nio_fflush(&g_console);
        msleep(1500);
        nio_free(&g_console);
        return 1;
    }

    if (write_file(REQ_PATH, g_request, encoded_len) != 0) {
        nio_printf("\nCould not write req.tns.\nDoes the chatlink folder exist?\n");
        nio_fflush(&g_console);
        msleep(2500);
        nio_free(&g_console);
        return 1;
    }

    nio_printf("\nSent as #%u. Exiting so the PC can read it.\n"
               "Run ChatLink again in a few seconds for the reply.\n",
               req_seq + 1);
    nio_fflush(&g_console);

    /* Brief pause so the confirmation is readable, then get out of the way:
     * the PC cannot reach the link while this program is resident. */
    msleep(1500);
    nio_free(&g_console);
    return 0;
}
