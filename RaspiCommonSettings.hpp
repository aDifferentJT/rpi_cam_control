/*
Copyright (c) 2018, Raspberry Pi (Trading) Ltd.
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
 * \file RaspiCommonSettings.c
 *
 * Description
 *
 * Handles general settings applicable to all the camera applications
 */

#ifndef RASPIGENERALSETTINGS_H_
#define RASPIGENERALSETTINGS_H_

#include "interface/mmal/mmal_parameters_camera.h"

#include <ccrtp/rtp.h>

struct RASPICOMMONSETTINGS_PARAMETERS {
  char camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN] = {'(','U','k','n','o','w','n',')','\0'}; // Name of the camera sensor
  int width = 1920;                   /// Requested width of image
  int height = 1080;                  /// requested height of image
  ost::InetHostAddress address{"192.168.16.122"};    /// output address
  ost::tpport_t port = ost::DefaultRTPDataPort;  /// output port
  ost::tpport_t control_port = 0;                /// output port
  int cameraNum = 0;                  /// Camera number
  int sensor_mode = 0;                /// Sensor mode. 0=auto. Check docs/forum for modes selected by other values.
  int verbose = 0;                    /// !0 if want detailed run information
};

void raspicommonsettings_dump_parameters(RASPICOMMONSETTINGS_PARAMETERS *);
void raspicommonsettings_display_help();
int raspicommonsettings_parse_cmdline(RASPICOMMONSETTINGS_PARAMETERS *state, const char *arg1, const char *arg2, void (*app_help)(const char *));

#endif