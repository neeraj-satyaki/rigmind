/* Test-only RTSP camera: serves a live H.264 test pattern on rtsp://127.0.0.1:<port>/<mount>.
 * Build: gcc rtsp_test_server.c -o rtsp_test_server $(pkg-config --cflags --libs gstreamer-rtsp-server-1.0)
 * Run:   ./rtsp_test_server 8554 live
 */
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    const char* port = argc > 1 ? argv[1] : "8554";
    const char* mount = argc > 2 ? argv[2] : "live";

    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server, port);
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    /* Test pattern, 640x360 @ 15 fps, H.264 baseline, keyframe every second. */
    gst_rtsp_media_factory_set_launch(factory,
        "( videotestsrc is-live=true pattern=ball ! video/x-raw,width=640,height=360,framerate=15/1 "
        "! x264enc tune=zerolatency key-int-max=15 bitrate=800 ! rtph264pay name=pay0 pt=96 )");
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    char path[256];
    snprintf(path, sizeof path, "/%s", mount);
    gst_rtsp_mount_points_add_factory(mounts, path, factory);
    g_object_unref(mounts);

    if (gst_rtsp_server_attach(server, NULL) == 0) {
        fprintf(stderr, "cannot bind RTSP port %s\n", port);
        return 1;
    }
    fprintf(stderr, "rtsp://127.0.0.1:%s/%s\n", port, mount);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
