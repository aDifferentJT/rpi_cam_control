#ifndef RASPICAM_HPP
#define RASPICAM_HPP

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

#include "gstpicam.hpp"

#include <optional>

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

// Max bitrate we allow for recording
const int MAX_BITRATE_MJPEG = 25000000; // 25Mbits/s
const int MAX_BITRATE_LEVEL4 = 25000000; // 25Mbits/s
const int MAX_BITRATE_LEVEL42 = 62500000; // 62.5Mbits/s

/// Interval at which we check for an failure abort during capture
const int ABORT_INTERVAL = 100; // ms

namespace raspicam {

  MMAL_COMPONENT_T *create_camera_component(GstPiCam *picam);

  void destroy_camera_component(MMAL_COMPONENT_T *&component);

  std::pair<MMAL_COMPONENT_T*, MMAL_POOL_T*> create_encoder_component(GstPiCam *picam);

  void destroy_encoder_component(MMAL_COMPONENT_T *&component, MMAL_POOL_T *&pool);

  MMAL_CONNECTION_T *connect_ports(MMAL_PORT_T *output, MMAL_PORT_T *input);

}

#endif
