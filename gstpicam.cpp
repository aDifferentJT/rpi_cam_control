#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bcm_host.h"

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

#include "raspicam.hpp"

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>
#include <gst/base/gstpushsrc.h>
#include "gstpicam.hpp"

#include <gsl/gsl>

GST_DEBUG_CATEGORY_STATIC (gst_pi_cam_debug_category);
#define GST_CAT_DEFAULT gst_pi_cam_debug_category




/* prototypes */

enum {
  PROP_0
};

/* pad templates */

static GstStaticPadTemplate gst_pi_cam_src_template = GST_STATIC_PAD_TEMPLATE
  ( "src"
  , GST_PAD_SRC
  , GST_PAD_ALWAYS
  , GST_STATIC_CAPS ("video/x-h264")
  );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE
  ( GstPiCam
  , gst_pi_cam
  , GST_TYPE_BASE_SRC
  , GST_DEBUG_CATEGORY_INIT
    ( gst_pi_cam_debug_category
    , "picam"
    , 0
    , "debug category for picam element"
    )
  );

// Constructor


// GObject virtual methods

void gst_pi_cam_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec) {
  GstPiCam *picam = GST_PI_CAM (object);

  GST_DEBUG_OBJECT (picam, "set_property");

  switch (property_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void gst_pi_cam_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec) {
  GstPiCam *picam = GST_PI_CAM (object);

  GST_DEBUG_OBJECT (picam, "get_property");

  switch (property_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void gst_pi_cam_dispose (GObject *object) {
  GstPiCam *picam = GST_PI_CAM (object);

  GST_DEBUG_OBJECT (picam, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_pi_cam_parent_class)->dispose (object);
}

void gst_pi_cam_finalize (GObject *object) {
  GstPiCam *picam = GST_PI_CAM (object);

  GST_DEBUG_OBJECT (picam, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (gst_pi_cam_parent_class)->finalize (object);
}

// GstBaseSrc virtual methods

/* start and stop processing, ideal for opening/closing the resource */

static gboolean gst_pi_cam_stop (GstBaseSrc *src) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "stop");

  return TRUE;
}

/* given a buffer, return start and stop time when it should be pushed
 * out. The base class will sync on the clock using these times. */
static void gst_pi_cam_get_times (GstBaseSrc *src, GstBuffer *buffer, GstClockTime *start, GstClockTime *end) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "get_times");

}

/* unlock any pending access to the resource. subclasses should unlock
 * any function ASAP. */
static gboolean gst_pi_cam_unlock (GstBaseSrc *src) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "unlock");

  return TRUE;
}

/* Clear any pending unlock request, as we succeeded in unlocking */
static gboolean gst_pi_cam_unlock_stop (GstBaseSrc *src) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "unlock_stop");

  return TRUE;
}

/* notify subclasses of a query */
static gboolean gst_pi_cam_query (GstBaseSrc *src, GstQuery *query) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "query");

  return TRUE;
}

/* notify subclasses of an event */
static gboolean gst_pi_cam_event (GstBaseSrc *src, GstEvent *event) {
  GstPiCam *picam = GST_PI_CAM (src);

  GST_DEBUG_OBJECT (picam, "event");

  return TRUE;
}

// GstPushSrc virtual methods

static GstFlowReturn gst_pi_cam_create(GstPushSrc *src, GstBuffer **buf) {
  GstPiCam *picam = GST_PI_CAM(src);
  GST_DEBUG_OBJECT(picam, "create");

  g_warning("creating");

  auto queueLock = std::unique_lock{picam->queueMutex};
  picam->queueNotEmpty.wait(queueLock, [&] () { return !picam->queue.empty(); });
  *buf = picam->queue.front();
  picam->queue.pop();

  g_warning("created");

  return GST_FLOW_OK;
}

static void gst_pi_cam_class_init(GstPiCamClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

  gst_element_class_add_static_pad_template(GST_ELEMENT_CLASS(klass), &gst_pi_cam_src_template);

  gst_element_class_set_static_metadata
    ( GST_ELEMENT_CLASS(klass)
    , "Raspberry Pi Camera"
    , "Source/Camera"
    , "Get a live feed from the raspbery pi camera"
    , "Jonathan Tanner"
    );

  gobject_class->set_property = gst_pi_cam_set_property;
  gobject_class->get_property = gst_pi_cam_get_property;
  gobject_class->dispose = gst_pi_cam_dispose;
  gobject_class->finalize = gst_pi_cam_finalize;

  base_src_class->start = GST_DEBUG_FUNCPTR(gst_pi_cam_start);
  base_src_class->stop = GST_DEBUG_FUNCPTR(gst_pi_cam_stop);
  base_src_class->get_times = GST_DEBUG_FUNCPTR(gst_pi_cam_get_times);
  base_src_class->unlock = GST_DEBUG_FUNCPTR(gst_pi_cam_unlock);
  base_src_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_pi_cam_unlock_stop);
  base_src_class->query = GST_DEBUG_FUNCPTR(gst_pi_cam_query);
  base_src_class->event = GST_DEBUG_FUNCPTR(gst_pi_cam_event);

  push_src_class->create = GST_DEBUG_FUNCPTR(gst_pi_cam_create);

  base_src_class->is_seekable = [] (GstBaseSrc *src) { return FALSE; };
}

static gboolean plugin_init (GstPlugin *plugin) {
  return gst_element_register
    ( plugin
    , "picam"
    , GST_RANK_NONE
    , GST_TYPE_PI_CAM
    );
}

/* FIXME: these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.0.FIXME"
#endif
#ifndef PACKAGE
#define PACKAGE "picam"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "picam"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://FIXME.org/"
#endif

GST_PLUGIN_DEFINE
  ( GST_VERSION_MAJOR
  , GST_VERSION_MINOR
  , picam
  , "FIXME plugin description"
  , plugin_init
  , VERSION
  , "LGPL"
  , PACKAGE_NAME
  , GST_PACKAGE_ORIGIN
  )

