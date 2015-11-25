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

#include <gst/gst.h>

#include "rtsp-media-factory.h"

#ifndef __GST_RTSP_MEDIA_FACTORY_WFD_H__
#define __GST_RTSP_MEDIA_FACTORY_WFD_H__

G_BEGIN_DECLS
/* types for the media factory */
#define GST_TYPE_RTSP_MEDIA_FACTORY_WFD              (gst_rtsp_media_factory_wfd_get_type ())
#define GST_IS_RTSP_MEDIA_FACTORY_WFD(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD))
#define GST_IS_RTSP_MEDIA_FACTORY_WFD_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_RTSP_MEDIA_FACTORY_WFD))
#define GST_RTSP_MEDIA_FACTORY_WFD_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFDClass))
#define GST_RTSP_MEDIA_FACTORY_WFD(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFD))
#define GST_RTSP_MEDIA_FACTORY_WFD_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFDClass))
#define GST_RTSP_MEDIA_FACTORY_WFD_CAST(obj)         ((GstRTSPMediaFactoryWFD*)(obj))
#define GST_RTSP_MEDIA_FACTORY_WFD_CLASS_CAST(klass) ((GstRTSPMediaFactoryWFDClass*)(klass))
    enum
{
  GST_WFD_VSRC_XIMAGESRC,
  GST_WFD_VSRC_XVIMAGESRC,
  GST_WFD_VSRC_CAMERASRC,
  GST_WFD_VSRC_VIDEOTESTSRC,
  GST_WFD_VSRC_WAYLANDSRC
};

typedef struct _GstRTSPMediaFactoryWFD GstRTSPMediaFactoryWFD;
typedef struct _GstRTSPMediaFactoryWFDClass GstRTSPMediaFactoryWFDClass;
typedef struct _GstRTSPMediaFactoryWFDPrivate GstRTSPMediaFactoryWFDPrivate;

/**
 * GstRTSPMediaFactoryWFD:
 *
 * The definition and logic for constructing the pipeline for a media. The media
 * can contain multiple streams like audio and video.
 */
struct _GstRTSPMediaFactoryWFD
{
  GstRTSPMediaFactory parent;

  /*< private > */
  GstRTSPMediaFactoryWFDPrivate *priv;
  gpointer _gst_reserved[GST_PADDING];
};

/**
 * GstRTSPMediaFactoryWFDClass:
 * @gen_key: convert @url to a key for caching shared #GstRTSPMedia objects.
 *       The default implementation of this function will use the complete URL
 *       including the query parameters to return a key.
 * @create_element: Construct and return a #GstElement that is a #GstBin containing
 *       the elements to use for streaming the media. The bin should contain
 *       payloaders pay\%d for each stream. The default implementation of this
 *       function returns the bin created from the launch parameter.
 * @construct: the vmethod that will be called when the factory has to create the
 *       #GstRTSPMedia for @url. The default implementation of this
 *       function calls create_element to retrieve an element and then looks for
 *       pay\%d to create the streams.
 * @create_pipeline: create a new pipeline or re-use an existing one and
 *       add the #GstRTSPMedia's element created by @construct to the pipeline.
 * @configure: configure the media created with @construct. The default
 *       implementation will configure the 'shared' property of the media.
 * @media_constructed: signal emited when a media was constructed
 * @media_configure: signal emited when a media should be configured
 *
 * The #GstRTSPMediaFactoryWFD class structure.
 */
struct _GstRTSPMediaFactoryWFDClass
{
  GstRTSPMediaFactoryClass parent_class;

  gchar *(*gen_key) (GstRTSPMediaFactoryWFD * factory, const GstRTSPUrl * url);

  GstElement *(*create_element) (GstRTSPMediaFactoryWFD * factory,
      const GstRTSPUrl * url);
  GstRTSPMedia *(*construct) (GstRTSPMediaFactoryWFD * factory,
      const GstRTSPUrl * url);
  GstElement *(*create_pipeline) (GstRTSPMediaFactoryWFD * factory,
      GstRTSPMedia * media);
  void (*configure) (GstRTSPMediaFactoryWFD * factory, GstRTSPMedia * media);

  /* signals */
  void (*media_constructed) (GstRTSPMediaFactoryWFD * factory,
      GstRTSPMedia * media);
  void (*media_configure) (GstRTSPMediaFactoryWFD * factory,
      GstRTSPMedia * media);

  /*< private > */
  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GType gst_rtsp_media_factory_wfd_get_type (void);

/* creating the factory */
GstRTSPMediaFactoryWFD *gst_rtsp_media_factory_wfd_new (void);
GstElement *gst_rtsp_media_factory_wfd_create_element (GstRTSPMediaFactoryWFD *
    factory, const GstRTSPUrl * url);

void  gst_rtsp_media_factory_wfd_set (GstRTSPMediaFactoryWFD * factory,
    guint8 videosrc_type, gchar *audio_device, guint64 audio_latency_time,
    guint64 audio_buffer_time, gboolean audio_do_timestamp, guint mtu_size);
void  gst_rtsp_media_factory_wfd_set_encoders (GstRTSPMediaFactoryWFD * factory,
    gchar *video_encoder, gchar *audio_encoder_aac, gchar *audio_encoder_ac3);
void  gst_rtsp_media_factory_wfd_set_dump_ts (GstRTSPMediaFactoryWFD * factory,
    gboolean dump_ts);
void gst_rtsp_media_factory_wfd_set_negotiated_resolution (GstRTSPMediaFactory *factory,
   guint32 width, guint32 height);
void gst_rtsp_media_factory_wfd_set_audio_codec (GstRTSPMediaFactory *factory,
    guint audio_codec);

G_END_DECLS
#endif /* __GST_RTSP_MEDIA_FACTORY_WFD_H__ */
