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

#include <ccrtp/rtp.h>

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

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Video format information
// 0 implies variable
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

// Max bitrate we allow for recording
const int MAX_BITRATE_MJPEG = 25000000; // 25Mbits/s
const int MAX_BITRATE_LEVEL4 = 25000000; // 25Mbits/s
const int MAX_BITRATE_LEVEL42 = 62500000; // 62.5Mbits/s

/// Interval at which we check for an failure abort during capture
const int ABORT_INTERVAL = 100; // ms


// Forward
struct RASPIVID_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
struct PORT_USERDATA {
   RASPIVID_STATE *pstate = nullptr;  /// pointer to our state in case required in callback
   int abort = 0;                     /// Set to 1 in callback if an error occurs to attempt to abort the capture
   char header_bytes[29];
   int header_wptr = 0;
   ost::RTPSession rtp_session{ost::InetHostAddress{"0.0.0.0"}};
   uint64_t initial_time = get_microseconds64();
};

/** Structure containing all state information for the current run
 */
struct RASPIVID_STATE {
   RASPICOMMONSETTINGS_PARAMETERS common_settings{};     /// Common settings
   MMAL_FOURCC_T encoding = MMAL_ENCODING_H264;             /// Requested codec video encoding (MJPEG or H264)
   int bitrate = 17000000;                        /// Requested bitrate
   int framerate = VIDEO_FRAME_RATE_NUM;                      /// Requested frame rate (fps)
   int intraperiod = -1;                    /// Intra-refresh period (key frame rate)
   uint32_t quantisationParameter = 0;     /// Quantisation parameter - quality. Set bitrate 0 and set this for variable bitrate
   int bInlineHeaders = 0;                 /// Insert inline headers to stream (SPS, PPS)
   MMAL_VIDEO_PROFILE_T profile = MMAL_VIDEO_PROFILE_H264_HIGH;       /// H264 profile to use for encoding
   MMAL_VIDEO_LEVEL_T level = MMAL_VIDEO_LEVEL_H264_4;           /// H264 level to use for encoding

   RASPICAM_CAMERA_PARAMETERS camera_parameters{}; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component = nullptr;    /// Pointer to the camera component
   MMAL_COMPONENT_T *encoder_component = nullptr;   /// Pointer to the encoder component
   MMAL_CONNECTION_T *encoder_connection = nullptr; /// Pointer to the connection from camera to encoder

   MMAL_POOL_T *encoder_pool = nullptr; /// Pointer to the pool of buffers used by encoder output port

   PORT_USERDATA callback_data{};        /// Used to move data to the encoder callback

   std::optional<MMAL_VIDEO_INTRA_REFRESH_T> intra_refresh_type = std::nullopt; /// What intra refresh type to use.
   int frame = 0;
   int64_t starttime = 0;
   int64_t lasttime = 0;

   MMAL_BOOL_T addSPSTiming = MMAL_FALSE;
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
static int parse_cmdline(int argc, const char **argv, RASPIVID_STATE *state)
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
         const char *second_arg = (i + 1 < argc) ? argv[i + 1] : NULL;
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


/**
 * Update any annotation data specific to the video.
 * This simply passes on the setting from cli, or
 * if application defined annotate requested, updates
 * with the H264 parameters
 *
 * @param state Pointer to state control struct
 *
 */
static void update_annotation_data(RASPIVID_STATE *state)
{
   // So, if we have asked for a application supplied string, set it to the H264 or GPS parameters
   if (state->camera_parameters.enable_annotate & ANNOTATE_APP_TEXT) {
      char *text;

      const char *refresh = state->intra_refresh_type ? raspicli_unmap_xref(*state->intra_refresh_type, intra_refresh_map) : "(none)";

      asprintf
        ( &text
        , "%dk,%df,%s,%d,%s,%s"
        , state->bitrate / 1000,  state->framerate
        , refresh
        , state->intraperiod
        , raspicli_unmap_xref(state->profile, profile_map)
        , raspicli_unmap_xref(state->level, level_map)
        );

      raspicamcontrol_set_annotate
        ( state->camera_component
        , state->camera_parameters.enable_annotate
        , text
        , state->camera_parameters.annotate_text_size
        , state->camera_parameters.annotate_text_colour
        , state->camera_parameters.annotate_bg_colour
        , state->camera_parameters.annotate_justify
        , state->camera_parameters.annotate_x
        , state->camera_parameters.annotate_y
        );

      free(text);
   } else {
      raspicamcontrol_set_annotate
        ( state->camera_component
        , state->camera_parameters.enable_annotate
        , state->camera_parameters.annotate_string
        , state->camera_parameters.annotate_text_size
        , state->camera_parameters.annotate_text_colour
        , state->camera_parameters.annotate_bg_colour
        , state->camera_parameters.annotate_justify
        , state->camera_parameters.annotate_x
        , state->camera_parameters.annotate_y
        );
   }
}

/**
 *  buffer header callback function for encoder
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   static int64_t last_second = -1;

   // We pass our file handle and other stuff in via the userdata field.

   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData) {
      int bytes_written = buffer->length;
      int64_t current_time = get_microseconds64()/1000;

      if (buffer->length)
      {
         mmal_buffer_header_mem_lock(buffer);
         if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
            bytes_written = buffer->length;
         } else {

            uint32 timestamp = ((get_microseconds64() - pData->initial_time) * pData->rtp_session.getCurrentRTPClockRate() / 1000);
            std::cerr << "Current time: " << get_microseconds64() << "\ntimestamp: " << timestamp << std::endl;
            pData->rtp_session.sendImmediate(timestamp, buffer->data, buffer->length);
            while (pData->rtp_session.isSending()) {} // This shouldn't be a spin loop
         }

         mmal_buffer_header_mem_unlock(buffer);

         if (bytes_written != buffer->length) {
            vcos_log_error("Failed to write buffer data (%d from %d)- aborting", bytes_written, buffer->length);
            pData->abort = 1;
         }
      }

      // See if the second count has changed and we need to update any annotation
      if (current_time/1000 != last_second) {
         update_annotation_data(pData->pstate);
         last_second = current_time/1000;
      }
   } else {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled) {
      MMAL_BUFFER_HEADER_T *new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer) {
         if (mmal_port_send_buffer(port, new_buffer) != MMAL_SUCCESS) {
             vcos_log_error("Unable to return a buffer to the encoder port");
         }
      } else {
         vcos_log_error("Unable to return a buffer to the encoder port");
      }
   }
}

/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(RASPIVID_STATE *state)
{
   MMAL_COMPONENT_T *camera = NULL;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *video_port = NULL;
   MMAL_STATUS_T status;
   MMAL_PARAMETER_INT32_T camera_num;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);;
   if (status != MMAL_SUCCESS) {
      vcos_log_error("Failed to create camera component");
      goto error;
   }

   status = (MMAL_STATUS_T)raspicamcontrol_set_stereo_mode(camera->output[0], &state->camera_parameters.stereo_mode);
   if (status != MMAL_SUCCESS) {
      vcos_log_error("Could not set stereo mode : error %d", status);
      goto error;
   }
   status = (MMAL_STATUS_T)raspicamcontrol_set_stereo_mode(camera->output[1], &state->camera_parameters.stereo_mode);
   if (status != MMAL_SUCCESS) {
      vcos_log_error("Could not set stereo mode : error %d", status);
      goto error;
   }
   status = (MMAL_STATUS_T)raspicamcontrol_set_stereo_mode(camera->output[2], &state->camera_parameters.stereo_mode);
   if (status != MMAL_SUCCESS) {
      vcos_log_error("Could not set stereo mode : error %d", status);
      goto error;
   }

   camera_num = {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->common_settings.cameraNum};

   status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("Could not select camera : error %d", status);
      goto error;
   }

   if (!camera->output_num) {
      status = MMAL_ENOSYS;
      vcos_log_error("Camera doesn't have output ports");
      goto error;
   }

   status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, state->common_settings.sensor_mode);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("Could not set sensor mode : error %d", status);
      goto error;
   }

   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, default_camera_control_callback);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("Unable to enable control port : error %d", status);
      goto error;
   }

   //  set up the camera configuration
   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      {
         { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
         .max_stills_w = (uint32_t)state->common_settings.width,
         .max_stills_h = (uint32_t)state->common_settings.height,
         .stills_yuv422 = 0,
         .one_shot_stills = 0,
         .max_preview_video_w = (uint32_t)state->common_settings.width,
         .max_preview_video_h = (uint32_t)state->common_settings.height,
         .num_preview_video_frames = (uint32_t)(3 + vcos_max(0, (state->framerate-30)/10)),
         .stills_capture_circular_buffer_height = 0,
         .fast_preview_resume = 0,
         .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
      };
      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   // Now set up the port formats

   // Set the encode format on the video port

   format = video_port->format;
   format->encoding_variant = MMAL_ENCODING_I420;

   if (state->camera_parameters.shutter_speed > 6000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 5, 1000 }, {166, 1000}
      };
      mmal_port_parameter_set(video_port, &fps_range.hdr);
   }
   else if (state->camera_parameters.shutter_speed > 1000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 167, 1000 }, {999, 1000}
      };
      mmal_port_parameter_set(video_port, &fps_range.hdr);
   }

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->common_settings.width;
   format->es->video.crop.height = state->common_settings.height;
   format->es->video.frame_rate.num = state->framerate;
   format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

   status = mmal_port_format_commit(video_port);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM) {
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;
   }

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   // Note: this sets lots of parameters that were not individually addressed before.
   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   state->camera_component = camera;

   update_annotation_data(state);

   if (state->common_settings.verbose) {
      fprintf(stderr, "Camera component done\n");
   }

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPIVID_STATE *state)
{
   if (state->camera_component) {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}

/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_encoder_component(RASPIVID_STATE *state)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   MMAL_STATUS_T status;
   MMAL_POOL_T *pool;

   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to create video encoder component");
      goto error;
   }

   if (!encoder->input_num || !encoder->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Video encoder doesn't have input/output ports");
      goto error;
   }

   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];

   // We want same format on input and output
   mmal_format_copy(encoder_output->format, encoder_input->format);

   // Only supporting H264 at the moment
   encoder_output->format->encoding = state->encoding;

   if (state->encoding == MMAL_ENCODING_H264)
   {
      if (state->level == MMAL_VIDEO_LEVEL_H264_4)
      {
         if (state->bitrate > MAX_BITRATE_LEVEL4)
         {
            fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
            state->bitrate = MAX_BITRATE_LEVEL4;
         }
      }
      else
      {
         if (state->bitrate > MAX_BITRATE_LEVEL42)
         {
            fprintf(stderr, "Bitrate too high: Reducing to 62.5MBit/s\n");
            state->bitrate = MAX_BITRATE_LEVEL42;
         }
      }
   }
   else if (state->encoding == MMAL_ENCODING_MJPEG)
   {
      if (state->bitrate > MAX_BITRATE_MJPEG)
      {
         fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
         state->bitrate = MAX_BITRATE_MJPEG;
      }
   }

   encoder_output->format->bitrate = state->bitrate;

   if (state->encoding == MMAL_ENCODING_H264)
      encoder_output->buffer_size = encoder_output->buffer_size_recommended;
   else
      encoder_output->buffer_size = 256<<10;


   if (encoder_output->buffer_size < encoder_output->buffer_size_min)
      encoder_output->buffer_size = encoder_output->buffer_size_min;

   encoder_output->buffer_num = encoder_output->buffer_num_recommended;

   if (encoder_output->buffer_num < encoder_output->buffer_num_min)
      encoder_output->buffer_num = encoder_output->buffer_num_min;

   // We need to set the frame rate on output to 0, to ensure it gets
   // updated correctly from the input framerate when port connected
   encoder_output->format->es->video.frame_rate.num = 0;
   encoder_output->format->es->video.frame_rate.den = 1;

   // Commit the port changes to the output port
   status = mmal_port_format_commit(encoder_output);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set format on video encoder output port");
      goto error;
   }

   // Set the rate control parameter
   if (0)
   {
      MMAL_PARAMETER_VIDEO_RATECONTROL_T param = {{ MMAL_PARAMETER_RATECONTROL, sizeof(param)}, MMAL_VIDEO_RATECONTROL_DEFAULT};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set ratecontrol");
         goto error;
      }

   }

   if (state->encoding == MMAL_ENCODING_H264 && state->intraperiod != -1) {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, (uint32_t)state->intraperiod};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS) {
         vcos_log_error("Unable to set intraperiod");
         goto error;
      }
   }

   if (state->encoding == MMAL_ENCODING_H264 && state->slices > 1 && state->common_settings.width <= 1280) {
      int frame_mb_rows = VCOS_ALIGN_UP(state->common_settings.height, 16) >> 4;

      if (state->slices > frame_mb_rows) //warn user if too many slices selected
      {
         fprintf(stderr,"H264 Slice count (%d) exceeds number of macroblock rows (%d). Setting slices to %d.\n", state->slices, frame_mb_rows, frame_mb_rows);
         // Continue rather than abort..
      }
      int slice_row_mb = frame_mb_rows/state->slices;
      if (frame_mb_rows - state->slices*slice_row_mb)
         slice_row_mb++; //must round up to avoid extra slice if not evenly divided

      status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_MB_ROWS_PER_SLICE, slice_row_mb);
      if (status != MMAL_SUCCESS) {
         vcos_log_error("Unable to set number of slices");
         goto error;
      }
   }

   if (state->encoding == MMAL_ENCODING_H264 &&
       state->quantisationParameter)
   {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set initial QP");
         goto error;
      }

      MMAL_PARAMETER_UINT32_T param2 = {{ MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param2.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set min QP");
         goto error;
      }

      MMAL_PARAMETER_UINT32_T param3 = {{ MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT, sizeof(param)}, state->quantisationParameter};
      status = mmal_port_parameter_set(encoder_output, &param3.hdr);
      if (status != MMAL_SUCCESS)
      {
         vcos_log_error("Unable to set max QP");
         goto error;
      }
   }

   if (state->encoding == MMAL_ENCODING_H264)
   {
      MMAL_PARAMETER_VIDEO_PROFILE_T  param;
      param.hdr.id = MMAL_PARAMETER_PROFILE;
      param.hdr.size = sizeof(param);

      param.profile[0].profile = state->profile;

      if ((VCOS_ALIGN_UP(state->common_settings.width,16) >> 4) * (VCOS_ALIGN_UP(state->common_settings.height,16) >> 4) * state->framerate > 245760) {
         if ((VCOS_ALIGN_UP(state->common_settings.width,16) >> 4) * (VCOS_ALIGN_UP(state->common_settings.height,16) >> 4) * state->framerate <= 522240) {
            fprintf(stderr, "Too many macroblocks/s: Increasing H264 Level to 4.2\n");
            state->level=MMAL_VIDEO_LEVEL_H264_42;
         } else {
            vcos_log_error("Too many macroblocks/s requested");
            status = MMAL_EINVAL;
            goto error;
         }
      }

      param.profile[0].level = state->level;

      status = mmal_port_parameter_set(encoder_output, &param.hdr);
      if (status != MMAL_SUCCESS) {
         vcos_log_error("Unable to set H264 profile");
         goto error;
      }
   }

   if (state->encoding == MMAL_ENCODING_H264) {
      //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
      if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, state->bInlineHeaders) != MMAL_SUCCESS) {
         vcos_log_error("failed to set INLINE HEADER FLAG parameters");
         // Continue rather than abort..
      }

      //set flag for add SPS TIMING
      if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, state->addSPSTiming) != MMAL_SUCCESS) {
         vcos_log_error("failed to set SPS TIMINGS FLAG parameters");
         // Continue rather than abort..
      }

      // Adaptive intra refresh settings
      if (state->intra_refresh_type) {
         MMAL_PARAMETER_VIDEO_INTRA_REFRESH_T  param;
         param.hdr.id = MMAL_PARAMETER_VIDEO_INTRA_REFRESH;
         param.hdr.size = sizeof(param);

         // Get first so we don't overwrite anything unexpectedly
         status = mmal_port_parameter_get(encoder_output, &param.hdr);
         if (status != MMAL_SUCCESS) {
            vcos_log_warn("Unable to get existing H264 intra-refresh values. Please update your firmware");
            // Set some defaults, don't just pass random stack data
            param.air_mbs = param.air_ref = param.cir_mbs = param.pir_mbs = 0;
         }

         param.refresh_mode = *state->intra_refresh_type;

         //if (state->intra_refresh_type == MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS)
         //   param.cir_mbs = 10;

         status = mmal_port_parameter_set(encoder_output, &param.hdr);
         if (status != MMAL_SUCCESS) {
            vcos_log_error("Unable to set H264 intra-refresh values");
            goto error;
         }
      }
   }

   //  Enable component
   status = mmal_component_enable(encoder);

   if (status != MMAL_SUCCESS) {
      vcos_log_error("Unable to enable video encoder component");
      goto error;
   }

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

   if (!pool) {
      vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
   }

   state->encoder_pool = pool;
   state->encoder_component = encoder;

   if (state->common_settings.verbose) {
      fprintf(stderr, "Encoder component done\n");
   }

   return status;

error:
   if (encoder)
      mmal_component_destroy(encoder);

   state->encoder_component = NULL;

   return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPIVID_STATE *state)
{
   // Get rid of any port buffers first
   if (state->encoder_pool)
   {
      mmal_port_pool_destroy(state->encoder_component->output[0], state->encoder_pool);
   }

   if (state->encoder_component)
   {
      mmal_component_destroy(state->encoder_component);
      state->encoder_component = NULL;
   }
}

/**
 * main
 */
int main(int argc, const char **argv)
{
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
         std::cout << "My SSRC identifier is: "
                   << std::hex << (int)state.callback_data.rtp_session.getLocalSSRC() << std::endl;
         ost::defaultApplication().setSDESItem(ost::SDESItemTypeTOOL, "cam_controller");
         state.callback_data.rtp_session.setSchedulingTimeout(10000);
         state.callback_data.rtp_session.setExpireTimeout(1000000);
      
         if (!state.callback_data.rtp_session.addDestination(state.common_settings.address, state.common_settings.port, state.common_settings.control_port)) {
            // Notify user, carry on but discarding encoded output buffers
            vcos_log_error("%s: Could not connect to: %s\n", __func__, state.common_settings.address.getHostname());
         }
      
         state.callback_data.rtp_session.setPayloadFormat(ost::StaticPayloadFormat(ost::sptPCMU));
         state.callback_data.rtp_session.startRunning();

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
