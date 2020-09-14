// We use some GNU extensions (basename)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <memory.h>
#include <sysexits.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include <optional>

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>

#include <semaphore.h>

#include <stdbool.h>

int main(int argc, char **argv) {
  // init
  gst_init (&argc, &argv);

  auto loop = g_main_loop_new(nullptr, FALSE);

  // create pipeline
  auto pipeline = gst_pipeline_new("my-pipeline");

  // create elements
  auto picam = gst_element_factory_make("picam", "picam");
  if (!picam) { g_warning("Failed to create picam"); }

  auto rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
  if (!rtpbin) { g_warning("Failed to create rtpbin"); }
  auto send_rtp_sink_0 = gst_element_get_request_pad(rtpbin, "send_rtp_sink_0");
  if (!send_rtp_sink_0) { g_warning("Failed to create send_rtp_sink_0"); }

  auto udpsink = gst_element_factory_make("udpsink", "udpsink");
  if (!udpsink) { g_warning("Failed to create udpsink"); }

  {
    GValue host = G_VALUE_INIT;
    g_value_init(&host, G_TYPE_STRING);
    g_value_set_string(&host, "192.168.16.72");
    g_object_set_property(G_OBJECT(udpsink), "host", &host);
  }

  /* must add elements to pipeline before linking them */
  gst_bin_add_many(GST_BIN(pipeline), picam, rtpbin, udpsink, NULL);

  /* link */
  if (!gst_pad_link(GST_BASE_SRC_PAD(picam), send_rtp_sink_0)) { g_warning ("Failed to link picam to rtpbin"); }
  if (!gst_element_link_pads(rtpbin, "send_rtp_src_0", udpsink, "sink")) { g_warning ("Failed to link rtpbin to udpsink"); }

  if (!gst_element_set_state(pipeline, GST_STATE_PLAYING)) { g_warning("Failed to start pipeline"); }

  // Iterate
  g_print("Running...\n");
  g_main_loop_run(loop);

  // Out of the main loop, clean up nicely
  g_print("Returned, stopping playback\n");
  gst_element_set_state(pipeline, GST_STATE_NULL);

  g_print("Deleting pipeline\n");
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref (loop);
}
