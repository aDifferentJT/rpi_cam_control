
#include "raspicam.hpp"

#include <iostream>

MMAL_COMPONENT_T *raspicam::create_camera_component(GstPiCam *picam) {
  MMAL_COMPONENT_T *raw_camera;
  if (mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &raw_camera) != MMAL_SUCCESS) {
    vcos_log_error("Failed to create camera component");
    return nullptr;
  }

  std::unique_ptr<MMAL_COMPONENT_T, void (*)(MMAL_COMPONENT_T*)> camera{raw_camera, [] (auto cam) { mmal_component_destroy(cam); }};

  if ( auto status = raspicamcontrol_set_stereo_mode(camera->output[MMAL_CAMERA_VIDEO_PORT], &picam->camera_parameters.stereo_mode)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Could not set stereo mode : error %d", status);
    return nullptr;
  }

  MMAL_PARAMETER_INT32_T camera_num = {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, picam->cameraNum};

  if ( auto status = mmal_port_parameter_set(camera->control, &camera_num.hdr)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Could not select camera : error %d", status);
    return nullptr;
  }

  if (!camera->output_num) {
    vcos_log_error("Camera doesn't have output ports");
    return nullptr;
  }

  if ( auto status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, picam->sensor_mode)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Could not set sensor mode : error %d", status);
    return nullptr;
  }

   // Enable the camera, and tell it its control callback function
  if ( auto status = mmal_port_enable(camera->control, default_camera_control_callback)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Unable to enable control port : error %d", status);
    return nullptr;
  }

  //  set up the camera configuration
  {
    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      { { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) }
      , .max_stills_w = (uint32_t)picam->width
      , .max_stills_h = (uint32_t)picam->height
      , .stills_yuv422 = 0
      , .one_shot_stills = 0
      , .max_preview_video_w = (uint32_t)picam->width
      , .max_preview_video_h = (uint32_t)picam->height
      , .num_preview_video_frames = (uint32_t)(3 + vcos_max(0, (picam->framerate-30)/10))
      , .stills_capture_circular_buffer_height = 0
      , .fast_preview_resume = 0
      , .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
      };
    mmal_port_parameter_set(camera->control, &cam_config.hdr);
  }

  // Now set up the port formats

  // Set the encode format on the video port

  MMAL_PORT_T *video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];

  MMAL_ES_FORMAT_T *format = video_port->format;
  format->encoding_variant = MMAL_ENCODING_I420;

  if (picam->camera_parameters.shutter_speed > 6000000) {
    MMAL_PARAMETER_FPS_RANGE_T fps_range =
      { {MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)}
      , { 5, 1000 }
      , { 166, 1000 }
      };
    mmal_port_parameter_set(video_port, &fps_range.hdr);
  } else if (picam->camera_parameters.shutter_speed > 1000000) {
    MMAL_PARAMETER_FPS_RANGE_T fps_range =
      { {MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)}
      , { 167, 1000 }
      , { 999, 1000 }
      };
    mmal_port_parameter_set(video_port, &fps_range.hdr);
  }

  format->encoding = MMAL_ENCODING_OPAQUE;
  format->es->video.width = VCOS_ALIGN_UP(picam->width, 32);
  format->es->video.height = VCOS_ALIGN_UP(picam->height, 16);
  format->es->video.crop.x = 0;
  format->es->video.crop.y = 0;
  format->es->video.crop.width = picam->width;
  format->es->video.crop.height = picam->height;
  format->es->video.frame_rate.num = picam->framerate;
  format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

  if (mmal_port_format_commit(video_port) != MMAL_SUCCESS) {
    vcos_log_error("camera video format couldn't be set");
    return nullptr;
  }

  // Ensure there are enough buffers to avoid dropping frames
  if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM) {
    video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;
  }

  if (mmal_component_enable(camera.get()) != MMAL_SUCCESS) {
    vcos_log_error("camera component couldn't be enabled");
    return nullptr;
  }

  // Note: this sets lots of parameters that were not individually addressed before.
  raspicamcontrol_set_all_parameters(camera.get(), &picam->camera_parameters);

  return camera.release();
}

void raspicam::destroy_camera_component(MMAL_COMPONENT_T *&component) {
   if (component) {
      mmal_component_destroy(component);
      component = nullptr;
   }
}

std::pair<MMAL_COMPONENT_T*, MMAL_POOL_T*> raspicam::create_encoder_component(GstPiCam *picam) {
  MMAL_COMPONENT_T *raw_encoder;
  if ( auto status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &raw_encoder)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Unable to create video encoder component");
    return std::make_pair(nullptr, nullptr);
  }

  std::unique_ptr<MMAL_COMPONENT_T, void(*)(MMAL_COMPONENT_T*)> encoder{raw_encoder, [] (auto enc) { mmal_component_destroy(enc); } };

  if (!encoder->input_num || !encoder->output_num) {
    vcos_log_error("Video encoder doesn't have input/output ports");
    return std::make_pair(nullptr, nullptr);
  }

  auto encoder_input = encoder->input[0];
  auto encoder_output = encoder->output[0];

  // We want same format on input and output
  mmal_format_copy(encoder_output->format, encoder_input->format);

  // Only supporting H264 at the moment
  encoder_output->format->encoding = MMAL_ENCODING_H264;

  if (picam->level == MMAL_VIDEO_LEVEL_H264_4) {
    if (picam->bitrate > MAX_BITRATE_LEVEL4) {
      fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
      picam->bitrate = MAX_BITRATE_LEVEL4;
    }
  } else {
    if (picam->bitrate > MAX_BITRATE_LEVEL42) {
      fprintf(stderr, "Bitrate too high: Reducing to 62.5MBit/s\n");
      picam->bitrate = MAX_BITRATE_LEVEL42;
    }
  }

  encoder_output->format->bitrate = picam->bitrate;

  encoder_output->buffer_size = encoder_output->buffer_size_recommended;

  if (encoder_output->buffer_size < encoder_output->buffer_size_min) {
    encoder_output->buffer_size = encoder_output->buffer_size_min;
  }

  encoder_output->buffer_num = encoder_output->buffer_num_recommended;

  if (encoder_output->buffer_num < encoder_output->buffer_num_min) {
    encoder_output->buffer_num = encoder_output->buffer_num_min;
  }

  // We need to set the frame rate on output to 0, to ensure it gets
  // updated correctly from the input framerate when port connected
  encoder_output->format->es->video.frame_rate.num = 0;
  encoder_output->format->es->video.frame_rate.den = 1;

  // Commit the port changes to the output port
  if ( auto status = mmal_port_format_commit(encoder_output)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Unable to set format on video encoder output port");
    return std::make_pair(nullptr, nullptr);
  }

  if (picam->intraperiod) {
    MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, (uint32_t)*picam->intraperiod};
    if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
       ; status != MMAL_SUCCESS
       ) {
      vcos_log_error("Unable to set intraperiod");
      return std::make_pair(nullptr, nullptr);
    }
  }

  if (picam->quantisationParameter) {
    {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT, sizeof(param)}, picam->quantisationParameter};
      if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
         ; status != MMAL_SUCCESS
         ) {
        vcos_log_error("Unable to set initial QP");
        return std::make_pair(nullptr, nullptr);
      }
    }

    {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT, sizeof(param)}, picam->quantisationParameter};
      if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
         ; status != MMAL_SUCCESS
         ) {
        vcos_log_error("Unable to set min QP");
        return std::make_pair(nullptr, nullptr);
      }
    }

    {
      MMAL_PARAMETER_UINT32_T param = {{ MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT, sizeof(param)}, picam->quantisationParameter};
      if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
         ; status != MMAL_SUCCESS
         ) {
        vcos_log_error("Unable to set max QP");
        return std::make_pair(nullptr, nullptr);
      }
    }
  }

  MMAL_PARAMETER_VIDEO_PROFILE_T  param;
  param.hdr.id = MMAL_PARAMETER_PROFILE;
  param.hdr.size = sizeof(param);

  param.profile[0].profile = picam->profile;

  if ((VCOS_ALIGN_UP(picam->width,16) >> 4) * (VCOS_ALIGN_UP(picam->height,16) >> 4) * picam->framerate > 245760) {
    if ((VCOS_ALIGN_UP(picam->width,16) >> 4) * (VCOS_ALIGN_UP(picam->height,16) >> 4) * picam->framerate <= 522240) {
      fprintf(stderr, "Too many macroblocks/s: Increasing H264 Level to 4.2\n");
      picam->level=MMAL_VIDEO_LEVEL_H264_42;
    } else {
      vcos_log_error("Too many macroblocks/s requested");
      return std::make_pair(nullptr, nullptr);
    }
  }

  param.profile[0].level = picam->level;

  if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Unable to set H264 profile");
    return std::make_pair(nullptr, nullptr);
  }

  //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
  if ( auto status = mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, picam->bInlineHeaders)
     ; status != MMAL_SUCCESS
     ) {
     vcos_log_error("failed to set INLINE HEADER FLAG parameters");
     // Continue rather than abort..
  }

  //set flag for add SPS TIMING
  if ( auto status = mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, picam->addSPSTiming)
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("failed to set SPS TIMINGS FLAG parameters");
    // Continue rather than abort..
  }

  // Adaptive intra refresh settings
  if (picam->intra_refresh_type) {
    MMAL_PARAMETER_VIDEO_INTRA_REFRESH_T param;
    param.hdr.id = MMAL_PARAMETER_VIDEO_INTRA_REFRESH;
    param.hdr.size = sizeof(param);

    // Get first so we don't overwrite anything unexpectedly
    if ( auto status = mmal_port_parameter_get(encoder_output, &param.hdr)
       ; status != MMAL_SUCCESS
       ) {
      vcos_log_warn("Unable to get existing H264 intra-refresh values. Please update your firmware");
      // Set some defaults, don't just pass random stack data
      param.air_mbs = param.air_ref = param.cir_mbs = param.pir_mbs = 0;
    }

    param.refresh_mode = *picam->intra_refresh_type;

    if ( auto status = mmal_port_parameter_set(encoder_output, &param.hdr)
       ; status != MMAL_SUCCESS
       ) {
      vcos_log_error("Unable to set H264 intra-refresh values");
      return std::make_pair(nullptr, nullptr);
    }
  }

  if ( auto status = mmal_component_enable(encoder.get())
     ; status != MMAL_SUCCESS
     ) {
    vcos_log_error("Unable to enable video encoder component");
    return std::make_pair(nullptr, nullptr);
  }

   /* Create pool of buffer headers for the output port to consume */
  MMAL_POOL_T *pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

  if (!pool) {
    vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
  }

  return std::make_pair(encoder.release(), pool);
}

void raspicam::destroy_encoder_component(MMAL_COMPONENT_T *&component, MMAL_POOL_T *&pool) {
  // Get rid of any port buffers first
  if (pool) {
    mmal_port_pool_destroy(component->output[0], pool);
    pool = nullptr;
  }

  if (component) {
    mmal_component_destroy(component);
    component = nullptr;
  }
}


MMAL_CONNECTION_T *raspicam::connect_ports(MMAL_PORT_T *output, MMAL_PORT_T *input) {
  MMAL_CONNECTION_T *connection = nullptr;

  if ( auto status = mmal_connection_create(&connection, output, input, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT)
     ; status != MMAL_SUCCESS
     ) {
    std::cerr << "Could not create connection\n";
    return nullptr;
  }

  if ( auto status = mmal_connection_enable(connection)
     ; status != MMAL_SUCCESS
     ) {
    std::cerr << "Could not enable connection\n";
    mmal_connection_destroy(connection);
    return nullptr;
  }

  return connection;
}

