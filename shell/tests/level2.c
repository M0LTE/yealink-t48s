// SPDX-License-Identifier: AGPL-3.0-or-later
/* An app as one written for API level 2 is: it speaks the protocol by hand,
 * answers READY with 2, and takes a KEY or TOUCH of any other size than level
 * 2's as an error (exit status 3). The 5 key turns the screen green, a touch
 * blue. For tests/15-flip.script: a newer shell must not change what an older
 * app is sent. */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "t48_proto.h"

static int sock;

static int get(void *p, size_t n)
{
    for (size_t at = 0; at < n; ) {
        ssize_t k = read(sock, (char *)p + at, n - at);
        if (k <= 0) return -1;
        at += (size_t)k;
    }
    return 0;
}

static void put(int type, const void *p, size_t n)
{
    struct t48_msg_hdr h = {(uint16_t)type, (uint16_t)n};
    if (write(sock, &h, sizeof h) < 0 || (n && write(sock, p, n) < 0)) exit(1);
}

int main(void)
{
    struct t48_msg_hdr h;
    struct t48_msg_hello hello = {0};
    unsigned char p[1024];
    uint32_t *px = NULL;

    sock = atoi(getenv(T48_ENV_SOCKET));
    while (get(&h, sizeof h) == 0) {
        if (h.len > sizeof p || get(p, h.len) < 0) return 1;
        if (h.type == T48_MSG_HELLO) {
            memcpy(&hello, p, sizeof hello);
            px = mmap(NULL, hello.size, PROT_READ | PROT_WRITE, MAP_SHARED, atoi(getenv(T48_ENV_BUFFER)), 0);
            if (px == MAP_FAILED) return 1;
        } else if (h.type == T48_MSG_START || h.type == T48_MSG_KEY || h.type == T48_MSG_TOUCH) {
            uint32_t c = 0x202020;
            if (h.type == T48_MSG_START) { struct t48_msg_ready r = {2}; put(T48_MSG_READY, &r, sizeof r); }
            if (h.type == T48_MSG_KEY && h.len != sizeof(struct t48_msg_key)) return 3;
            if (h.type == T48_MSG_TOUCH && h.len != sizeof(struct t48_msg_touch)) return 3;
            if (h.type == T48_MSG_KEY && p[0] == '5') c = 0x00ff00;
            if (h.type == T48_MSG_KEY && p[0] != '5' && p[0] >= 0x10) continue;   /* any other key: nothing; soft keys must not come */
            if (h.type == T48_MSG_KEY && (p[0] | p[1] << 8) >= T48_KEY_SOFT1) return 3;
            if (h.type == T48_MSG_TOUCH) c = 0x0000ff;
            for (unsigned i = 0; i < hello.size / 4; i++) px[i] = c;
            struct t48_msg_rect r = {0, 0, hello.width, hello.height};
            put(T48_MSG_DAMAGE, &r, sizeof r);
        } else if (h.type == T48_MSG_QUIT) {
            return 0;
        }
    }
    return 0;
}
