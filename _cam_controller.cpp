/*
Copyright (c) 2018, Raspberry Pi (Trading) Ltd.
Copyright (c) 2013, Broadcom Europe Ltd.
Copyright (c) 2013, James Hughes
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file RaspiVid.c
 * Command line program to capture a camera video stream and encode it to file.
 * Also optionally display a preview/viewfinder of current camera input.
 *
 * Description
 *
 * 3 components are created; camera, preview and video encoder.
 * Camera component has three ports, preview, video and stills.
 * This program connects preview and video to the preview and video
 * encoder. Using mmal we don't need to worry about buffers between these
 * components, but we do need to handle buffers from the encoder, which
 * are simply written straight to the file in the requisite buffer callback.
 *
 * If raw option is selected, a video splitter component is connected between
 * camera and preview. This allows us to set up callback for raw camera data
 * (in YUV420 or RGB format) which might be useful for further image processing.
 *
 * We use the RaspiCamControl code to handle the specific camera settings.
 * We use the RaspiPreview code to handle the (generic) preview window
 */

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

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#include "RaspiCommonSettings.hpp"
#include "RaspiCamControl.hpp"
#include "RaspiCLI.hpp"
#include "RaspiHelpers.hpp"

#include <semaphore.h>

#include <stdbool.h>


// Forward
struct RASPIVID_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
struct PORT_USERDATA {
   RASPIVID_STATE *pstate = nullptr;  /// pointer to our state in case required in callback
   int abort = 0;                     /// Set to 1 in callback if an error occurs to attempt to abort the capture
   char header_bytes[29];
   int header_wptr = 0;
   //ost::RTPSession rtp_session{ost::InetHostAddress{"0.0.0.0"}};
   uint64_t initial_time = get_microseconds64();
};

/** Structure containing all state information for the current run
 */
struct RASPIVID_STATE {
   RASPICOMMONSETTINGS_PARAMETERS common_settings{};     /// Common settings
   MMAL_FOURCC_T encoding = MMAL_ENCODING_H264;             /// Requested codec video encoding (MJPEG or H264)

   PORT_USERDATA callback_data{};        /// Used to move data to the encoder callback

   int frame = 0;
   int64_t starttime = 0;
   int64_t lasttime = 0;

   int slices = 1;
};


/// Structure to cross reference H264 profile strings against the MMAL parameter equivalent
static auto profile_map = std::array
  { XREF_T{"baseline", MMAL_VIDEO_PROFILE_H264_BASELINE}
  , XREF_T{"main",     MMAL_VIDEO_PROFILE_H264_MAIN}
  , XREF_T{"high",     MMAL_VIDEO_PROFILE_H264_HIGH}
  };

static int profile_map_size = sizeof(profile_map) / sizeof(profile_map[0]);

/// Structure to cross reference H264 level strings against the MMAL parameter equivalent
static auto level_map = std::array
  { XREF_T{"4",   MMAL_VIDEO_LEVEL_H264_4}
  , XREF_T{"4.1", MMAL_VIDEO_LEVEL_H264_41}
  , XREF_T{"4.2", MMAL_VIDEO_LEVEL_H264_42}
  };

static int level_map_size = sizeof(level_map) / sizeof(level_map[0]);

static auto intra_refresh_map = std::array
  { XREF_T{"cyclic",       MMAL_VIDEO_INTRA_REFRESH_CYCLIC}
  , XREF_T{"adaptive",     MMAL_VIDEO_INTRA_REFRESH_ADAPTIVE}
  , XREF_T{"both",         MMAL_VIDEO_INTRA_REFRESH_BOTH}
  , XREF_T{"cyclicrows",   MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS}
  };

static int intra_refresh_map_size = sizeof(intra_refresh_map) / sizeof(intra_refresh_map[0]);

/// Command ID's and Structure defining our command line options
enum
{
   CommandBitrate,
   CommandFramerate,
   CommandIntraPeriod,
   CommandProfile,
   CommandQP,
   CommandInlineHeaders,
   CommandIntraRefreshType,
   CommandCodec,
   CommandLevel,
   CommandSPSTimings,
   CommandSlices
};

static COMMAND_LIST cmdline_commands[] =
{
   { CommandBitrate,       "-bitrate",    "b",  "Set bitrate. Use bits per second (e.g. 10MBits/s would be -b 10000000)", 1 },
   { CommandFramerate,     "-framerate",  "fps","Specify the frames per second to record", 1},
   { CommandIntraPeriod,   "-intra",      "g",  "Specify the intra refresh period (key frame rate/GoP size). Zero to produce an initial I-frame and then just P-frames.", 1},
   { CommandProfile,       "-profile",    "pf", "Specify H264 profile to use for encoding", 1},
   { CommandQP,            "-qp",         "qp", "Quantisation parameter. Use approximately 10-40. Default 0 (off)", 1},
   { CommandInlineHeaders, "-inline",     "ih", "Insert inline headers (SPS, PPS) to stream", 0},
   { CommandIntraRefreshType,"-irefresh", "if", "Set intra refresh type", 1},
   { CommandCodec,         "-codec",      "cd", "Specify the codec to use - H264 (default) or MJPEG", 1 },
   { CommandLevel,         "-level",      "lev","Specify H264 level to use for encoding", 1},
   { CommandSPSTimings,    "-spstimings",    "stm", "Add in h.264 sps timings", 0},
   { CommandSlices   ,     "-slices",     "sl", "Horizontal slices per frame. Default 1 (off)", 1},
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);


/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPIVID_STATE *state) {
   if (!state) {
      vcos_assert(0);
      return;
   }
}

static void check_camera_model(int cam_num) {
   MMAL_COMPONENT_T *camera_info;
   MMAL_STATUS_T status;

   // Try to get the camera name
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
   if (status == MMAL_SUCCESS)
   {
      MMAL_PARAMETER_CAMERA_INFO_T param;
      param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
      param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
      status = mmal_port_parameter_get(camera_info->control, &param.hdr);

      if (status != MMAL_SUCCESS)
      {
         // Running on newer firmware
         param.hdr.size = sizeof(param);
         status = mmal_port_parameter_get(camera_info->control, &param.hdr);
         if (status == MMAL_SUCCESS && param.num_cameras > cam_num)
         {
            if (!strncmp(param.cameras[cam_num].camera_name, "toshh2c", 7))
            {
               fprintf(stderr, "The driver for the TC358743 HDMI to CSI2 chip you are using is NOT supported.\n");
               fprintf(stderr, "They were written for a demo purposes only, and are in the firmware on an as-is\n");
               fprintf(stderr, "basis and therefore requests for support or changes will not be acted on.\n\n");
            }
         }
      }

      mmal_component_destroy(camera_info);
   }
}

/**
 * Dump image state parameters to stderr.
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void dump_status(RASPIVID_STATE *state)
{
   if (!state)
   {
      vcos_assert(0);
      return;
   }

   raspicommonsettings_dump_parameters(&state->common_settings);

   fprintf(stderr, "bitrate %d, framerate %d\n", state->bitrate, state->framerate);
   fprintf(stderr, "H264 Profile %s\n", raspicli_unmap_xref(state->profile, profile_map));
   fprintf(stderr, "H264 Level %s\n", raspicli_unmap_xref(state->level, level_map));
   fprintf(stderr, "H264 Quantisation level %d, Inline headers %s\n", state->quantisationParameter, state->bInlineHeaders ? "Yes" : "No");
   fprintf(stderr, "H264 Fill SPS Timings %s\n", state->addSPSTiming ? "Yes" : "No");
   if (state->intra_refresh_type) {
     fprintf(stderr, "H264 Intra refresh type %s, period %d\n", raspicli_unmap_xref(*state->intra_refresh_type, intra_refresh_map), state->intraperiod);
   } else {
     fprintf(stderr, "H264 Intra refresh type not set, period %d\n", state->intraperiod);
   }
   fprintf(stderr, "H264 Slices %d\n", state->slices);

   fprintf(stderr, "\n\n");

   raspicamcontrol_dump_parameters(&state->camera_parameters);
}

/**
 * Display usage information for the application to stdout
 *
 * @param app_name String to display as the application name
 */
static void application_help_message(char const *app_name)
{
   int i;

   fprintf(stdout, "Display camera output to display, and optionally saves an H264 capture at requested bitrate\n\n");
   fprintf(stdout, "\nusage: %s [options]\n\n", app_name);

   fprintf(stdout, "Image parameter commands\n\n");

   raspicli_display_help(cmdline_commands, cmdline_commands_size);

   // Profile options
   fprintf(stdout, "\n\nH264 Profile options :\n%s", profile_map[0].mode );

   for (i=1; i<profile_map_size; i++)
   {
      fprintf(stdout, ",%s", profile_map[i].mode);
   }

   // Level options
   fprintf(stdout, "\n\nH264 Level options :\n%s", level_map[0].mode );

   for (i=1; i<level_map_size; i++)
   {
      fprintf(stdout, ",%s", level_map[i].mode);
   }

   // Intra refresh options
   fprintf(stdout, "\n\nH264 Intra refresh options :\n%s", intra_refresh_map[0].mode );

   for (i=1; i<intra_refresh_map_size; i++)
   {
      fprintf(stdout, ",%s", intra_refresh_map[i].mode);
   }

   fprintf(stdout, "\n\n");

   fprintf(stdout, "Raspivid allows output to a remote IPv4 host e.g. -o tcp://192.168.1.2:1234"
           "or -o udp://192.168.1.2:1234\n"
           "To listen on a TCP port (IPv4) and wait for an incoming connection use the -l option\n"
           "e.g. raspivid -l -o tcp://0.0.0.0:3333 -> bind to all network interfaces,\n"
           "raspivid -l -o tcp://192.168.1.1:3333 -> bind to a certain local IPv4 port\n");

   return;
}

/**
 * Parse the incoming command line and put resulting parameters in to the state
 *
 * @param argc Number of arguments in command line
 * @param argv Array of pointers to strings from command line
 * @param state Pointer to state structure to assign any discovered parameters to
 * @return Non-0 if failed for some reason, 0 otherwise
 */
static int parse_cmdline(int argc, char **argv, RASPIVID_STATE *state)
{
   // Parse the command line arguments.
   // We are looking for --<something> or -<abbreviation of something>

   int valid = 1;
   int i;

   for (i = 1; i < argc && valid; i++) {
      int command_id, num_parameters;

      if (!argv[i])
         continue;

      if (argv[i][0] != '-')
      {
         valid = 0;
         continue;
      }

      // Assume parameter is valid until proven otherwise
      valid = 1;

      command_id = raspicli_get_command_id(cmdline_commands, cmdline_commands_size, &argv[i][1], &num_parameters);

      // If we found a command but are missing a parameter, continue (and we will drop out of the loop)
      if (command_id != -1 && num_parameters > 0 && (i + 1 >= argc) )
         continue;

      //  We are now dealing with a command line option
      switch (command_id)
      {
      case CommandBitrate: // 1-100
         if (sscanf(argv[i + 1], "%u", &state->bitrate) == 1)
         {
            i++;
         }
         else
            valid = 0;

         break;

      case CommandFramerate: // fps to record
      {
         if (sscanf(argv[i + 1], "%u", &state->framerate) == 1) {
            // TODO : What limits do we need for fps 1 - 30 - 120??
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandIntraPeriod: // key frame rate
      {
         if (sscanf(argv[i + 1], "%u", &state->intraperiod) == 1)
            i++;
         else
            valid = 0;
         break;
      }

      case CommandQP: // quantisation parameter
      {
         if (sscanf(argv[i + 1], "%u", &state->quantisationParameter) == 1)
            i++;
         else
            valid = 0;
         break;
      }

      case CommandProfile: // H264 profile
         state->profile = raspicli_map_xref(argv[i + 1], profile_map).value_or(MMAL_VIDEO_PROFILE_H264_HIGH);
         i++;
         break;

      case CommandInlineHeaders: // H264 inline headers
         state->bInlineHeaders = 1;
         break;

      case CommandIntraRefreshType:
         state->intra_refresh_type = raspicli_map_xref(argv[i + 1], intra_refresh_map);
         i++;
         break;

      case CommandCodec:  // codec type
      {
         int len = strlen(argv[i + 1]);
         if (len)
         {
            if (len==4 && !strncmp("H264", argv[i+1], 4))
               state->encoding = MMAL_ENCODING_H264;
            else  if (len==5 && !strncmp("MJPEG", argv[i+1], 5))
               state->encoding = MMAL_ENCODING_MJPEG;
            else
               valid = 0;
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandLevel: // H264 level
         state->level = raspicli_map_xref(argv[i + 1], level_map).value_or(MMAL_VIDEO_LEVEL_H264_4);
         i++;
         break;

      case CommandSlices:
         if ((sscanf(argv[i + 1], "%d", &state->slices) == 1) && (state->slices > 0)) {
            i++;
         } else {
            valid = 0;
         }
         break;

      case CommandSPSTimings:
         state->addSPSTiming = MMAL_TRUE;
         break;

      default:
      {
         // Try parsing for any image specific parameters
         // result indicates how many parameters were used up, 0,1,2
         // but we adjust by -1 as we have used one already
         char const *second_arg = (i + 1 < argc) ? argv[i + 1] : NULL;
         int parms_used = raspicamcontrol_parse_cmdline(&state->camera_parameters, &argv[i][1], second_arg);

         // Still unused, try common settings
         if (!parms_used)
            parms_used = raspicommonsettings_parse_cmdline(&state->common_settings, &argv[i][1], second_arg, &application_help_message);

         // If no parms were used, this must be a bad parameter
         if (!parms_used)
            valid = 0;
         else
            i += parms_used - 1;

         break;
      }
      }
   }

   if (!valid)
   {
      fprintf(stderr, "Invalid command line option (%s)\n", argv[i-1]);
      return 1;
   }

   return 0;
}


int main(int argc, char **argv) {
  /* init */
  gst_init (&argc, &argv);

  /* create pipeline */
  auto pipeline = gst_pipeline_new("my-pipeline");

  /* create elements */
  auto src = gst_element_factory_make("fakesrc", "src");
  if (!src) { g_warning("Failed to create src"); }

  gst_base_src_set_live(GST_BASE_SRC_CAST(src), TRUE);

  auto rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
  if (!rtpbin) { g_warning("Failed to create rtpbin"); }
  auto send_rtp_sink_0 = gst_element_get_request_pad(rtpbin, "send_rtp_sink_0");
  if (!send_rtp_sink_0) { g_warning("Failed to create send_rtp_sink_0"); }

  auto udpsink = gst_element_factory_make("udpsink", "udpsink");
  if (!udpsink) { g_warning("Failed to create udpsink"); }


  /* must add elements to pipeline before linking them */
  gst_bin_add_many(GST_BIN(pipeline), src, rtpbin, udpsink, NULL);

  /* link */
  if (!gst_pad_link(GST_BASE_SRC_PAD(src), send_rtp_sink_0)) { g_warning ("Failed to link source to rtpbin"); }
  if (!gst_element_link_pads(rtpbin, "send_rtp_src_0", udpsink, "sink")) { g_warning ("Failed to link rtpbin to udpsink"); }

  if (!gst_element_set_state(pipeline, GST_STATE_PLAYING)) {
    g_warning("Failed to start pipeline");
  }

   // Our main data storage vessel..
   RASPIVID_STATE state{};
   int exit_code = EX_OK;

   MMAL_STATUS_T status = MMAL_SUCCESS;
   MMAL_PORT_T *camera_video_port = NULL;
   MMAL_PORT_T *encoder_input_port = NULL;
   MMAL_PORT_T *encoder_output_port = NULL;

   bcm_host_init();

   // Register our application with the logging system
   vcos_log_register("RaspiVid", VCOS_LOG_CATEGORY);

   signal(SIGINT, default_signal_handler);

   // Disable USR1 for the moment - may be reenabled if go in to signal capture mode
   signal(SIGUSR1, SIG_IGN);

   set_app_name(argv[0]);

   default_status(&state);

   // Parse the command line and put options in to our status structure
   if (parse_cmdline(argc, argv, &state))
   {
      // status = -1; // TODO work out what this did
      exit(EX_USAGE);
   }

   // Setup for sensor specific parameters, only set W/H settings if zero on entry
   get_sensor_defaults(state.common_settings.cameraNum, state.common_settings.camera_name,
                       &state.common_settings.width, &state.common_settings.height);

   if (state.common_settings.verbose)
   {
      print_app_details(stderr);
      dump_status(&state);
   }

   check_camera_model(state.common_settings.cameraNum);


   // OK, we have a nice set of parameters. Now set up our components
   // We have three components. Camera, Preview and encoder.

   if ((status = create_camera_component(&state)) != MMAL_SUCCESS) {
      vcos_log_error("%s: Failed to create camera component", __func__);
      exit_code = EX_SOFTWARE;
   } else if ((status = create_encoder_component(&state)) != MMAL_SUCCESS) {
      vcos_log_error("%s: Failed to create encode component", __func__);
      destroy_camera_component(&state);
      exit_code = EX_SOFTWARE;
   } else {
      if (state.common_settings.verbose)
         fprintf(stderr, "Starting component connection stage\n");

      camera_video_port   = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
      encoder_input_port  = state.encoder_component->input[0];
      encoder_output_port = state.encoder_component->output[0];

      if (state.common_settings.verbose)
         fprintf(stderr, "Connecting camera video port to encoder input port\n");

      // Now connect the camera to the encoder
      status = connect_ports(camera_video_port, encoder_input_port, &state.encoder_connection);

      if (status != MMAL_SUCCESS)
      {
         state.encoder_connection = NULL;
         vcos_log_error("%s: Failed to connect camera video port to encoder input", __func__);
         goto error;
      }

      // Set up our userdata - this is passed though to the callback where we need the information.
      state.callback_data.pstate = &state;
      state.callback_data.abort = 0;

      if (state.common_settings.address) {
        g_object_set(G_OBJECT(udpsink), "host", state.common_settings.address, NULL);
        g_object_set(G_OBJECT(udpsink), "port", state.common_settings.port, NULL);
      }

      // Set up our userdata - this is passed though to the callback where we need the information.
      encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *)&state.callback_data;

      if (state.common_settings.verbose)
         fprintf(stderr, "Enabling encoder output port\n");

      // Enable the encoder output port and tell it its callback function
      status = mmal_port_enable(encoder_output_port, encoder_buffer_callback);

      if (status != MMAL_SUCCESS) {
         vcos_log_error("Failed to setup encoder output");
         goto error;
      }

      // Send all the buffers to the encoder output port
      for (int q = 0; q < mmal_queue_length(state.encoder_pool->queue); q++) {
         MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state.encoder_pool->queue);

         if (!buffer) {
            vcos_log_error("Unable to get a required buffer %d from pool queue", q);
         }

         if (mmal_port_send_buffer(encoder_output_port, buffer)!= MMAL_SUCCESS) {
            vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
         }
      }

      if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
         // How to handle?
      }

      if (state.common_settings.verbose) {
         fprintf(stderr, "Starting video capture\n");
      }

      // We never return from this. Expect a ctrl-c to exit or abort.
      while (!state.callback_data.abort) {
         // Have a sleep so we don't hog the CPU.
         vcos_sleep(ABORT_INTERVAL);
      }

error:

      mmal_status_to_int(status);

      if (state.common_settings.verbose) {
         fprintf(stderr, "Closing down\n");
      }

      // Disable all our ports that are not handled by connections
      check_disable_port(encoder_output_port);

      if (state.encoder_connection) {
         mmal_connection_destroy(state.encoder_connection);
      }

      // Can now close our file. Note disabling ports may flush buffers which causes
      // problems if we have already closed the file!
      // TODO does this need looking at?

      /* Disable components */
      if (state.encoder_component) {
         mmal_component_disable(state.encoder_component);
      }

      if (state.camera_component) {
         mmal_component_disable(state.camera_component);
      }

      destroy_encoder_component(&state);
      destroy_camera_component(&state);

      if (state.common_settings.verbose) {
         fprintf(stderr, "Close down completed, all components disconnected, disabled and destroyed\n\n");
      }
   }

   if (status != MMAL_SUCCESS) {
      raspicamcontrol_check_configuration(128);
   }

   return exit_code;
}
