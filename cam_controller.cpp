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
  auto rpicamsrc = gst_element_factory_make("rpicamsrc", "rpicamsrc");
  if (!rpicamsrc) { g_warning("Failed to create rpicamsrc"); }
  {
    GValue preview = G_VALUE_INIT;
    g_value_init(&preview, G_TYPE_BOOLEAN);
    g_value_set_boolean(&preview, false);
    g_object_set_property(G_OBJECT(rpicamsrc), "preview", &preview);
  }

  auto rtph264pay = gst_element_factory_make("rtph264pay", "rtph264pay");
  if (!rtph264pay) { g_warning("Failed to create rtph264pay"); }
  {
    GValue send_config = G_VALUE_INIT;
    g_value_init(&send_config, G_TYPE_BOOLEAN);
    g_value_set_boolean(&send_config, TRUE);
    g_object_set_property(G_OBJECT(rtph264pay), "send-config", &send_config);
    GValue config_interval = G_VALUE_INIT;
    g_value_init(&config_interval, G_TYPE_INT);
    g_value_set_int(&config_interval, -1);
    g_object_set_property(G_OBJECT(rtph264pay), "config-interval", &config_interval);
    GValue pt = G_VALUE_INIT;
    g_value_init(&pt, G_TYPE_INT);
    g_value_set_int(&pt, 98);
    g_object_set_property(G_OBJECT(rtph264pay), "pt", &pt);
  }

  auto udpsink = gst_element_factory_make("udpsink", "udpsink");
  if (!udpsink) { g_warning("Failed to create udpsink"); }
  {
    GValue host = G_VALUE_INIT;
    g_value_init(&host, G_TYPE_STRING);
    g_value_set_string(&host, "192.168.16.152");
    g_object_set_property(G_OBJECT(udpsink), "host", &host);
    GValue port = G_VALUE_INIT;
    g_value_init(&port, G_TYPE_INT);
    g_value_set_int(&port, 5000);
    g_object_set_property(G_OBJECT(udpsink), "port", &port);
  }

  /* must add elements to pipeline before linking them */
  gst_bin_add_many(GST_BIN(pipeline), rpicamsrc, rtph264pay, udpsink, NULL);

  auto caps = gst_caps_new_simple
    ( "video/x-h264"
    //, "format", G_TYPE_STRING, "I420"
    , "width", G_TYPE_INT, 1280
    , "height", G_TYPE_INT, 720
    , "framerate", GST_TYPE_FRACTION, 30, 1
    , NULL
    );

  /* link */
  if (!gst_element_link_filtered(rpicamsrc, rtph264pay, caps)) { g_warning ("Failed to link rpicamsrc to rtph264pay"); }
  if (!gst_element_link(rtph264pay, udpsink)) { g_warning ("Failed to link rtph264pay to udpsink"); }

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
