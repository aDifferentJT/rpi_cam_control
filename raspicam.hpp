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

#include <functional>
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

// Video format information
// 0 implies variable
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

class RaspiCam {
  private:
    std::function<void(uint8_t const*, uint32_t)> callback;

    MMAL_COMPONENT_T *camera_component = nullptr;
    MMAL_COMPONENT_T *encoder_component = nullptr;
  
    MMAL_CONNECTION_T *encoder_connection = nullptr;
  
    MMAL_PORT_T *camera_video_port = nullptr;
    MMAL_PORT_T *encoder_input_port = nullptr;
    MMAL_PORT_T *encoder_output_port = nullptr;
  
    MMAL_POOL_T *encoder_pool = nullptr;
  
    char camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN] = "(Unknown)"; // Name of the camera sensor
    int width = 1920;                   /// Requested width of image
    int height = 1080;                  /// requested height of image
    char const *address = "192.168.16.122";  /// output address
    int port = 5004;                         /// output port
    int cameraNum = 0;                  /// Camera number
    int sensor_mode = 0;                /// Sensor mode. 0=auto. Check docs/forum for modes selected by other values.
  
    int bitrate = 17000000;                        /// Requested bitrate
    int framerate = VIDEO_FRAME_RATE_NUM;                      /// Requested frame rate (fps)
    std::optional<int> intraperiod = std::nullopt;                    /// Intra-refresh period (key frame rate)
    uint32_t quantisationParameter = 0;     /// Quantisation parameter - quality. Set bitrate 0 and set this for variable bitrate
    int bInlineHeaders = 0;                 /// Insert inline headers to stream (SPS, PPS)
    MMAL_VIDEO_PROFILE_T profile = MMAL_VIDEO_PROFILE_H264_HIGH;       /// H264 profile to use for encoding
    MMAL_VIDEO_LEVEL_T level = MMAL_VIDEO_LEVEL_H264_4;           /// H264 level to use for encoding
  
    RASPICAM_CAMERA_PARAMETERS camera_parameters{}; /// Camera setup parameters
  
    std::optional<MMAL_VIDEO_INTRA_REFRESH_T> intra_refresh_type = std::nullopt; /// What intra refresh type to use.
    MMAL_BOOL_T addSPSTiming = MMAL_FALSE;

  public:
    RaspiCam(std::function<void(uint8_t const*, uint32_t)> callback);

    void start();

  private:
    MMAL_COMPONENT_T *create_camera_component();
  
    static void destroy_camera_component(MMAL_COMPONENT_T *&component);
  
    std::pair<MMAL_COMPONENT_T*, MMAL_POOL_T*> create_encoder_component();
  
    static void destroy_encoder_component(MMAL_COMPONENT_T *&component, MMAL_POOL_T *&pool);
  
    static MMAL_CONNECTION_T *connect_ports(MMAL_PORT_T *output, MMAL_PORT_T *input);

    static void mmal_encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *mmal_buffer);

  public:
    struct CouldNotStart {};
};

#endif
