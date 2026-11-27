/* lab-server: headless WUP Stream endpoint for the isolated NAT lab.
 *
 * Wraps the real patched libwupstream (the same code path the daemon uses)
 * and streams generated black frames + silence so fake_pad.py can verify
 * B/H/I/V/A/N/T handling through network namespaces and nftables NAT.
 *
 * usage: lab-server <port> [pad_ip]
 *   port    UDP port to listen on (the client's discovery/dial port)
 *   pad_ip  optional direct-pad mode (server pushes to pad_ip:50000)
 *
 * Re-opens the session automatically when the peer disappears so NAT
 * mapping expiry / reconnect scenarios can be exercised.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wupstream.h"

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 50001;
    const char *pad_ip = argc > 2 ? argv[2] : NULL;
    static unsigned char px[WUPSTREAM_W * WUPSTREAM_H * 4];
    static int16_t silence[800 * 2];
    memset(px, 0x18, sizeof px);
    for (;;) {
        wupstream *w = wupstream_open_port(pad_ip, port, NULL, 120000);
        if (!w) { fprintf(stderr, "lab-server: open failed\n"); return 1; }
        fprintf(stderr, "lab-server: session up on :%d\n", port);
        wupstream_input_t in;
        int reqid, tmode;
        for (;;) {
            if (wupstream_input(w, &in)) break;
            if (wupstream_take_control(w, &reqid, &tmode))
                wupstream_control_reply(w, reqid, 0, 1);   /* accept: effective Desktop mode */
            if (wupstream_frame(w, px, WUPSTREAM_W * 4, WUPSTREAM_RGBA)) break;
            if (wupstream_audio(w, silence, 800)) break;
        }
        wupstream_close(w);
        fprintf(stderr, "lab-server: peer gone, re-listening\n");
    }
}
