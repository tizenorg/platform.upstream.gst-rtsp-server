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

#include <gst/gst.h>
#include <gst/rtsp/gstrtspconnection.h>

#ifndef __GST_RTSP_WFD_CLIENT_H__
#define __GST_RTSP_WFD_CLIENT_H__

G_BEGIN_DECLS

typedef struct _GstRTSPWFDClient GstRTSPWFDClient;
typedef struct _GstRTSPWFDClientClass GstRTSPWFDClientClass;
typedef struct _GstRTSPWFDClientPrivate GstRTSPWFDClientPrivate;

#include "rtsp-context.h"
#include "rtsp-mount-points.h"
#include "rtsp-sdp.h"
#include "rtsp-auth.h"
#include "rtsp-client.h"
#include "gstwfdmessage.h"

#define GST_TYPE_RTSP_WFD_CLIENT              (gst_rtsp_wfd_client_get_type ())
#define GST_IS_RTSP_WFD_CLIENT(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_RTSP_WFD_CLIENT))
#define GST_IS_RTSP_WFD_CLIENT_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_RTSP_WFD_CLIENT))
#define GST_RTSP_WFD_CLIENT_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_RTSP_WFD_CLIENT, GstRTSPWFDClientClass))
#define GST_RTSP_WFD_CLIENT(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_RTSP_WFD_CLIENT, GstRTSPWFDClient))
#define GST_RTSP_WFD_CLIENT_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_RTSP_WFD_CLIENT, GstRTSPWFDClientClass))
#define GST_RTSP_WFD_CLIENT_CAST(obj)         ((GstRTSPWFDClient*)(obj))
#define GST_RTSP_WFD_CLIENT_CLASS_CAST(klass) ((GstRTSPWFDClientClass*)(klass))


/**
 *
 */
typedef enum {
  WFD_TRIGGER_SETUP,
  WFD_TRIGGER_PAUSE,
  WFD_TRIGGER_TEARDOWN,
  WFD_TRIGGER_PLAY
} GstWFDTriggerType;

/**
 * GstRTSPWFDClientSendFunc:
 * @client: a #GstRTSPWFDClient
 * @message: a #GstRTSPMessage
 * @close: close the connection
 * @user_data: user data when registering the callback
 *
 * This callback is called when @client wants to send @message. When @close is
 * %TRUE, the connection should be closed when the message has been sent.
 *
 * Returns: %TRUE on success.
 */
typedef gboolean (*GstRTSPWFDClientSendFunc)      (GstRTSPWFDClient *client,
                                                GstRTSPMessage *message,
                                                gboolean close,
                                                gpointer user_data);

/**
 * GstRTSPWFDClient:
 *
 * The client object represents the connection and its state with a client.
 */
struct _GstRTSPWFDClient {
  GstRTSPClient  parent;

  gint           supported_methods;
  /*< private >*/
  GstRTSPWFDClientPrivate *priv;
  gpointer _gst_reserved[GST_PADDING];
};

/**
 * GstRTSPWFDClientClass:
 * @create_sdp: called when the SDP needs to be created for media.
 * @configure_client_media: called when the stream in media needs to be configured.
 *    The default implementation will configure the blocksize on the payloader when
 *    spcified in the request headers.
 * @configure_client_transport: called when the client transport needs to be
 *    configured.
 * @params_set: set parameters. This function should also initialize the
 *    RTSP response(ctx->response) via a call to gst_rtsp_message_init_response()
 * @params_get: get parameters. This function should also initialize the
 *    RTSP response(ctx->response) via a call to gst_rtsp_message_init_response()
 *
 * The client class structure.
 */
struct _GstRTSPWFDClientClass {
  GstRTSPClientClass  parent_class;

  GstRTSPResult       (*prepare_resource) (GstRTSPWFDClient *client, GstRTSPContext *ctx);
  GstRTSPResult       (*confirm_resource) (GstRTSPWFDClient *client, GstRTSPContext *ctx);

  /* signals */
  void     (*wfd_options_request)         (GstRTSPWFDClient *client, GstRTSPContext *ctx);
  void     (*wfd_get_param_request)       (GstRTSPWFDClient *client, GstRTSPContext *ctx);

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GType                 gst_rtsp_wfd_client_get_type          (void);

GstRTSPWFDClient *    gst_rtsp_wfd_client_new               (void);

void                  gst_rtsp_wfd_client_set_host_address  (
                          GstRTSPWFDClient *client, const gchar * address);

void                  gst_rtsp_wfd_client_start_wfd(GstRTSPWFDClient *client);
GstRTSPResult         gst_rtsp_wfd_client_trigger_request (
                          GstRTSPWFDClient * client, GstWFDTriggerType type);

GstRTSPResult         gst_rtsp_wfd_client_set_video_supported_resolution (
                          GstRTSPWFDClient * client, guint64 supported_reso);
GstRTSPResult         gst_rtsp_wfd_client_set_video_native_resolution (
		                      GstRTSPWFDClient * client, guint64 native_reso);
GstRTSPResult         gst_rtsp_wfd_client_set_audio_codec (
		                      GstRTSPWFDClient * client, guint8 audio_codec);

guint     gst_rtsp_wfd_client_get_audio_codec(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_audio_freq(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_audio_channels(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_audio_bit_width(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_audio_latency(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_codec(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_native(GstRTSPWFDClient *client);
guint64   gst_rtsp_wfd_client_get_video_native_resolution(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_cea_resolution(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_vesa_resolution(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_hh_resolution(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_profile(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_level(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_latency(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_video_max_height(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_video_max_width(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_video_framerate(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_video_min_slice_size(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_video_slice_enc_params(GstRTSPWFDClient *client);
guint     gst_rtsp_wfd_client_get_video_framerate_control(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_rtp_port0(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_rtp_port1(GstRTSPWFDClient *client);
gboolean  gst_rtsp_wfd_client_get_edid_supported(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_edid_hresolution(GstRTSPWFDClient *client);
guint32   gst_rtsp_wfd_client_get_edid_vresolution(GstRTSPWFDClient *client);
gboolean  gst_rtsp_wfd_client_get_protection_enabled(GstRTSPWFDClient *client);

void gst_rtsp_wfd_client_set_audio_freq(GstRTSPWFDClient *client, guint freq);
void gst_rtsp_wfd_client_set_edid_supported(GstRTSPWFDClient *client, gboolean supported);
void gst_rtsp_wfd_client_set_edid_hresolution(GstRTSPWFDClient *client, guint32 reso);
void gst_rtsp_wfd_client_set_edid_vresolution(GstRTSPWFDClient *client, guint32 reso);
void gst_rtsp_wfd_client_set_protection_enabled(GstRTSPWFDClient *client, gboolean enable);
void gst_rtsp_wfd_client_set_keep_alive_flag(GstRTSPWFDClient *client, gboolean flag);
void gst_rtsp_wfd_client_set_aud_codec(GstRTSPWFDClient *client, guint acodec);
void gst_rtsp_wfd_client_set_audio_channels(GstRTSPWFDClient *client, guint channels);
void gst_rtsp_wfd_client_set_audio_bit_width(GstRTSPWFDClient *client, guint bwidth);
void gst_rtsp_wfd_client_set_audio_latency(GstRTSPWFDClient *client, guint latency);
void gst_rtsp_wfd_client_set_video_codec(GstRTSPWFDClient *client, guint vcodec);
void gst_rtsp_wfd_client_set_video_native(GstRTSPWFDClient *client, guint native);
void gst_rtsp_wfd_client_set_vid_native_resolution(GstRTSPWFDClient *client, guint64 res);
void gst_rtsp_wfd_client_set_video_cea_resolution(GstRTSPWFDClient *client, guint res);
void gst_rtsp_wfd_client_set_video_vesa_resolution(GstRTSPWFDClient *client, guint res);
void gst_rtsp_wfd_client_set_video_hh_resolution(GstRTSPWFDClient *client, guint res);
void gst_rtsp_wfd_client_set_video_profile(GstRTSPWFDClient *client, guint profile);
void gst_rtsp_wfd_client_set_video_level(GstRTSPWFDClient *client, guint level);
void gst_rtsp_wfd_client_set_video_latency(GstRTSPWFDClient *client, guint latency);
void gst_rtsp_wfd_client_set_video_max_height(GstRTSPWFDClient *client, guint32 height);
void gst_rtsp_wfd_client_set_video_max_width(GstRTSPWFDClient *client, guint32 width);
void gst_rtsp_wfd_client_set_video_framerate(GstRTSPWFDClient *client, guint32 framerate);
void gst_rtsp_wfd_client_set_video_min_slice_size(GstRTSPWFDClient *client, guint32 slice_size);
void gst_rtsp_wfd_client_set_video_slice_enc_params(GstRTSPWFDClient *client, guint32 enc_params);
void gst_rtsp_wfd_client_set_video_framerate_control(GstRTSPWFDClient *client, guint framerate);
void gst_rtsp_wfd_client_set_rtp_port0(GstRTSPWFDClient *client, guint32 port);
void gst_rtsp_wfd_client_set_rtp_port1(GstRTSPWFDClient *client, guint32 port);

/**
 * GstRTSPWFDClientSessionFilterFunc:
 * @client: a #GstRTSPWFDClient object
 * @sess: a #GstRTSPSession in @client
 * @user_data: user data that has been given to gst_rtsp_wfd_client_session_filter()
 *
 * This function will be called by the gst_rtsp_wfd_client_session_filter(). An
 * implementation should return a value of #GstRTSPFilterResult.
 *
 * When this function returns #GST_RTSP_FILTER_REMOVE, @sess will be removed
 * from @client.
 *
 * A return value of #GST_RTSP_FILTER_KEEP will leave @sess untouched in
 * @client.
 *
 * A value of #GST_RTSP_FILTER_REF will add @sess to the result #GList of
 * gst_rtsp_wfd_client_session_filter().
 *
 * Returns: a #GstRTSPFilterResult.
 */
typedef GstRTSPFilterResult (*GstRTSPWFDClientSessionFilterFunc)  (GstRTSPWFDClient *client,
                                                                GstRTSPSession *sess,
                                                                gpointer user_data);



G_END_DECLS

#endif /* __GST_RTSP_WFD_CLIENT_H__ */
