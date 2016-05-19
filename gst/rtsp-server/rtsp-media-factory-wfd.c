/* GStreamer
 * Copyright (C) 2015 Samsung Electronics Hyunjun Ko <zzoon.ko@samsung.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/**
 * SECTION:rtsp-media-factory
 * @short_description: A factory for media pipelines
 * @see_also: #GstRTSPMountPoints, #GstRTSPMedia
 *
 * The #GstRTSPMediaFactoryWFD is responsible for creating or recycling
 * #GstRTSPMedia objects based on the passed URL.
 *
 * The default implementation of the object can create #GstRTSPMedia objects
 * containing a pipeline created from a launch description set with
 * gst_rtsp_media_factory_wfd_set_launch().
 *
 * Media from a factory can be shared by setting the shared flag with
 * gst_rtsp_media_factory_wfd_set_shared(). When a factory is shared,
 * gst_rtsp_media_factory_wfd_construct() will return the same #GstRTSPMedia when
 * the url matches.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include <stdio.h>
#include <string.h>

#include "rtsp-media-factory-wfd.h"
#include "gstwfdmessage.h"

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFDPrivate))

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)       (&(GST_RTSP_MEDIA_FACTORY_WFD_CAST(f)->priv->lock))
#define GST_RTSP_MEDIA_FACTORY_WFD_LOCK(f)           (g_mutex_lock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))
#define GST_RTSP_MEDIA_FACTORY_WFD_UNLOCK(f)         (g_mutex_unlock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))

typedef struct _GstRTPSMediaWFDTypeFindResult GstRTPSMediaWFDTypeFindResult;

struct _GstRTPSMediaWFDTypeFindResult{
  gint h264_found;
  gint aac_found;
  gint ac3_found;
  GstElementFactory *demux_fact;
  GstElementFactory *src_fact;
};

typedef struct _GstRTSPMediaWFDDirectPipelineData GstRTSPMediaWFDDirectPipelineData;

struct _GstRTSPMediaWFDDirectPipelineData {
  GstBin *pipeline;
  GstElement *ap;
  GstElement *vp;
  GstElement *aq;
  GstElement *vq;
  GstElement *tsmux;
  GstElement *mux_fs;
  gchar *uri;
};


struct _GstRTSPMediaFactoryWFDPrivate
{
  GMutex lock;
  GstRTSPPermissions *permissions;
  gchar *launch;
  gboolean shared;
  GstRTSPLowerTrans protocols;
  guint buffer_size;
  guint mtu_size;

  guint8 videosrc_type;
  guint8 video_codec;
  gchar *video_encoder;
  guint video_bitrate;
  guint video_width;
  guint video_height;
  guint video_framerate;
  guint video_enc_skip_inbuf_value;
  GstElement *video_queue;
  GstBin *video_srcbin;

  gchar *audio_device;
  gchar *audio_encoder_aac;
  gchar *audio_encoder_ac3;
  guint8 audio_codec;
  guint64 audio_latency_time;
  guint64 audio_buffer_time;
  gboolean audio_do_timestamp;
  guint8 audio_channels;
  guint8 audio_freq;
  guint8 audio_bitrate;
  GstElement *audio_queue;
  GstBin *audio_srcbin;

  GMutex direct_lock;
  GCond direct_cond;
  GType decodebin_type;
  GstBin *discover_pipeline;
  GstRTPSMediaWFDTypeFindResult res;
  GstRTSPMediaWFDDirectPipelineData *direct_pipe;
  GstBin *stream_bin;
  GstElement *mux;
  GstElement *mux_queue;
  GstElement *pay;
  GstElement *stub_fs;
  GMainLoop *discover_loop;

  guint64 video_resolution_supported;

  gboolean dump_ts;
};

#define DEFAULT_LAUNCH          NULL
#define DEFAULT_SHARED          FALSE
#define DEFAULT_PROTOCOLS       GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | \
                                        GST_RTSP_LOWER_TRANS_TCP
#define DEFAULT_BUFFER_SIZE     0x80000

enum
{
  PROP_0,
  PROP_LAUNCH,
  PROP_SHARED,
  PROP_SUSPEND_MODE,
  PROP_EOS_SHUTDOWN,
  PROP_PROTOCOLS,
  PROP_BUFFER_SIZE,
  PROP_LAST
};

enum
{
  SIGNAL_MEDIA_CONSTRUCTED,
  SIGNAL_MEDIA_CONFIGURE,
  SIGNAL_DIRECT_STREAMING_END,
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_media_wfd_debug);
#define GST_CAT_DEFAULT rtsp_media_wfd_debug

static guint gst_rtsp_media_factory_wfd_signals[SIGNAL_LAST] = { 0 };

static void gst_rtsp_media_factory_wfd_get_property (GObject * object,
    guint propid, GValue * value, GParamSpec * pspec);
static void gst_rtsp_media_factory_wfd_set_property (GObject * object,
    guint propid, const GValue * value, GParamSpec * pspec);

static void gst_rtsp_media_factory_wfd_finalize (GObject * obj);


static GstElement *rtsp_media_factory_wfd_create_element (GstRTSPMediaFactory *
    factory, const GstRTSPUrl * url);
static GstRTSPMedia *rtsp_media_factory_wfd_construct (GstRTSPMediaFactory *
    factory, const GstRTSPUrl * url);

G_DEFINE_TYPE (GstRTSPMediaFactoryWFD, gst_rtsp_media_factory_wfd,
    GST_TYPE_RTSP_MEDIA_FACTORY);

static void
gst_rtsp_media_factory_wfd_class_init (GstRTSPMediaFactoryWFDClass * klass)
{
  GObjectClass *gobject_class;
  GstRTSPMediaFactoryClass *factory_class;

  g_type_class_add_private (klass, sizeof (GstRTSPMediaFactoryWFDPrivate));

  gobject_class = G_OBJECT_CLASS (klass);
  factory_class = GST_RTSP_MEDIA_FACTORY_CLASS (klass);

  gobject_class->get_property = gst_rtsp_media_factory_wfd_get_property;
  gobject_class->set_property = gst_rtsp_media_factory_wfd_set_property;
  gobject_class->finalize = gst_rtsp_media_factory_wfd_finalize;

  gst_rtsp_media_factory_wfd_signals[SIGNAL_DIRECT_STREAMING_END] =
      g_signal_new ("direct-stream-end", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPMediaFactoryWFDClass,
          direct_stream_end), NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 0, G_TYPE_NONE);

  factory_class->construct = rtsp_media_factory_wfd_construct;
  factory_class->create_element = rtsp_media_factory_wfd_create_element;

  GST_DEBUG_CATEGORY_INIT (rtsp_media_wfd_debug, "rtspmediafactorywfd", 0,
      "GstRTSPMediaFactoryWFD");
}

void  gst_rtsp_media_factory_wfd_set (GstRTSPMediaFactoryWFD * factory,
    guint8 videosrc_type, gchar *audio_device, guint64 audio_latency_time,
    guint64 audio_buffer_time, gboolean audio_do_timestamp, guint mtu_size)
{
  GstRTSPMediaFactoryWFDPrivate *priv =
      GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE (factory);
  factory->priv = priv;

  priv->videosrc_type = videosrc_type;
  priv->audio_device = audio_device;
  priv->audio_latency_time = audio_latency_time;
  priv->audio_buffer_time = audio_buffer_time;
  priv->audio_do_timestamp = audio_do_timestamp;
  priv->mtu_size = mtu_size;
}

void  gst_rtsp_media_factory_wfd_set_encoders (GstRTSPMediaFactoryWFD * factory,
    gchar *video_encoder, gchar *audio_encoder_aac, gchar *audio_encoder_ac3)
{
  GstRTSPMediaFactoryWFDPrivate *priv =
      GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE (factory);
  factory->priv = priv;

  priv->video_encoder = video_encoder;
  priv->audio_encoder_aac = audio_encoder_aac;
  priv->audio_encoder_ac3 = audio_encoder_ac3;
}

void  gst_rtsp_media_factory_wfd_set_dump_ts (GstRTSPMediaFactoryWFD * factory,
    gboolean dump_ts)
{
  GstRTSPMediaFactoryWFDPrivate *priv =
      GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE (factory);
  factory->priv = priv;

  priv->dump_ts = dump_ts;
}
void gst_rtsp_media_factory_wfd_set_negotiated_resolution (GstRTSPMediaFactory *factory,
   guint32 width, guint32 height)
{
  GstRTSPMediaFactoryWFD *factory_wfd = GST_RTSP_MEDIA_FACTORY_WFD (factory);
  GstRTSPMediaFactoryWFDPrivate *priv = factory_wfd->priv;

  priv->video_width = width;
  priv->video_height = height;
}
void gst_rtsp_media_factory_wfd_set_audio_codec (GstRTSPMediaFactory *factory,
   guint audio_codec)
{
  GstRTSPMediaFactoryWFD *factory_wfd = GST_RTSP_MEDIA_FACTORY_WFD (factory);
  GstRTSPMediaFactoryWFDPrivate *priv = factory_wfd->priv;

  priv->audio_codec = audio_codec;
}

static void
gst_rtsp_media_factory_wfd_init (GstRTSPMediaFactoryWFD * factory)
{
  GstRTSPMediaFactoryWFDPrivate *priv =
      GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE (factory);
  factory->priv = priv;

  priv->launch = g_strdup (DEFAULT_LAUNCH);
  priv->shared = DEFAULT_SHARED;
  priv->protocols = DEFAULT_PROTOCOLS;
  priv->buffer_size = DEFAULT_BUFFER_SIZE;

  //priv->videosrc_type = GST_WFD_VSRC_XIMAGESRC;
  //priv->videosrc_type = GST_WFD_VSRC_XVIMAGESRC;
  //priv->videosrc_type = GST_WFD_VSRC_CAMERASRC;
  priv->videosrc_type = GST_WFD_VSRC_VIDEOTESTSRC;
  priv->video_codec = GST_WFD_VIDEO_H264;
  priv->video_encoder = g_strdup ("omxh264enc");
  priv->video_bitrate = 200000;
  priv->video_width = 640;
  priv->video_height = 480;
  priv->video_framerate = 30;
  priv->video_enc_skip_inbuf_value = 5;
  priv->video_srcbin = NULL;

  priv->audio_device = g_strdup ("alsa_output.1.analog-stereo.monitor");
  priv->audio_codec = GST_WFD_AUDIO_AAC;
  priv->audio_encoder_aac = g_strdup ("avenc_aac");
  priv->audio_encoder_ac3 = g_strdup ("avenc_ac3");
  priv->audio_latency_time = 10000;
  priv->audio_buffer_time = 200000;
  priv->audio_do_timestamp = FALSE;
  priv->audio_channels = GST_WFD_CHANNEL_2;
  priv->audio_freq = GST_WFD_FREQ_48000;
  priv->audio_srcbin = NULL;

  g_mutex_init (&priv->direct_lock);
  g_cond_init (&priv->direct_cond);

  priv->discover_pipeline = NULL;
  priv->direct_pipe = NULL;
  memset (&priv->res, 0x00, sizeof (GstRTPSMediaWFDTypeFindResult));
  priv->stream_bin = NULL;
  priv->mux = NULL;
  priv->mux_queue = NULL;
  priv->pay = NULL;

  g_mutex_init (&priv->lock);
}

static void
gst_rtsp_media_factory_wfd_finalize (GObject * obj)
{
  GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (obj);
  GstRTSPMediaFactoryWFDPrivate *priv = factory->priv;

  if (priv->permissions)
    gst_rtsp_permissions_unref (priv->permissions);
  g_free (priv->launch);
  g_mutex_clear (&priv->lock);

  g_mutex_clear (&priv->direct_lock);
  g_cond_clear (&priv->direct_cond);


  if (priv->audio_device)
    g_free (priv->audio_device);
  if (priv->audio_encoder_aac)
    g_free (priv->audio_encoder_aac);
  if (priv->audio_encoder_ac3)
    g_free (priv->audio_encoder_ac3);

  if (priv->video_encoder)
    g_free (priv->video_encoder);

  G_OBJECT_CLASS (gst_rtsp_media_factory_wfd_parent_class)->finalize (obj);
}

GstRTSPMediaFactoryWFD *
gst_rtsp_media_factory_wfd_new (void)
{
  GstRTSPMediaFactoryWFD *result;

  result = g_object_new (GST_TYPE_RTSP_MEDIA_FACTORY_WFD, NULL);

  return result;
}

static void
gst_rtsp_media_factory_wfd_get_property (GObject * object,
    guint propid, GValue * value, GParamSpec * pspec)
{
  //GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_media_factory_wfd_set_property (GObject * object,
    guint propid, const GValue * value, GParamSpec * pspec)
{
  //GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static GstPadProbeReturn
rtsp_media_wfd_dump_data (GstPad * pad, GstPadProbeInfo *info, gpointer u_data)
{
  guint8 *data;
  gsize size;
  FILE *f;
  GstMapInfo mapinfo;

  if (info->type == (GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_PUSH)) {
    GstBuffer *buffer = gst_pad_probe_info_get_buffer (info);

    gst_buffer_map (buffer, &mapinfo, GST_MAP_READ);
    data = mapinfo.data;
    size = gst_buffer_get_size (buffer);

    f = fopen ("/root/probe.ts", "a");
    if (f != NULL) {
      fwrite (data, size, 1, f);
      fclose (f);
    }
    gst_buffer_unmap (buffer, &mapinfo);
  }

  return GST_PAD_PROBE_OK;
}

static gboolean
_rtsp_media_factory_wfd_create_audio_capture_bin (GstRTSPMediaFactoryWFD *
    factory, GstBin * srcbin)
{
  GstElement *audiosrc = NULL;
  GstElement *acaps = NULL;
  GstElement *acaps2 = NULL;
  GstElement *aenc = NULL;
  GstElement *audio_convert = NULL;
  GstElement *aqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstStructure *audio_properties_name = NULL;

  guint channels = 0;
  gboolean is_enc_req = TRUE;
  guint freq = 0;
  gchar *acodec = NULL;

  priv = factory->priv;

  priv->audio_srcbin = (GstBin *)gst_bin_new ("audio");
  /* create audio src element */
  audiosrc = gst_element_factory_make ("pulsesrc", "audiosrc");
  if (!audiosrc) {
    GST_ERROR_OBJECT (factory, "failed to create audiosrc element");
    goto create_error;
  }

  GST_INFO_OBJECT (factory, "audio device : %s", priv->audio_device);
  GST_INFO_OBJECT (factory, "audio latency time  : %"G_GUINT64_FORMAT,
      priv->audio_latency_time);
  GST_INFO_OBJECT (factory, "audio_buffer_time  : %"G_GUINT64_FORMAT,
      priv->audio_buffer_time);
  GST_INFO_OBJECT (factory, "audio_do_timestamp  : %d",
      priv->audio_do_timestamp);

  audio_properties_name = gst_structure_new_from_string(priv->audio_device);

  g_object_set (audiosrc, "stream-properties", audio_properties_name, NULL);
  g_object_set (audiosrc, "buffer-time", (gint64) priv->audio_buffer_time,
      NULL);
  g_object_set (audiosrc, "latency-time", (gint64) priv->audio_latency_time,
      NULL);
  g_object_set (audiosrc, "do-timestamp", (gboolean) priv->audio_do_timestamp,
      NULL);
  g_object_set (audiosrc, "provide-clock", (gboolean) FALSE, NULL);
  g_object_set (audiosrc, "is-live", (gboolean) TRUE, NULL);

  if (priv->audio_codec == GST_WFD_AUDIO_LPCM) {
    /* To meet miracast certification */
    gint64 block_size = 1920;
    g_object_set (audiosrc, "blocksize", (gint64) block_size, NULL);

    audio_convert = gst_element_factory_make ("capssetter", "audio_convert");
    if (NULL == audio_convert) {
      GST_ERROR_OBJECT (factory, "failed to create audio convert element");
      goto create_error;
    }
    g_object_set (audio_convert, "caps", gst_caps_new_simple("audio/x-lpcm",
              "width", G_TYPE_INT, 16,
              "rate", G_TYPE_INT, 48000,
              "channels", G_TYPE_INT, 2,
              "dynamic_range", G_TYPE_INT, 0,
              "emphasis", G_TYPE_BOOLEAN, FALSE,
              "mute", G_TYPE_BOOLEAN, FALSE, NULL), NULL);
    g_object_set (audio_convert, "join", (gboolean)FALSE, NULL);
    g_object_set (audio_convert, "replace", (gboolean)TRUE, NULL);

    acaps2 = gst_element_factory_make ("capsfilter", "audiocaps2");
    if (NULL == acaps2) {
      GST_ERROR_OBJECT (factory, "failed to create audio capsilfter element");
      goto create_error;
    }
    /* In case of LPCM, uses big endian */
        g_object_set (G_OBJECT (acaps2), "caps",
            gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "S16BE",
                /* In case of LPCM, uses big endian */
                "rate", G_TYPE_INT, 48000,
                "channels", G_TYPE_INT, 2, NULL), NULL);
  }

  /* create audio caps element */
  acaps = gst_element_factory_make ("capsfilter", "audiocaps");
  if (NULL == acaps) {
    GST_ERROR_OBJECT (factory, "failed to create audio capsilfter element");
    goto create_error;
  }

  if (priv->audio_channels == GST_WFD_CHANNEL_2)
    channels = 2;
  else if (priv->audio_channels == GST_WFD_CHANNEL_4)
    channels = 4;
  else if (priv->audio_channels == GST_WFD_CHANNEL_6)
    channels = 6;
  else if (priv->audio_channels == GST_WFD_CHANNEL_8)
    channels = 8;
  else
    channels = 2;

  if (priv->audio_freq == GST_WFD_FREQ_44100)
    freq = 44100;
  else if (priv->audio_freq == GST_WFD_FREQ_48000)
    freq = 48000;
  else
    freq = 44100;

  if (priv->audio_codec == GST_WFD_AUDIO_LPCM) {
    g_object_set (G_OBJECT (acaps), "caps",
        gst_caps_new_simple ("audio/x-lpcm", "width", G_TYPE_INT, 16,
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "dynamic_range", G_TYPE_INT, 0,
            "emphasis", G_TYPE_BOOLEAN, FALSE,
            "mute", G_TYPE_BOOLEAN, FALSE, NULL), NULL);
  } else if ((priv->audio_codec == GST_WFD_AUDIO_AAC)
      || (priv->audio_codec == GST_WFD_AUDIO_AC3)) {
    g_object_set (G_OBJECT (acaps), "caps", gst_caps_new_simple ("audio/x-raw",
            "endianness", G_TYPE_INT, 1234, "signed", G_TYPE_BOOLEAN, TRUE,
            "depth", G_TYPE_INT, 16, "rate", G_TYPE_INT, freq, "channels",
            G_TYPE_INT, channels, NULL), NULL);
  }

  if (priv->audio_codec == GST_WFD_AUDIO_AAC) {
    acodec = g_strdup (priv->audio_encoder_aac);
    is_enc_req = TRUE;
  } else if (priv->audio_codec == GST_WFD_AUDIO_AC3) {
    acodec = g_strdup (priv->audio_encoder_ac3);
    is_enc_req = TRUE;
  } else if (priv->audio_codec == GST_WFD_AUDIO_LPCM) {
    GST_DEBUG_OBJECT (factory, "No codec required, raw data will be sent");
    is_enc_req = FALSE;
  } else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  if (is_enc_req) {
    aenc = gst_element_factory_make (acodec, "audioenc");
    if (NULL == aenc) {
      GST_ERROR_OBJECT (factory, "failed to create audio encoder element");
      goto create_error;
    }

    g_object_set (aenc, "compliance", -2, NULL);
    g_object_set (aenc, "tolerance", 400000000, NULL);
    g_object_set (aenc, "bitrate", (guint) 128000, NULL);
    g_object_set (aenc, "rate-control", 2, NULL);

    aqueue = gst_element_factory_make ("queue", "audio-queue");
    if (!aqueue) {
      GST_ERROR_OBJECT (factory, "failed to create audio queue element");
      goto create_error;
    }

    gst_bin_add_many (priv->audio_srcbin, audiosrc, acaps, aenc, aqueue, NULL);
    gst_bin_add (srcbin, GST_ELEMENT (priv->audio_srcbin));

    if (!gst_element_link_many (audiosrc, acaps, aenc, aqueue, NULL)) {
      GST_ERROR_OBJECT (factory, "Failed to link audio src elements...");
      goto create_error;
    }
  } else {
    aqueue = gst_element_factory_make ("queue", "audio-queue");
    if (!aqueue) {
      GST_ERROR_OBJECT (factory, "failed to create audio queue element");
      goto create_error;
    }

    gst_bin_add_many (priv->audio_srcbin, audiosrc, acaps2, audio_convert, acaps, aqueue, NULL);
    gst_bin_add (srcbin, GST_ELEMENT (priv->audio_srcbin));

    if (!gst_element_link_many (audiosrc, acaps2, audio_convert, acaps, aqueue, NULL)) {
      GST_ERROR_OBJECT (factory, "Failed to link audio src elements...");
      goto create_error;
    }
  }

  priv->audio_queue = aqueue;
  if (acodec) g_free (acodec);
  if (audio_properties_name) gst_structure_free(audio_properties_name);
  return TRUE;

create_error:
  if (acodec) g_free (acodec);
  if (audio_properties_name) gst_structure_free(audio_properties_name);
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_videotest_bin (GstRTSPMediaFactoryWFD * factory,
    GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  GstElement *videoconvert = NULL;
  GstElement *venc_caps = NULL;
  gchar *vcodec = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;

  GST_INFO_OBJECT (factory, "picked videotestsrc as video source");
  priv->video_srcbin = (GstBin *)gst_bin_new ("video");

  videosrc = gst_element_factory_make ("videotestsrc", "videosrc");
  if (NULL == videosrc) {
    GST_ERROR_OBJECT (factory, "failed to create ximagesrc element");
    goto create_error;
  }

  /* create video caps element */
  vcaps = gst_element_factory_make ("capsfilter", "videocaps");
  if (NULL == vcaps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (vcaps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, "I420",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  /* create video convert element */
  videoconvert = gst_element_factory_make ("videoconvert", "videoconvert");
  if (NULL == videoconvert) {
    GST_ERROR_OBJECT (factory, "failed to create video videoconvert element");
    goto create_error;
  }

  venc_caps = gst_element_factory_make ("capsfilter", "venc_caps");
  if (NULL == venc_caps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (venc_caps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, "SN12",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  if (priv->video_codec == GST_WFD_VIDEO_H264)
    vcodec = g_strdup (priv->video_encoder);
  else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  venc = gst_element_factory_make (vcodec, "videoenc");
  if (vcodec) g_free (vcodec);

  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }

  g_object_set (venc, "aud", 0, NULL);
  g_object_set (venc, "byte-stream", 1, NULL);
  g_object_set (venc, "bitrate", 512, NULL);

  vparse = gst_element_factory_make ("h264parse", "videoparse");
  if (NULL == vparse) {
    GST_ERROR_OBJECT (factory, "failed to create h264 parse element");
    goto create_error;
  }
  g_object_set (vparse, "config-interval", 1, NULL);

  vqueue = gst_element_factory_make ("queue", "video-queue");
  if (!vqueue) {
    GST_ERROR_OBJECT (factory, "failed to create video queue element");
    goto create_error;
  }

  gst_bin_add_many (priv->video_srcbin, videosrc, vcaps, videoconvert, venc_caps, venc, vparse, vqueue, NULL);
  gst_bin_add (srcbin, GST_ELEMENT (priv->video_srcbin));
  if (!gst_element_link_many (videosrc, vcaps, videoconvert, venc_caps, venc, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;

  return TRUE;

create_error:
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_waylandsrc_bin (GstRTSPMediaFactoryWFD * factory,
    GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  gchar *vcodec = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;

  GST_INFO_OBJECT (factory, "picked waylandsrc as video source");
  priv->video_srcbin = (GstBin *)gst_bin_new ("video");

  videosrc = gst_element_factory_make ("waylandsrc", "videosrc");
  if (NULL == videosrc) {
    GST_ERROR_OBJECT (factory, "failed to create ximagesrc element");
    goto create_error;
  }

  /* create video caps element */
  vcaps = gst_element_factory_make ("capsfilter", "videocaps");
  if (NULL == vcaps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (vcaps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, "SN12",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  if (priv->video_codec == GST_WFD_VIDEO_H264)
    vcodec = g_strdup (priv->video_encoder);
  else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  venc = gst_element_factory_make (vcodec, "videoenc");
  if (vcodec) g_free (vcodec);

  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }

  g_object_set (venc, "aud", 0, NULL);
  g_object_set (venc, "byte-stream", 1, NULL);
  g_object_set (venc, "bitrate", 512, NULL);

  vparse = gst_element_factory_make ("h264parse", "videoparse");
  if (NULL == vparse) {
    GST_ERROR_OBJECT (factory, "failed to create h264 parse element");
    goto create_error;
  }
  g_object_set (vparse, "config-interval", 1, NULL);

  vqueue = gst_element_factory_make ("queue", "video-queue");
  if (!vqueue) {
    GST_ERROR_OBJECT (factory, "failed to create video queue element");
    goto create_error;
  }

  gst_bin_add_many (priv->video_srcbin, videosrc, vcaps, venc, vparse, vqueue, NULL);
  gst_bin_add (srcbin, GST_ELEMENT (priv->video_srcbin));
  if (!gst_element_link_many (videosrc, vcaps, venc, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;

  return TRUE;

create_error:
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_camera_capture_bin (GstRTSPMediaFactoryWFD *
    factory, GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  gchar *vcodec = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;
  priv->video_srcbin = (GstBin *)gst_bin_new ("video");

  videosrc = gst_element_factory_make ("camerasrc", "videosrc");
  if (NULL == videosrc) {
    GST_ERROR_OBJECT (factory, "failed to create camerasrc element");
    goto create_error;
  }

  /* create video caps element */
  vcaps = gst_element_factory_make ("capsfilter", "videocaps");
  if (NULL == vcaps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  GST_INFO_OBJECT (factory, "picked camerasrc as video source");
  g_object_set (G_OBJECT (vcaps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "format", G_TYPE_STRING, "SN12",
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  if (priv->video_codec == GST_WFD_VIDEO_H264)
    vcodec = g_strdup (priv->video_encoder);
  else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  venc = gst_element_factory_make (vcodec, "videoenc");
  if (vcodec) g_free (vcodec);

  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }

  g_object_set (venc, "bitrate", priv->video_bitrate, NULL);
  g_object_set (venc, "byte-stream", 1, NULL);
  g_object_set (venc, "append-dci", 1, NULL);

  vparse = gst_element_factory_make ("h264parse", "videoparse");
  if (NULL == vparse) {
    GST_ERROR_OBJECT (factory, "failed to create h264 parse element");
    goto create_error;
  }
  g_object_set (vparse, "config-interval", 1, NULL);

  vqueue = gst_element_factory_make ("queue", "video-queue");
  if (!vqueue) {
    GST_ERROR_OBJECT (factory, "failed to create video queue element");
    goto create_error;
  }

  gst_bin_add_many (priv->video_srcbin, videosrc, vcaps, venc, vparse, vqueue, NULL);
  gst_bin_add (srcbin, GST_ELEMENT (priv->video_srcbin));

  if (!gst_element_link_many (videosrc, vcaps, venc, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;

  return TRUE;

create_error:
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_xcapture_bin (GstRTSPMediaFactoryWFD * factory,
    GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  GstElement *venc_caps = NULL;
  GstElement *videoconvert = NULL, *videoscale = NULL;
  gchar *vcodec = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;

  GST_INFO_OBJECT (factory, "picked ximagesrc as video source");
  priv->video_srcbin = (GstBin *)gst_bin_new ("video");

  videosrc = gst_element_factory_make ("ximagesrc", "videosrc");
  if (NULL == videosrc) {
    GST_ERROR_OBJECT (factory, "failed to create ximagesrc element");
    goto create_error;
  }

  videoscale = gst_element_factory_make ("videoscale", "videoscale");
  if (NULL == videoscale) {
    GST_ERROR_OBJECT (factory, "failed to create videoscale element");
    goto create_error;
  }

  videoconvert = gst_element_factory_make ("videoconvert", "videoconvert");
  if (NULL == videoconvert) {
    GST_ERROR_OBJECT (factory, "failed to create videoconvert element");
    goto create_error;
  }

  /* create video caps element */
  vcaps = gst_element_factory_make ("capsfilter", "videocaps");
  if (NULL == vcaps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (vcaps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  if (priv->video_codec == GST_WFD_VIDEO_H264)
    vcodec = g_strdup (priv->video_encoder);
  else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  venc = gst_element_factory_make (vcodec, "videoenc");
  if (vcodec) g_free (vcodec);

  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }

  g_object_set (venc, "aud", 0, NULL);
  g_object_set (venc, "byte-stream", 1, NULL);
  g_object_set (venc, "bitrate", 512, NULL);

  venc_caps = gst_element_factory_make ("capsfilter", "venc_caps");
  if (NULL == venc_caps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (venc_caps), "caps",
      gst_caps_new_simple ("video/x-h264",
          "profile", G_TYPE_STRING, "baseline", NULL), NULL);

  vparse = gst_element_factory_make ("h264parse", "videoparse");
  if (NULL == vparse) {
    GST_ERROR_OBJECT (factory, "failed to create h264 parse element");
    goto create_error;
  }
  g_object_set (vparse, "config-interval", 1, NULL);

  vqueue = gst_element_factory_make ("queue", "video-queue");
  if (!vqueue) {
    GST_ERROR_OBJECT (factory, "failed to create video queue element");
    goto create_error;
  }

  gst_bin_add_many (priv->video_srcbin, videosrc, videoscale, videoconvert, vcaps, venc,
      venc_caps, vparse, vqueue, NULL);
  gst_bin_add (srcbin, GST_ELEMENT (priv->video_srcbin));
  if (!gst_element_link_many (videosrc, videoscale, videoconvert, vcaps, venc,
          venc_caps, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;

  return TRUE;

create_error:
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_xvcapture_bin (GstRTSPMediaFactoryWFD * factory,
    GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  gchar *vcodec = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;

  GST_INFO_OBJECT (factory, "picked xvimagesrc as video source");
  priv->video_srcbin = (GstBin *)gst_bin_new ("video");

  videosrc = gst_element_factory_make ("xvimagesrc", "videosrc");
  if (NULL == videosrc) {
    GST_ERROR_OBJECT (factory, "failed to create xvimagesrc element");
    goto create_error;
  }

  /* create video caps element */
  vcaps = gst_element_factory_make ("capsfilter", "videocaps");
  if (NULL == vcaps) {
    GST_ERROR_OBJECT (factory, "failed to create video capsilfter element");
    goto create_error;
  }

  g_object_set (G_OBJECT (vcaps), "caps",
      gst_caps_new_simple ("video/x-raw",
          "width", G_TYPE_INT, priv->video_width,
          "height", G_TYPE_INT, priv->video_height,
          "format", G_TYPE_STRING, "SN12",
          "framerate", GST_TYPE_FRACTION, priv->video_framerate, 1, NULL),
      NULL);

  if (priv->video_codec == GST_WFD_VIDEO_H264) {
    vcodec = g_strdup (priv->video_encoder);
  } else {
    GST_ERROR_OBJECT (factory, "Yet to support other than H264 format");
    goto create_error;
  }

  venc = gst_element_factory_make (vcodec, "videoenc");
  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }
  g_object_set (venc, "bitrate", priv->video_bitrate, NULL);
  g_object_set (venc, "byte-stream", 1, NULL);
  g_object_set (venc, "append-dci", 1, NULL);
  g_object_set (venc, "idr-period", 120, NULL);
  g_object_set (venc, "skip-inbuf", priv->video_enc_skip_inbuf_value, NULL);

  vparse = gst_element_factory_make ("h264parse", "videoparse");
  if (NULL == vparse) {
    GST_ERROR_OBJECT (factory, "failed to create h264 parse element");
    goto create_error;
  }
  g_object_set (vparse, "config-interval", 1, NULL);

  vqueue = gst_element_factory_make ("queue", "video-queue");
  if (!vqueue) {
    GST_ERROR_OBJECT (factory, "failed to create video queue element");
    goto create_error;
  }

  gst_bin_add_many (priv->video_srcbin, videosrc, vcaps, venc, vparse, vqueue, NULL);
  gst_bin_add (srcbin, GST_ELEMENT (priv->video_srcbin));
  if (!gst_element_link_many (videosrc, vcaps, venc, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;
  if (vcodec) g_free (vcodec);

  return TRUE;

create_error:
  if (vcodec) g_free (vcodec);
  return FALSE;
}

static GstElement *
_rtsp_media_factory_wfd_create_srcbin (GstRTSPMediaFactoryWFD * factory)
{
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  GstBin *srcbin = NULL;
  GstElement *mux = NULL;
  GstElement *mux_queue = NULL;
  GstElement *payload = NULL;
  GstPad *srcpad = NULL;
  GstPad *mux_vsinkpad = NULL;
  GstPad *mux_asinkpad = NULL;
  GstPad *ghost_pad = NULL;

  priv = factory->priv;

  /* create source bin */
  srcbin = GST_BIN (gst_bin_new ("srcbin"));
  if (!srcbin) {
    GST_ERROR_OBJECT (factory, "failed to create source bin...");
    goto create_error;
  }

  /* create video src element */
  switch (priv->videosrc_type) {
    case GST_WFD_VSRC_XIMAGESRC:
      if (!_rtsp_media_factory_wfd_create_xcapture_bin (factory, srcbin)) {
        GST_ERROR_OBJECT (factory, "failed to create xcapture bin...");
        goto create_error;
      }
      break;
    case GST_WFD_VSRC_XVIMAGESRC:
      if (!_rtsp_media_factory_wfd_create_xvcapture_bin (factory, srcbin)) {
        GST_ERROR_OBJECT (factory, "failed to create xvcapture bin...");
        goto create_error;
      }
      break;
    case GST_WFD_VSRC_CAMERASRC:
      if (!_rtsp_media_factory_wfd_create_camera_capture_bin (factory, srcbin)) {
        GST_ERROR_OBJECT (factory, "failed to create camera capture bin...");
        goto create_error;
      }
      break;
    case GST_WFD_VSRC_VIDEOTESTSRC:
      if (!_rtsp_media_factory_wfd_create_videotest_bin (factory, srcbin)) {
        GST_ERROR_OBJECT (factory, "failed to create videotestsrc bin...");
        goto create_error;
      }
      break;
    case GST_WFD_VSRC_WAYLANDSRC:
      if (!_rtsp_media_factory_wfd_create_waylandsrc_bin (factory, srcbin)) {
        GST_ERROR_OBJECT (factory, "failed to create videotestsrc bin...");
        goto create_error;
      }
      break;
    default:
      GST_ERROR_OBJECT (factory, "unknow mode selected...");
      goto create_error;
  }

  mux = gst_element_factory_make ("mpegtsmux", "tsmux");
  if (!mux) {
    GST_ERROR_OBJECT (factory, "failed to create muxer element");
    goto create_error;
  }

  g_object_set (mux, "wfd-mode", TRUE, NULL);

  mux_queue = gst_element_factory_make ("queue", "muxer-queue");
  if (!mux_queue) {
    GST_ERROR_OBJECT (factory, "failed to create muxer-queue element");
    goto create_error;
  }

  g_object_set (mux_queue, "max-size-buffers", 20000, NULL);

  payload = gst_element_factory_make ("rtpmp2tpay", "pay0");
  if (!payload) {
    GST_ERROR_OBJECT (factory, "failed to create payload element");
    goto create_error;
  }

  g_object_set (payload, "pt", 33, NULL);
  g_object_set (payload, "mtu", priv->mtu_size, NULL);
  g_object_set (payload, "rtp-flush", (gboolean) TRUE, NULL);

  gst_bin_add_many (srcbin, mux, mux_queue, payload, NULL);

  if (!gst_element_link_many (mux, mux_queue, payload, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link muxer & payload...");
    goto create_error;
  }

  /* request video sink pad from muxer, which has elementary pid 0x1011 */
  mux_vsinkpad = gst_element_get_request_pad (mux, "sink_4113");
  if (!mux_vsinkpad) {
    GST_ERROR_OBJECT (factory, "Failed to get sink pad from muxer...");
    goto create_error;
  }

  /* request srcpad from video queue */
  srcpad = gst_element_get_static_pad (priv->video_queue, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT (factory, "Failed to get srcpad from video queue...");
    goto create_error;
  }
  ghost_pad = gst_ghost_pad_new ("video_src", srcpad);
  gst_element_add_pad (GST_ELEMENT (priv->video_srcbin), ghost_pad);

  if (gst_pad_link (ghost_pad, mux_vsinkpad) != GST_PAD_LINK_OK) {
    GST_ERROR_OBJECT (factory,
        "Failed to link video queue src pad & muxer video sink pad...");
    goto create_error;
  }

  gst_object_unref (srcpad);
  srcpad = NULL;
  ghost_pad = NULL;

  /* create audio source elements & add to pipeline */
  if (!_rtsp_media_factory_wfd_create_audio_capture_bin (factory, srcbin))
    goto create_error;

  /* request audio sink pad from muxer, which has elementary pid 0x1100 */
  mux_asinkpad = gst_element_get_request_pad (mux, "sink_4352");
  if (!mux_asinkpad) {
    GST_ERROR_OBJECT (factory, "Failed to get sinkpad from muxer...");
    goto create_error;
  }

  /* request srcpad from audio queue */
  srcpad = gst_element_get_static_pad (priv->audio_queue, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT (factory, "Failed to get srcpad from audio queue...");
    goto create_error;
  }
  ghost_pad = gst_ghost_pad_new ("audio_src", srcpad);
  gst_element_add_pad (GST_ELEMENT (priv->audio_srcbin), ghost_pad);

  /* link audio queue's srcpad & muxer sink pad */
  if (gst_pad_link (ghost_pad, mux_asinkpad) != GST_PAD_LINK_OK) {
    GST_ERROR_OBJECT (factory,
        "Failed to link audio queue src pad & muxer audio sink pad...");
    goto create_error;
  }
  gst_object_unref (srcpad);

  if (priv->dump_ts)
  {
    GstPad *pad_probe = NULL;
    pad_probe = gst_element_get_static_pad (mux, "src");

    if (NULL == pad_probe) {
      GST_INFO_OBJECT (factory, "pad for probe not created");
    } else {
      GST_INFO_OBJECT (factory, "pad for probe SUCCESSFUL");
    }
    gst_pad_add_probe (pad_probe, GST_PAD_PROBE_TYPE_BUFFER,
        rtsp_media_wfd_dump_data, factory, NULL);
  }

  GST_DEBUG_OBJECT (factory, "successfully created source bin...");

  priv->stream_bin = srcbin;
  priv->mux = gst_object_ref (mux);
  priv->mux_queue = gst_object_ref (mux_queue);
  priv->pay = gst_object_ref (payload);

  return GST_ELEMENT_CAST (srcbin);

create_error:
  GST_ERROR_OBJECT (factory, "Failed to create pipeline");
  return NULL;
}

static GstElement *
rtsp_media_factory_wfd_create_element (GstRTSPMediaFactory * factory,
    const GstRTSPUrl * url)
{
  GstRTSPMediaFactoryWFD *_factory = GST_RTSP_MEDIA_FACTORY_WFD_CAST (factory);
  GstElement *element = NULL;

  GST_RTSP_MEDIA_FACTORY_WFD_LOCK (factory);

  element = _rtsp_media_factory_wfd_create_srcbin (_factory);

  GST_RTSP_MEDIA_FACTORY_WFD_UNLOCK (factory);

  return element;
}

static GstRTSPMedia *
rtsp_media_factory_wfd_construct (GstRTSPMediaFactory * factory,
    const GstRTSPUrl * url)
{
  GstRTSPMedia *media;
  GstElement *element, *pipeline;
  GstRTSPMediaFactoryClass *klass;

  klass = GST_RTSP_MEDIA_FACTORY_GET_CLASS (factory);

  if (!klass->create_pipeline)
    goto no_create;

  element = gst_rtsp_media_factory_create_element (factory, url);
  if (element == NULL)
    goto no_element;

  /* create a new empty media */
  media = gst_rtsp_media_new (element);

  gst_rtsp_media_collect_streams (media);

  pipeline = klass->create_pipeline (factory, media);
  if (pipeline == NULL)
    goto no_pipeline;

  return media;

  /* ERRORS */
no_create:
  {
    g_critical ("no create_pipeline function");
    return NULL;
  }
no_element:
  {
    g_critical ("could not create element");
    return NULL;
  }
no_pipeline:
  {
    g_critical ("can't create pipeline");
    g_object_unref (media);
    return NULL;
  }
}

gint type_detected = FALSE;
gint linked = FALSE;
static gint in_pad_probe;

static GstPadProbeReturn
_rtsp_media_factory_wfd_restore_pipe_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstPad *old_src = NULL;
  GstPad *sink = NULL;
  GstPad *old_sink = NULL;
  GstPad *new_src = NULL;
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstRTSPMediaWFDDirectPipelineData *pipe_data = NULL;

  if (!g_atomic_int_compare_and_exchange (&in_pad_probe, FALSE, TRUE))
    return GST_PAD_PROBE_OK;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;
  pipe_data = priv->direct_pipe;

  sink = gst_element_get_static_pad (priv->pay, "sink");
  old_src = gst_pad_get_peer (sink);
  gst_pad_unlink (old_src, sink);

  new_src = gst_element_get_static_pad (priv->mux_queue, "src");
  old_sink = gst_pad_get_peer (new_src);
  gst_pad_unlink (new_src, old_sink);
  gst_element_set_state (priv->stub_fs, GST_STATE_NULL);
  gst_bin_remove ((GstBin *)priv->stream_bin, priv->stub_fs);

  gst_pad_link (new_src, sink);
  gst_object_unref (new_src);
  gst_object_unref (old_sink);

  gst_element_set_state (GST_ELEMENT(pipe_data->pipeline), GST_STATE_PAUSED);

  /* signal that new pipeline linked */
  g_mutex_lock (&priv->direct_lock);
  linked = TRUE;
  g_cond_signal (&priv->direct_cond);
  g_mutex_unlock (&priv->direct_lock);

  return GST_PAD_PROBE_REMOVE;
}

static gboolean
_rtsp_media_factory_wfd_destroy_direct_pipe(void *user_data)
{
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstRTSPMediaWFDDirectPipelineData *pipe_data = NULL;
  GstPad *probe_pad;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;
  pipe_data = priv->direct_pipe;

  probe_pad = gst_element_get_static_pad (priv->pay, "sink");

  gst_element_sync_state_with_parent (GST_ELEMENT(priv->audio_srcbin));
  gst_element_sync_state_with_parent (GST_ELEMENT(priv->video_srcbin));
  gst_element_sync_state_with_parent (GST_ELEMENT(priv->mux));
  gst_element_sync_state_with_parent (GST_ELEMENT(priv->mux_queue));

  in_pad_probe = FALSE;
  linked = FALSE;
  gst_pad_add_probe (probe_pad, GST_PAD_PROBE_TYPE_IDLE, _rtsp_media_factory_wfd_restore_pipe_probe_cb, factory, NULL);

  g_mutex_lock (&factory->priv->direct_lock);
  while (linked != TRUE)
    g_cond_wait (&factory->priv->direct_cond, &factory->priv->direct_lock);
  g_mutex_unlock (&factory->priv->direct_lock);

  GST_DEBUG_OBJECT (factory, "Deleting pipeline");
  gst_element_set_state (GST_ELEMENT(pipe_data->pipeline), GST_STATE_NULL);
  gst_bin_remove ((GstBin *)priv->stream_bin, GST_ELEMENT(pipe_data->pipeline));
  g_free (pipe_data);
  g_signal_emit (factory,
      gst_rtsp_media_factory_wfd_signals[SIGNAL_DIRECT_STREAMING_END], 0, NULL);
  return FALSE;
}

static void
_rtsp_media_factory_wfd_demux_pad_added_cb (GstElement *element,
              GstPad     *pad,
              gpointer    data)
{
  GstPad *sinkpad = NULL;
  GstCaps *caps = gst_pad_get_current_caps (pad);
  gchar *pad_name = gst_pad_get_name (pad);
  gchar *pad_caps = gst_caps_to_string (caps);
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstRTSPMediaWFDDirectPipelineData *pipe_data = NULL;

  factory = (GstRTSPMediaFactoryWFD *) data;
  priv = factory->priv;
  pipe_data = priv->direct_pipe;

  if (g_strrstr (g_ascii_strdown(pad_caps, -1), "audio")) {
    sinkpad = gst_element_get_static_pad (pipe_data->ap, "sink");
    if (gst_pad_is_linked (sinkpad)) {
      gst_object_unref (sinkpad);
      GST_DEBUG_OBJECT (factory, "pad linked");
      return;
    }
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK)
      GST_DEBUG_OBJECT (factory, "can't link demux %s pad", pad_name);

    gst_object_unref (sinkpad);
    sinkpad = NULL;
  }
  if (g_strrstr (g_ascii_strdown(pad_caps, -1), "video")) {
    if (g_strrstr (g_ascii_strdown(pad_caps, -1), "h264")) {
      sinkpad = gst_element_get_static_pad (pipe_data->vp, "sink");
      if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK)
        GST_DEBUG_OBJECT (factory, "can't link demux %s pad", pad_name);

      gst_object_unref (sinkpad);
      sinkpad = NULL;
    }
  }

  g_free (pad_caps);
  g_free (pad_name);
}

static GstPadProbeReturn
_rtsp_media_factory_wfd_pay_pad_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstPad *old_src = NULL;
  GstPad *sink = NULL;
  GstPad *old_sink = NULL;
  GstPad *new_src = NULL;
  GstPad *fas_sink = NULL;
  GstPad *gp = NULL;
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstRTSPMediaWFDDirectPipelineData *pipe_data = NULL;

  if (!g_atomic_int_compare_and_exchange (&in_pad_probe, FALSE, TRUE))
    return GST_PAD_PROBE_OK;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;
  pipe_data = priv->direct_pipe;

  sink = gst_element_get_static_pad (priv->pay, "sink");
  old_src = gst_pad_get_peer (sink);
  gst_pad_unlink (old_src, sink);

  new_src = gst_element_get_static_pad (pipe_data->tsmux, "src");
  old_sink = gst_pad_get_peer (new_src);
  gst_pad_unlink (new_src, old_sink);
  gst_element_set_state (pipe_data->mux_fs, GST_STATE_NULL);
  gst_bin_remove ((GstBin *)pipe_data->pipeline, pipe_data->mux_fs);

  gp = gst_ghost_pad_new ("audio_file", new_src);
  gst_pad_set_active(gp,TRUE);
  gst_element_add_pad (GST_ELEMENT (pipe_data->pipeline), gp);
  gst_pad_link (gp, sink);
  gst_object_unref (new_src);
  gst_object_unref (old_sink);

  priv->stub_fs = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add (priv->stream_bin, priv->stub_fs);
  gst_element_sync_state_with_parent (priv->stub_fs);
  fas_sink = gst_element_get_static_pad (priv->stub_fs, "sink");
  gst_pad_link (old_src, fas_sink);
  gst_object_unref (old_src);
  gst_object_unref (fas_sink);
  gst_element_set_state (GST_ELEMENT(priv->audio_srcbin), GST_STATE_PAUSED);
  gst_element_set_state (GST_ELEMENT(priv->video_srcbin), GST_STATE_PAUSED);
  gst_element_set_state (GST_ELEMENT(priv->mux), GST_STATE_PAUSED);
  gst_element_set_state (GST_ELEMENT(priv->mux_queue), GST_STATE_PAUSED);

  /* signal that new pipeline linked */
  g_mutex_lock (&priv->direct_lock);
  linked = TRUE;
  g_cond_signal (&priv->direct_cond);
  g_mutex_unlock (&priv->direct_lock);

  return GST_PAD_PROBE_REMOVE;
}


static GstPadProbeReturn
_rtsp_media_factory_wfd_src_pad_probe_cb(GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);

  factory = (GstRTSPMediaFactoryWFD *) user_data;

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_INFO_OBJECT (factory, "Got event: %s in direct streaming", GST_EVENT_TYPE_NAME (event));
    info->data = NULL;
    info->data = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, gst_structure_new_empty ("fillEOS"));
    g_idle_add((GSourceFunc)_rtsp_media_factory_wfd_destroy_direct_pipe, factory);
    return GST_PAD_PROBE_REMOVE;
  }

  return GST_PAD_PROBE_OK;
}

static void
_rtsp_media_factory_wfd_create_direct_pipeline(GstRTSPMediaFactoryWFD * factory)
{
  GstElement *src = NULL;
  GstElement *demux = NULL;
  gchar *path = NULL;
  GstPad *srcpad = NULL;
  GstPad *mux_vsinkpad = NULL;
  GstPad *mux_asinkpad = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstRTSPMediaWFDDirectPipelineData *pipe_data = NULL;

  priv = factory->priv;
  pipe_data = priv->direct_pipe;

  pipe_data->pipeline = (GstBin *) gst_bin_new ("direct");

  src = gst_element_factory_create(priv->res.src_fact, NULL);
  demux = gst_element_factory_create(priv->res.demux_fact, NULL);
  pipe_data->ap = gst_element_factory_make ("aacparse", NULL);
  pipe_data->vp = gst_element_factory_make ("h264parse", NULL);
  pipe_data->aq = gst_element_factory_make ("queue", NULL);
  pipe_data->vq = gst_element_factory_make ("queue", NULL);
  pipe_data->tsmux = gst_element_factory_make ("mpegtsmux", NULL);
  pipe_data->mux_fs = gst_element_factory_make ("fakesink", NULL);

  if (src == NULL || demux == NULL) {
    GST_DEBUG_OBJECT (factory, "Not all element created");
    return;
  }

  if (g_strrstr (g_ascii_strdown(g_type_name(G_OBJECT_TYPE(src)), -1), "file")) {
    path = g_filename_from_uri (pipe_data->uri, NULL, NULL);
    if (path == NULL) {
      GST_DEBUG_OBJECT(factory, "No file path");
      return;
    }
    g_object_set (src, "location", path, NULL);
  } else
    g_object_set (src, "uri", pipe_data->uri, NULL);

  gst_bin_add_many (pipe_data->pipeline, src, demux, pipe_data->ap,
      pipe_data->vp, pipe_data->aq, pipe_data->vq,
      pipe_data->tsmux, pipe_data->mux_fs, NULL);

  if (!gst_element_link (src, demux)) {
    GST_DEBUG_OBJECT (factory, "Can't link src with demux");
    return;
  }

  if (!gst_element_link (pipe_data->ap, pipe_data->aq)) {
    GST_DEBUG_OBJECT (factory, "Can't link audio parse and queue");
    return;
  }

  if (!gst_element_link (pipe_data->vp, pipe_data->vq)) {
    GST_DEBUG_OBJECT (factory, "Can't link video parse and queue");
    return;
  }

  if (!gst_element_link (pipe_data->tsmux, pipe_data->mux_fs)) {
    GST_DEBUG_OBJECT (factory, "Can't link muxer and fakesink");
    return;
  }

  g_signal_connect_object (demux, "pad-added", G_CALLBACK (_rtsp_media_factory_wfd_demux_pad_added_cb), factory, 0);

  gst_bin_add (priv->stream_bin, GST_ELEMENT (pipe_data->pipeline));


  /* request video sink pad from muxer, which has elementary pid 0x1011 */
  mux_vsinkpad = gst_element_get_request_pad (pipe_data->tsmux, "sink_4113");
  if (!mux_vsinkpad) {
    GST_DEBUG_OBJECT (factory, "Failed to get sink pad from muxer...");
    return;
  }

  /* request srcpad from video queue */
  srcpad = gst_element_get_static_pad (pipe_data->vq, "src");
  if (!srcpad) {
    GST_DEBUG_OBJECT (factory, "Failed to get srcpad from video queue...");
  }

  if (gst_pad_link (srcpad, mux_vsinkpad) != GST_PAD_LINK_OK) {
    GST_DEBUG_OBJECT (factory, "Failed to link video queue src pad & muxer video sink pad...");
    return;
  }

  gst_object_unref (mux_vsinkpad);
  gst_object_unref (srcpad);
  srcpad = NULL;

  /* request audio sink pad from muxer, which has elementary pid 0x1100 */
  mux_asinkpad = gst_element_get_request_pad (pipe_data->tsmux, "sink_4352");
  if (!mux_asinkpad) {
    GST_DEBUG_OBJECT (factory, "Failed to get sinkpad from muxer...");
    return;
  }

  /* request srcpad from audio queue */
  srcpad = gst_element_get_static_pad (pipe_data->aq, "src");
  if (!srcpad) {
    GST_DEBUG_OBJECT (factory, "Failed to get srcpad from audio queue...");
    return;
  }

  /* link audio queue's srcpad & muxer sink pad */
  if (gst_pad_link (srcpad, mux_asinkpad) != GST_PAD_LINK_OK) {
    GST_DEBUG_OBJECT (factory, "Failed to link audio queue src pad & muxer audio sink pad...");
    return;
  }
  gst_object_unref (mux_asinkpad);
  gst_object_unref (srcpad);
  srcpad = NULL;

  gst_element_sync_state_with_parent (GST_ELEMENT (pipe_data->pipeline));

  srcpad = gst_element_get_static_pad (priv->pay, "sink");

  in_pad_probe = FALSE;
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_IDLE, _rtsp_media_factory_wfd_pay_pad_probe_cb, factory, NULL);
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, _rtsp_media_factory_wfd_src_pad_probe_cb, factory, NULL);
}

static void
_rtsp_media_factory_wfd_decodebin_element_added_cb (GstElement *decodebin,
        GstElement *child, void *user_data)
{
  gchar *elem_name = g_ascii_strdown(g_type_name(G_OBJECT_TYPE(child)), -1);
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;

  if (g_strrstr (elem_name, "h264"))
    priv->res.h264_found++;
  if (g_strrstr (elem_name, "aac"))
    priv->res.aac_found++;
  if (g_strrstr (elem_name, "ac3"))
    priv->res.ac3_found++;
  if (g_strrstr (elem_name, "demux"))
    priv->res.demux_fact = gst_element_get_factory(child);
}

static void
_rtsp_media_factory_wfd_uridecodebin_element_added_cb (GstElement *uridecodebin,
        GstElement *child, void *user_data)
{
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;

  if (g_strrstr (g_ascii_strdown(g_type_name(G_OBJECT_TYPE(child)), -1), "src"))
    priv->res.src_fact = gst_element_get_factory(child);

  if (G_OBJECT_TYPE(child) == priv->decodebin_type)
    g_signal_connect_object (child, "element-added",
        G_CALLBACK (_rtsp_media_factory_wfd_decodebin_element_added_cb), factory, 0);
}

static void
_rtsp_media_factory_wfd_discover_pad_added_cb (GstElement *uridecodebin, GstPad *pad,
    GstBin *pipeline)
{
  GstPad *sinkpad = NULL;
  GstCaps *caps;

  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);

  if (G_UNLIKELY (queue == NULL || sink == NULL))
    goto error;

  g_object_set (sink, "silent", TRUE, NULL);
  g_object_set (queue, "max-size-buffers", 1, "silent", TRUE, NULL);

  caps = gst_pad_query_caps (pad, NULL);

  sinkpad = gst_element_get_static_pad (queue, "sink");
  if (sinkpad == NULL)
    goto error;

  gst_caps_unref (caps);

  gst_bin_add_many (pipeline, queue, sink, NULL);

  if (!gst_element_link_pads_full (queue, "src", sink, "sink",
          GST_PAD_LINK_CHECK_NOTHING))
    goto error;
  if (!gst_element_sync_state_with_parent (sink))
    goto error;
  if (!gst_element_sync_state_with_parent (queue))
    goto error;

  if (gst_pad_link_full (pad, sinkpad,
          GST_PAD_LINK_CHECK_NOTHING) != GST_PAD_LINK_OK)
    goto error;
  gst_object_unref (sinkpad);

  return;

error:
  if (sinkpad)
    gst_object_unref (sinkpad);
  if (queue)
    gst_object_unref (queue);
  if (sink)
    gst_object_unref (sink);
  return;
}

static void
_rtsp_media_factory_wfd_uridecode_no_pad_cb (GstElement * uridecodebin, void * user_data)
{
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  factory = (GstRTSPMediaFactoryWFD *) user_data;
  priv = factory->priv;
  type_detected = TRUE;
  g_main_loop_quit (priv->discover_loop);
}

static void
_rtsp_media_factory_wfd_discover_pipe_bus_call (GstBus *bus,
          GstMessage *msg,
          gpointer data)
{
  GstRTSPMediaFactoryWFD *factory = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  factory = (GstRTSPMediaFactoryWFD *) data;
  priv = factory->priv;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      GST_ERROR_OBJECT (factory, "Error: %s", error->message);
      g_error_free (error);

      type_detected = FALSE;
      g_main_loop_quit (priv->discover_loop);
      break;
    }
    default:
      break;
  }
}

static gboolean
_rtsp_media_factory_wfd_find_media_type (GstRTSPMediaFactoryWFD * factory, gchar *uri)
{
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;
  GstElement *uridecode = NULL;
  GstElement *tmp = NULL;
  GstBus *bus;
  GMainContext *context;
  GSource *source;
  guint id;

  priv = factory->priv;

  context = g_main_context_new();
  priv->discover_loop = g_main_loop_new(context, FALSE);

  tmp = gst_element_factory_make ("decodebin", NULL);
  priv->decodebin_type = G_OBJECT_TYPE (tmp);
  gst_object_unref (tmp);

  /* if a URI was provided, use it instead of the default one */
  priv->discover_pipeline = (GstBin *) gst_pipeline_new ("Discover");
  uridecode = gst_element_factory_make("uridecodebin", "uri");
  g_object_set (G_OBJECT (uridecode), "uri", uri, NULL);
  gst_bin_add (priv->discover_pipeline, uridecode);
  if (priv->discover_pipeline == NULL || uridecode == NULL) {
    GST_INFO_OBJECT (factory, "Failed to create type find pipeline");
    type_detected = FALSE;
    return FALSE;
  }

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->discover_pipeline));
  source = gst_bus_create_watch (bus);
  gst_bus_add_signal_watch (bus);

  g_source_set_callback (source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
  id = g_source_attach (source, context);
  g_signal_connect_object (bus, "message",
      G_CALLBACK (_rtsp_media_factory_wfd_discover_pipe_bus_call), factory, 0);

  g_signal_connect_object (uridecode, "pad-added",
      G_CALLBACK (_rtsp_media_factory_wfd_discover_pad_added_cb), priv->discover_pipeline, 0);
  g_signal_connect_object (uridecode, "element-added",
            G_CALLBACK (_rtsp_media_factory_wfd_uridecodebin_element_added_cb),
            factory, 0);
  g_signal_connect_object (uridecode, "no-more-pads",
            G_CALLBACK (_rtsp_media_factory_wfd_uridecode_no_pad_cb), factory, 0);
  gst_element_set_state (GST_ELEMENT (priv->discover_pipeline), GST_STATE_PLAYING);

  g_main_loop_run(priv->discover_loop);

  gst_element_set_state (GST_ELEMENT (priv->discover_pipeline), GST_STATE_NULL);
  g_source_destroy(source);
  g_source_unref (source);
  g_main_loop_unref(priv->discover_loop);
  g_main_context_unref(context);
  gst_object_unref(bus);
  gst_object_unref (GST_OBJECT (priv->discover_pipeline));

  return TRUE;
}

gint
gst_rtsp_media_factory_wfd_set_direct_streaming(GstRTSPMediaFactory * factory,
    gint direct_streaming, gchar *filesrc)
{
  GstRTSPMediaFactoryWFD *_factory = GST_RTSP_MEDIA_FACTORY_WFD_CAST (factory);
  type_detected = FALSE;
  linked = FALSE;

  if (direct_streaming == 0) {
    _rtsp_media_factory_wfd_destroy_direct_pipe ((void *)_factory);

    GST_INFO_OBJECT (_factory, "Direct streaming bin removed");

    return GST_RTSP_OK;
  }

  _rtsp_media_factory_wfd_find_media_type (_factory, filesrc);

  if (type_detected == FALSE) {
    GST_ERROR_OBJECT (_factory, "Media type cannot be detected");
    return GST_RTSP_ERROR;
  }
  GST_INFO_OBJECT (_factory, "Media type detected");

  _factory->priv->direct_pipe = g_new0 (GstRTSPMediaWFDDirectPipelineData, 1);
  _factory->priv->direct_pipe->uri = g_strdup(filesrc);

  _rtsp_media_factory_wfd_create_direct_pipeline(_factory);

  g_mutex_lock (&_factory->priv->direct_lock);
  while (linked != TRUE)
    g_cond_wait (&_factory->priv->direct_cond, &_factory->priv->direct_lock);
  g_mutex_unlock (&_factory->priv->direct_lock);

  GST_INFO_OBJECT (_factory, "Direct streaming bin created");

  return GST_RTSP_OK;
}
