#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#include <stdio.h>
extern int game_running(void); /* your frame loop; the game paces it */
int main(void) {
    rbl_config cfg; rbl_link* link = NULL; rbl_event ev[64];
    rbl_config_init(&cfg, "My Game");                  /* listens on 127.0.0.1:47800 */
    if (rbl_link_host(&cfg, &link) != RBL_OK) return 1;
    while (game_running()) {
        int i, n = rbl_link_poll(link, ev, 64);         /* never blocks; sends the heartbeats */
        for (i = 0; i < n; i++)
            if (ev[i].type == RBL_EV_POSES) printf("%u poses\n", ev[i].u.poses.count);
        if (rbl_link_is_connected(link)) {              /* once per rendered frame */
            rbl_camera cam; const float eye[3] = {0, 2, 8}, at[3] = {0, 1, 0};
            rbl_camera_look_at(&cam, eye, at, 1.0f, 0.1f, 1000.0f, 1280, 720);
            rbl_link_send_camera(link, &cam, NULL);
        }
    }
    rbl_link_close(link);
}
