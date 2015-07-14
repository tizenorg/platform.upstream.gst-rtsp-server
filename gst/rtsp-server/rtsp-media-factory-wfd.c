/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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
#include "rtsp-media-factory-wfd.h"
#include "gstwfdmessage.h"

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFDPrivate))

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)       (&(GST_RTSP_MEDIA_FACTORY_WFD_CAST(f)->priv->lock))
#define GST_RTSP_MEDIA_FACTORY_WFD_LOCK(f)           (g_mutex_lock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))
#define GST_RTSP_MEDIA_FACTORY_WFD_UNLOCK(f)         (g_mutex_unlock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))

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
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_media_wfd_debug);
#define GST_CAT_DEFAULT rtsp_media_wfd_debug

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

  priv->audio_device = g_strdup ("alsa_output.1.analog-stereo.monitor");
  priv->audio_codec = GST_WFD_AUDIO_AAC;
  priv->audio_encoder_aac = g_strdup ("avenc_aac");
  priv->audio_encoder_ac3 = g_strdup ("avenc_ac3");
  priv->audio_latency_time = 10000;
  priv->audio_buffer_time = 200000;
  priv->audio_do_timestamp = FALSE;
  priv->audio_channels = GST_WFD_CHANNEL_2;
  priv->audio_freq = GST_WFD_FREQ_48000;

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

  guint channels = 0;
  gboolean is_enc_req = TRUE;
  guint freq = 0;
  gchar *acodec = NULL;

  priv = factory->priv;

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

  g_object_set (audiosrc, "device", priv->audio_device, NULL);
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

    gst_bin_add_many (srcbin, audiosrc, acaps, aenc, aqueue, NULL);

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

    gst_bin_add_many (srcbin, audiosrc, acaps2, audio_convert, acaps, aqueue, NULL);

    if (!gst_element_link_many (audiosrc, acaps2, audio_convert, acaps, aqueue, NULL)) {
      GST_ERROR_OBJECT (factory, "Failed to link audio src elements...");
      goto create_error;
    }
  }

  priv->audio_queue = aqueue;
  if (acodec) g_free (acodec);

  return TRUE;

create_error:
  if (acodec) g_free (acodec);
  return FALSE;
}

static gboolean
_rtsp_media_factory_wfd_create_videotest_bin (GstRTSPMediaFactoryWFD * factory,
    GstBin * srcbin)
{
  GstElement *videosrc = NULL;
  GstElement *vcaps = NULL;
  GstElement *venc_caps = NULL;
  gchar *vcodec = NULL;
  GstElement *venc = NULL;
  GstElement *vparse = NULL;
  GstElement *vqueue = NULL;
  GstRTSPMediaFactoryWFDPrivate *priv = NULL;

  priv = factory->priv;

  GST_INFO_OBJECT (factory, "picked videotestsrc as video source");

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

  gst_bin_add_many (srcbin, videosrc, vcaps, venc, venc_caps, vparse, vqueue, NULL);
  if (!gst_element_link_many (videosrc, vcaps, venc, venc_caps, vparse, vqueue, NULL)) {
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
  if (!venc) {
    GST_ERROR_OBJECT (factory, "failed to create video encoder element");
    goto create_error;
  }
  if (vcodec) g_free (vcodec);

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

  gst_bin_add_many (srcbin, videosrc, vcaps, venc, vparse, vqueue, NULL);

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

  gst_bin_add_many (srcbin, videosrc, videoscale, videoconvert, vcaps, venc,
      venc_caps, vparse, vqueue, NULL);
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
  if (vcodec) g_free (vcodec);

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

  gst_bin_add_many (srcbin, videosrc, vcaps, venc, vparse, vqueue, NULL);
  if (!gst_element_link_many (videosrc, vcaps, venc, vparse, vqueue, NULL)) {
    GST_ERROR_OBJECT (factory, "Failed to link video src elements...");
    goto create_error;
  }

  priv->video_queue = vqueue;

  return TRUE;

create_error:
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

  if (gst_pad_link (srcpad, mux_vsinkpad) != GST_PAD_LINK_OK) {
    GST_ERROR_OBJECT (factory,
        "Failed to link video queue src pad & muxer video sink pad...");
    goto create_error;
  }

  gst_object_unref (mux_vsinkpad);
  gst_object_unref (srcpad);
  srcpad = NULL;

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

  /* link audio queue's srcpad & muxer sink pad */
  if (gst_pad_link (srcpad, mux_asinkpad) != GST_PAD_LINK_OK) {
    GST_ERROR_OBJECT (factory,
        "Failed to link audio queue src pad & muxer audio sink pad...");
    goto create_error;
  }
  gst_object_unref (mux_asinkpad);
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
