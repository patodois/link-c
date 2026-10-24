#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
extern int game_running(void); /* your frame loop; the game paces it */
int main(void) {
    rbl_config cfg; rbl_link* link = NULL; rbl_event ev[64];
    rbl_config_init(&cfg, "My Rewrite");               /* connects to 127.0.0.1:47800, retrying */
    if (rbl_link_guest(&cfg, &link) != RBL_OK) return 1;
    while (game_running()) {
        int i, n = rbl_link_poll(link, ev, 64);
        for (i = 0; i < n; i++) {
            if (ev[i].type == RBL_EV_CONNECTED) rbl_link_spawn(link, 1, "My Rewrite", "demo/cube", "Cube");
            if (ev[i].type == RBL_EV_CAMERA) {          /* the host's view, once per frame */
                rbl_pose pose; rbl_pose_identity(&pose, 1, rbl_now_us());
                pose.translation[1] = 1.0f;             /* metres, +Y up, in the host's world */
                rbl_link_send_poses(link, &pose, 1);
            }
        }
    }
    rbl_link_close(link);
}
