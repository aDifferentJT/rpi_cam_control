#ifndef _GST_PI_CAM_H_
#define _GST_PI_CAM_H_

#include <gst/base/gstpushsrc.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <queue>

G_BEGIN_DECLS

#define GST_TYPE_PI_CAM   (gst_pi_cam_get_type())
#define GST_PI_CAM(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PI_CAM,GstPiCam))
#define GST_PI_CAM_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PI_CAM,GstPiCamClass))
#define GST_IS_PI_CAM(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PI_CAM))
#define GST_IS_PI_CAM_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PI_CAM))

// Video format information
// 0 implies variable
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

typedef struct _GstPiCam {
  GstPushSrc base_picam;

  std::queue<GstBuffer*> queue;
  std::mutex queueMutex;
  std::condition_variable queueNotEmpty;

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
} GstPiCam;

typedef struct _GstPiCamClass {
  GstPushSrcClass base_picam_class;
} GstPiCamClass;

GType gst_pi_cam_get_type (void);

G_END_DECLS

#endif
