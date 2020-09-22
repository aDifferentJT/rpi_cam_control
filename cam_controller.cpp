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

#include <iostream>
#include <optional>

extern "C" {
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
}

#include "raspicam.hpp"

// GstAppSrc callbacks

void need_data(GstAppSrc *src, guint length, gpointer user_data) {
  //g_warning("need_data");
}

void enough_data(GstAppSrc *src, gpointer user_data) {
  //g_warning("enough_data");
}

int main(int argc, char **argv) {
  // init
  gst_init (&argc, &argv);

  auto loop = g_main_loop_new(nullptr, FALSE);

  // create pipeline
  auto pipeline = gst_pipeline_new("my-pipeline");

  // create elements
  auto appsrc = gst_element_factory_make("appsrc", "appsrc");
  if (!appsrc) { g_warning("Failed to create appsrc"); }

  auto raspicam = RaspiCam
    { [&] (auto buffer, auto length) {
        auto gst_buffer = gst_buffer_new_allocate(nullptr, length, nullptr);
        auto map_info = GstMapInfo{};
        gst_buffer_map(gst_buffer, &map_info, GST_MAP_WRITE);
        memcpy(map_info.data, buffer, length);
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), gst_buffer);
      }
    };

  GstAppSrcCallbacks callbacks =
    { &need_data
    , &enough_data
    , nullptr // seek_data
    };

  gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
  gst_app_src_set_callbacks
    ( GST_APP_SRC(appsrc)
    , &callbacks
    , nullptr
    , nullptr //GDestroyNotify notify
    );

  auto h264parse = gst_element_factory_make("h264parse", "h264parse");
  if (!h264parse) { g_warning("Failed to create h264parse"); }

  auto rtph264pay = gst_element_factory_make("rtph264pay", "rtph264pay");
  if (!rtph264pay) { g_warning("Failed to create rtph264pay"); }
  auto rtph264pay_src = gst_element_get_static_pad (rtph264pay, "src");

  auto rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
  if (!rtpbin) { g_warning("Failed to create rtpbin"); }
  auto send_rtp_sink_0 = gst_element_get_request_pad(rtpbin, "send_rtp_sink_0");
  if (!send_rtp_sink_0) { g_warning("Failed to create send_rtp_sink_0"); }

  auto udpsink = gst_element_factory_make("udpsink", "udpsink");
  if (!udpsink) { g_warning("Failed to create udpsink"); }
  {
    GValue host = G_VALUE_INIT;
    g_value_init(&host, G_TYPE_STRING);
    g_value_set_string(&host, "192.168.16.84");
    g_object_set_property(G_OBJECT(udpsink), "host", &host);
    GValue port = G_VALUE_INIT;
    g_value_init(&port, G_TYPE_INT);
    g_value_set_int(&port, 5000);
    g_object_set_property(G_OBJECT(udpsink), "port", &port);
  }

  /* must add elements to pipeline before linking them */
  gst_bin_add_many(GST_BIN(pipeline), appsrc, h264parse, rtph264pay, rtpbin, udpsink, NULL);

  /* link */
  if (!gst_element_link(appsrc, h264parse)) { g_warning ("Failed to link appsrc to h264parse"); }
  if (!gst_element_link(h264parse, rtph264pay)) { g_warning ("Failed to link h264parse to rtph264pay"); }
  if (gst_pad_link(rtph264pay_src, send_rtp_sink_0) != GST_PAD_LINK_OK) { g_warning ("Failed to link rtph264pay to rtpbin"); }
  if (!gst_element_link_pads(rtpbin, "send_rtp_src_0", udpsink, "sink")) { g_warning ("Failed to link rtpbin to udpsink"); }

  if (!gst_element_set_state(pipeline, GST_STATE_PLAYING)) { g_warning("Failed to start pipeline"); }

  GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline");

  // Iterate
  g_print("Starting...\n");
  raspicam.start();

  g_print("Running...\n");
  g_main_loop_run(loop);

  // Out of the main loop, clean up nicely
  g_print("Returned, stopping playback\n");
  gst_element_set_state(pipeline, GST_STATE_NULL);

  g_print("Deleting pipeline\n");
  gst_object_unref(GST_OBJECT(pipeline));
  g_main_loop_unref (loop);
}
