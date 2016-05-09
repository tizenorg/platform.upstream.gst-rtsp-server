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
 * SECTION:rtsp-client
 * @short_description: A client connection state
 * @see_also: #GstRTSPServer, #GstRTSPThreadPool
 *
 * The client object handles the connection with a client for as long as a TCP
 * connection is open.
 *
 * A #GstRTSPWFDClient is created by #GstRTSPServer when a new connection is
 * accepted and it inherits the #GstRTSPMountPoints, #GstRTSPSessionPool,
 * #GstRTSPAuth and #GstRTSPThreadPool from the server.
 *
 * The client connection should be configured with the #GstRTSPConnection using
 * gst_rtsp_wfd_client_set_connection() before it can be attached to a #GMainContext
 * using gst_rtsp_wfd_client_attach(). From then on the client will handle requests
 * on the connection.
 *
 * Use gst_rtsp_wfd_client_session_filter() to iterate or modify all the
 * #GstRTSPSession objects managed by the client object.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include <stdio.h>
#include <string.h>

#include "rtsp-client-wfd.h"
#include "rtsp-media-factory-wfd.h"
#include "rtsp-sdp.h"
#include "rtsp-params.h"

#define GST_RTSP_WFD_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_WFD_CLIENT, GstRTSPWFDClientPrivate))

typedef struct _GstRTSPClientRTPStats GstRTSPClientRTPStats;

struct _GstRTSPClientRTPStats {
  GstRTSPStream *stream;
  guint64 last_sent_bytes;
  guint64 sent_bytes;
  guint last_seqnum;
  guint seqnum;

  /* Info in RR (Receiver Report) */
  guint8 fraction_lost;
  guint32 cumulative_lost_num;
  guint16 max_seqnum;
  guint32 arrival_jitter;
  guint32 lsr;
  guint32 dlsr;
  guint32 rtt;
  guint resent_packets;
};

struct _GstRTSPWFDClientPrivate
{
  GstRTSPWFDClientSendFunc send_func;   /* protected by send_lock */
  gpointer send_data;           /* protected by send_lock */
  GDestroyNotify send_notify;   /* protected by send_lock */

  /* used to cache the media in the last requested DESCRIBE so that
   * we can pick it up in the next SETUP immediately */
  gchar *path;
  GstRTSPMedia *media;

  GList *transports;
  GList *sessions;

  guint8 m1_done;
  guint8 m3_done;
  guint8 m4_done;

  /* Host's URL info */
  gchar *host_address;

  /* Parameters for WIFI-DISPLAY */
  guint caCodec;
  guint8 audio_codec;
  guint cFreq;
  guint cChanels;
  guint cBitwidth;
  guint caLatency;
  guint cvCodec;
  guint cNative;
  guint64 cNativeResolution;
  guint64 video_resolution_supported;
  gint video_native_resolution;
  guint64 cCEAResolution;
  guint64 cVESAResolution;
  guint64 cHHResolution;
  guint cProfile;
  guint cLevel;
  guint32 cMaxHeight;
  guint32 cMaxWidth;
  guint32 cFramerate;
  guint32 cInterleaved;
  guint32 cmin_slice_size;
  guint32 cslice_enc_params;
  guint cframe_rate_control;
  guint cvLatency;
  guint ctrans;
  guint cprofile;
  guint clowertrans;
  guint32 crtp_port0;
  guint32 crtp_port1;

  gboolean protection_enabled;
  GstWFDHDCPProtection hdcp_version;
  guint32 hdcp_tcpport;

  gboolean edid_supported;
  guint32 edid_hres;
  guint32 edid_vres;

  gboolean keep_alive_flag;
  GMutex keep_alive_lock;

  /* RTP statistics */
  GstRTSPClientRTPStats stats;
  GMutex stats_lock;
  guint stats_timer_id;
  gboolean rtcp_stats_enabled;
};

#define DEFAULT_WFD_TIMEOUT 60
#define WFD_MOUNT_POINT "/wfd1.0/streamid=0"

enum
{
  SIGNAL_WFD_OPTIONS_REQUEST,
  SIGNAL_WFD_GET_PARAMETER_REQUEST,
  SIGNAL_WFD_KEEP_ALIVE_FAIL,
  SIGNAL_WFD_PLAYING_DONE,
  SIGNAL_WFD_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_wfd_client_debug);
#define GST_CAT_DEFAULT rtsp_wfd_client_debug

static guint gst_rtsp_client_wfd_signals[SIGNAL_WFD_LAST] = { 0 };

static void gst_rtsp_wfd_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_client_finalize (GObject * obj);

static gboolean handle_wfd_options_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean handle_wfd_set_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean handle_wfd_get_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);

static void send_generic_wfd_response (GstRTSPWFDClient * client,
    GstRTSPStatusCode code, GstRTSPContext * ctx);
static gchar *wfd_make_path_from_uri (GstRTSPClient * client,
    const GstRTSPUrl * uri);
static void wfd_options_request_done (GstRTSPWFDClient * client, GstRTSPContext *ctx);
static void wfd_get_param_request_done (GstRTSPWFDClient * client, GstRTSPContext *ctx);
static void handle_wfd_response (GstRTSPClient * client, GstRTSPContext * ctx);
static void handle_wfd_play (GstRTSPClient * client, GstRTSPContext * ctx);
static void wfd_set_keep_alive_condition(GstRTSPWFDClient * client);
static gboolean wfd_ckeck_keep_alive_response (gpointer userdata);
static gboolean keep_alive_condition(gpointer userdata);
static gboolean wfd_configure_client_media (GstRTSPClient * client, GstRTSPMedia * media,
    GstRTSPStream * stream, GstRTSPContext * ctx);

GstRTSPResult prepare_trigger_request (GstRTSPWFDClient * client,
    GstRTSPMessage * request, GstWFDTriggerType trigger_type, gchar * url);

GstRTSPResult
prepare_response (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMessage * response, GstRTSPMethod method);

static GstRTSPResult handle_M1_message (GstRTSPWFDClient * client);
static GstRTSPResult handle_M3_message (GstRTSPWFDClient * client);
static GstRTSPResult handle_M4_message (GstRTSPWFDClient * client);
static GstRTSPResult handle_M16_message (GstRTSPWFDClient * client);

G_DEFINE_TYPE (GstRTSPWFDClient, gst_rtsp_wfd_client, GST_TYPE_RTSP_CLIENT);

static void
gst_rtsp_wfd_client_class_init (GstRTSPWFDClientClass * klass)
{
  GObjectClass *gobject_class;
  GstRTSPClientClass *rtsp_client_class;

  g_type_class_add_private (klass, sizeof (GstRTSPWFDClientPrivate));

  gobject_class = G_OBJECT_CLASS (klass);
  rtsp_client_class = GST_RTSP_CLIENT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_wfd_client_get_property;
  gobject_class->set_property = gst_rtsp_wfd_client_set_property;
  gobject_class->finalize = gst_rtsp_wfd_client_finalize;

  //klass->create_sdp = create_sdp;
  //klass->configure_client_transport = default_configure_client_transport;
  //klass->params_set = default_params_set;
  //klass->params_get = default_params_get;

  rtsp_client_class->handle_options_request = handle_wfd_options_request;
  rtsp_client_class->handle_set_param_request = handle_wfd_set_param_request;
  rtsp_client_class->handle_get_param_request = handle_wfd_get_param_request;
  rtsp_client_class->make_path_from_uri = wfd_make_path_from_uri;
  rtsp_client_class->configure_client_media = wfd_configure_client_media;

  rtsp_client_class->handle_response = handle_wfd_response;
  rtsp_client_class->play_request = handle_wfd_play;

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_OPTIONS_REQUEST] =
      g_signal_new ("wfd-options-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPWFDClientClass,
          wfd_options_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, GST_TYPE_RTSP_CONTEXT);

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_GET_PARAMETER_REQUEST] =
      g_signal_new ("wfd-get-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPWFDClientClass,
          wfd_get_param_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, GST_TYPE_RTSP_CONTEXT);

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_KEEP_ALIVE_FAIL] =
      g_signal_new ("wfd-keep-alive-fail", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPWFDClientClass, wfd_keep_alive_fail), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_PLAYING_DONE] =
      g_signal_new ("wfd-playing-done", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPWFDClientClass, wfd_playing_done), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

  klass->wfd_options_request = wfd_options_request_done;
  klass->wfd_get_param_request = wfd_get_param_request_done;

  GST_DEBUG_CATEGORY_INIT (rtsp_wfd_client_debug, "rtspwfdclient", 0,
      "GstRTSPWFDClient");
}

static void
gst_rtsp_wfd_client_init (GstRTSPWFDClient * client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (priv != NULL);

  client->priv = priv;
  priv->protection_enabled = FALSE;
  priv->video_native_resolution = GST_WFD_VIDEO_CEA_RESOLUTION;
  priv->video_resolution_supported = GST_WFD_CEA_640x480P60;
  priv->audio_codec = GST_WFD_AUDIO_AAC;
  priv->keep_alive_flag = FALSE;

  g_mutex_init (&priv->keep_alive_lock);
  g_mutex_init (&priv->stats_lock);

  priv->host_address = NULL;

  priv->stats_timer_id = -1;
  priv->rtcp_stats_enabled = FALSE;
  memset (&priv->stats, 0x00, sizeof (GstRTSPClientRTPStats));

  GST_INFO_OBJECT (client, "Client is initialized");
}

/* A client is finalized when the connection is broken */
static void
gst_rtsp_wfd_client_finalize (GObject * obj)
{
  GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (obj);
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (GST_IS_RTSP_WFD_CLIENT (obj));
  g_return_if_fail (priv != NULL);

  GST_INFO ("finalize client %p", client);

  if (priv->host_address)
    g_free (priv->host_address);

  if (priv->stats_timer_id > 0)
    g_source_remove(priv->stats_timer_id);

  g_mutex_clear (&priv->keep_alive_lock);
  g_mutex_clear (&priv->stats_lock);
  G_OBJECT_CLASS (gst_rtsp_wfd_client_parent_class)->finalize (obj);
}

static void
gst_rtsp_wfd_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_wfd_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/**
 * gst_rtsp_wfd_client_new:
 *
 * Create a new #GstRTSPWFDClient instance.
 *
 * Returns: a new #GstRTSPWFDClient
 */
GstRTSPWFDClient *
gst_rtsp_wfd_client_new (void)
{
  GstRTSPWFDClient *result;

  result = g_object_new (GST_TYPE_RTSP_WFD_CLIENT, NULL);

  return result;
}

void
gst_rtsp_wfd_client_start_wfd (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GST_INFO_OBJECT (client, "gst_rtsp_wfd_client_start_wfd");

  res = handle_M1_message (client);
  if (res < GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "handle_M1_message failed : %d", res);
  }

  return;
}

static gboolean
wfd_display_rtp_stats (gpointer userdata)
{
  guint16 seqnum = 0;
  guint64 bytes = 0;

  GstRTSPWFDClient *client = NULL;
  GstRTSPWFDClientPrivate *priv = NULL;

  client = (GstRTSPWFDClient *) userdata;
  priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  if (!priv) {
    GST_ERROR("No priv");
    return FALSE;
  }

  g_mutex_lock(&priv->stats_lock);

  seqnum = gst_rtsp_stream_get_current_seqnum (priv->stats.stream);
  bytes = gst_rtsp_stream_get_udp_sent_bytes (priv->stats.stream);

  GST_INFO ("----------------------------------------------------\n");
  GST_INFO ("Sent RTP packets : %d", seqnum - priv->stats.last_seqnum);
  GST_INFO ("Sent Bytes of RTP packets : %lld bytes", bytes - priv->stats.last_sent_bytes);

  priv->stats.last_seqnum = seqnum;
  priv->stats.last_sent_bytes = bytes;

  if (priv->rtcp_stats_enabled) {
    GST_INFO ("Fraction Lost: %d", priv->stats.fraction_lost);
    GST_INFO ("Cumulative number of packets lost: %d", priv->stats.cumulative_lost_num);
    GST_INFO ("Extended highest sequence number received: %d", priv->stats.max_seqnum);
    GST_INFO ("Interarrival Jitter: %d", priv->stats.arrival_jitter);
    GST_INFO ("Round trip time : %d", priv->stats.rtt);
  }

  GST_INFO ("----------------------------------------------------\n");

  g_mutex_unlock(&priv->stats_lock);

  return TRUE;
}

static void
on_rtcp_stats (GstRTSPStream *stream, GstStructure *stats, GstRTSPClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  guint fraction_lost, exthighestseq, jitter, lsr, dlsr, rtt;
  gint packetslost;

  if (!priv) return;

  g_mutex_lock(&priv->stats_lock);

  gst_structure_get_uint (stats, "rb-fractionlost", &fraction_lost);
  gst_structure_get_int (stats, "rb-packetslost", &packetslost);
  gst_structure_get_uint (stats, "rb-exthighestseq", &exthighestseq);
  gst_structure_get_uint (stats, "rb-jitter", &jitter);
  gst_structure_get_uint (stats, "rb-lsr", &lsr);
  gst_structure_get_uint (stats, "rb-dlsr", &dlsr);
  gst_structure_get_uint (stats, "rb-round-trip", &rtt);

  if (!priv->rtcp_stats_enabled)
    priv->rtcp_stats_enabled = TRUE;

  priv->stats.stream = stream;
  priv->stats.fraction_lost = (guint8)fraction_lost;
  priv->stats.cumulative_lost_num += (guint32)fraction_lost;
  priv->stats.max_seqnum = (guint16)exthighestseq;
  priv->stats.arrival_jitter = (guint32)jitter;
  priv->stats.lsr = (guint32)lsr;
  priv->stats.dlsr = (guint32)dlsr;
  priv->stats.rtt = (guint32)rtt;

  g_mutex_unlock(&priv->stats_lock);
}

static gboolean
wfd_configure_client_media (GstRTSPClient * client, GstRTSPMedia * media,
    GstRTSPStream * stream, GstRTSPContext * ctx)
{
  if (stream) {
    GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
    if (priv)
      priv->stats.stream = stream;
    g_signal_connect (stream, "rtcp-statistics", (GCallback) on_rtcp_stats, client);
  }

  return GST_RTSP_CLIENT_CLASS (gst_rtsp_wfd_client_parent_class)->configure_client_media (client, media, stream, ctx);
}
static void
wfd_options_request_done (GstRTSPWFDClient * client, GstRTSPContext *ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPWFDClientClass *klass = GST_RTSP_WFD_CLIENT_GET_CLASS (client);

  g_return_if_fail (klass != NULL);

  GST_INFO_OBJECT (client, "M2 done..");

  res = handle_M3_message (client);
  if (res < GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "handle_M3_message failed : %d", res);
  }

  if (klass->prepare_resource) {
    klass->prepare_resource (client, ctx);
  }

  return;
}

static void
wfd_get_param_request_done (GstRTSPWFDClient * client, GstRTSPContext *ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  GstRTSPWFDClientClass *klass = GST_RTSP_WFD_CLIENT_GET_CLASS (client);

  g_return_if_fail (priv != NULL && klass != NULL);

  priv->m3_done = TRUE;
  GST_INFO_OBJECT (client, "M3 done..");

  res = handle_M4_message (client);
  if (res < GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "handle_M4_message failed : %d", res);
  }

  if (klass->confirm_resource) {
    klass->confirm_resource (client, ctx);
  }

  return;
}

static guint
wfd_get_prefered_audio_codec (guint8 srcAudioCodec,
    guint sinkAudioCodec)
{
  int i = 0;
  guint codec = 0;
  for (i = 0; i < 8; i++) {
    if (((sinkAudioCodec << i) & 0x80)
        && ((srcAudioCodec << i) & 0x80)) {
      codec = (0x01 << (7 - i));
      break;
    }
  }
  return codec;
}

static guint64
wfd_get_prefered_resolution (guint64 srcResolution,
    guint64 sinkResolution,
    GstWFDVideoNativeResolution native,
    guint32 * cMaxWidth,
    guint32 * cMaxHeight, guint32 * cFramerate, guint32 * interleaved)
{
  int i = 0;
  guint64 resolution = 0;
  for (i = 0; i < 32; i++) {
    if (((sinkResolution << i) & 0x80000000)
        && ((srcResolution << i) & 0x80000000)) {
      resolution = ((guint64) 0x00000001 << (31 - i));
      break;
    }
  }
  switch (native) {
    case GST_WFD_VIDEO_CEA_RESOLUTION:
    {
      switch (resolution) {
        case GST_WFD_CEA_640x480P60:
          *cMaxWidth = 640;
          *cMaxHeight = 480;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_720x480P60:
          *cMaxWidth = 720;
          *cMaxHeight = 480;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_720x480I60:
          *cMaxWidth = 720;
          *cMaxHeight = 480;
          *cFramerate = 60;
          *interleaved = 1;
          break;
        case GST_WFD_CEA_720x576P50:
          *cMaxWidth = 720;
          *cMaxHeight = 576;
          *cFramerate = 50;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_720x576I50:
          *cMaxWidth = 720;
          *cMaxHeight = 576;
          *cFramerate = 50;
          *interleaved = 1;
          break;
        case GST_WFD_CEA_1280x720P30:
          *cMaxWidth = 1280;
          *cMaxHeight = 720;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1280x720P60:
          *cMaxWidth = 1280;
          *cMaxHeight = 720;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080P30:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080P60:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080I60:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 60;
          *interleaved = 1;
          break;
        case GST_WFD_CEA_1280x720P25:
          *cMaxWidth = 1280;
          *cMaxHeight = 720;
          *cFramerate = 25;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1280x720P50:
          *cMaxWidth = 1280;
          *cMaxHeight = 720;
          *cFramerate = 50;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080P25:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 25;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080P50:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 50;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080I50:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 50;
          *interleaved = 1;
          break;
        case GST_WFD_CEA_1280x720P24:
          *cMaxWidth = 1280;
          *cMaxHeight = 720;
          *cFramerate = 24;
          *interleaved = 0;
          break;
        case GST_WFD_CEA_1920x1080P24:
          *cMaxWidth = 1920;
          *cMaxHeight = 1080;
          *cFramerate = 24;
          *interleaved = 0;
          break;
        default:
          *cMaxWidth = 0;
          *cMaxHeight = 0;
          *cFramerate = 0;
          *interleaved = 0;
          break;
      }
    }
      break;
    case GST_WFD_VIDEO_VESA_RESOLUTION:
    {
      switch (resolution) {
        case GST_WFD_VESA_800x600P30:
          *cMaxWidth = 800;
          *cMaxHeight = 600;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_800x600P60:
          *cMaxWidth = 800;
          *cMaxHeight = 600;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1024x768P30:
          *cMaxWidth = 1024;
          *cMaxHeight = 768;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1024x768P60:
          *cMaxWidth = 1024;
          *cMaxHeight = 768;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1152x864P30:
          *cMaxWidth = 1152;
          *cMaxHeight = 864;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1152x864P60:
          *cMaxWidth = 1152;
          *cMaxHeight = 864;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x768P30:
          *cMaxWidth = 1280;
          *cMaxHeight = 768;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x768P60:
          *cMaxWidth = 1280;
          *cMaxHeight = 768;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x800P30:
          *cMaxWidth = 1280;
          *cMaxHeight = 800;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x800P60:
          *cMaxWidth = 1280;
          *cMaxHeight = 800;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1360x768P30:
          *cMaxWidth = 1360;
          *cMaxHeight = 768;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1360x768P60:
          *cMaxWidth = 1360;
          *cMaxHeight = 768;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1366x768P30:
          *cMaxWidth = 1366;
          *cMaxHeight = 768;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1366x768P60:
          *cMaxWidth = 1366;
          *cMaxHeight = 768;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x1024P30:
          *cMaxWidth = 1280;
          *cMaxHeight = 1024;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1280x1024P60:
          *cMaxWidth = 1280;
          *cMaxHeight = 1024;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1400x1050P30:
          *cMaxWidth = 1400;
          *cMaxHeight = 1050;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1400x1050P60:
          *cMaxWidth = 1400;
          *cMaxHeight = 1050;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1440x900P30:
          *cMaxWidth = 1440;
          *cMaxHeight = 900;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1440x900P60:
          *cMaxWidth = 1440;
          *cMaxHeight = 900;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1600x900P30:
          *cMaxWidth = 1600;
          *cMaxHeight = 900;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1600x900P60:
          *cMaxWidth = 1600;
          *cMaxHeight = 900;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1600x1200P30:
          *cMaxWidth = 1600;
          *cMaxHeight = 1200;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1600x1200P60:
          *cMaxWidth = 1600;
          *cMaxHeight = 1200;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1680x1024P30:
          *cMaxWidth = 1680;
          *cMaxHeight = 1024;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1680x1024P60:
          *cMaxWidth = 1680;
          *cMaxHeight = 1024;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1680x1050P30:
          *cMaxWidth = 1680;
          *cMaxHeight = 1050;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1680x1050P60:
          *cMaxWidth = 1680;
          *cMaxHeight = 1050;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1920x1200P30:
          *cMaxWidth = 1920;
          *cMaxHeight = 1200;
          *cFramerate = 30;
          *interleaved = 0;
          break;
        case GST_WFD_VESA_1920x1200P60:
          *cMaxWidth = 1920;
          *cMaxHeight = 1200;
          *cFramerate = 60;
          *interleaved = 0;
          break;
        default:
          *cMaxWidth = 0;
          *cMaxHeight = 0;
          *cFramerate = 0;
          *interleaved = 0;
          break;
      }
    }
      break;
    case GST_WFD_VIDEO_HH_RESOLUTION:
    {
      *interleaved = 0;
      switch (resolution) {
        case GST_WFD_HH_800x480P30:
          *cMaxWidth = 800;
          *cMaxHeight = 480;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_800x480P60:
          *cMaxWidth = 800;
          *cMaxHeight = 480;
          *cFramerate = 60;
          break;
        case GST_WFD_HH_854x480P30:
          *cMaxWidth = 854;
          *cMaxHeight = 480;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_854x480P60:
          *cMaxWidth = 854;
          *cMaxHeight = 480;
          *cFramerate = 60;
          break;
        case GST_WFD_HH_864x480P30:
          *cMaxWidth = 864;
          *cMaxHeight = 480;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_864x480P60:
          *cMaxWidth = 864;
          *cMaxHeight = 480;
          *cFramerate = 60;
          break;
        case GST_WFD_HH_640x360P30:
          *cMaxWidth = 640;
          *cMaxHeight = 360;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_640x360P60:
          *cMaxWidth = 640;
          *cMaxHeight = 360;
          *cFramerate = 60;
          break;
        case GST_WFD_HH_960x540P30:
          *cMaxWidth = 960;
          *cMaxHeight = 540;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_960x540P60:
          *cMaxWidth = 960;
          *cMaxHeight = 540;
          *cFramerate = 60;
          break;
        case GST_WFD_HH_848x480P30:
          *cMaxWidth = 848;
          *cMaxHeight = 480;
          *cFramerate = 30;
          break;
        case GST_WFD_HH_848x480P60:
          *cMaxWidth = 848;
          *cMaxHeight = 480;
          *cFramerate = 60;
          break;
        default:
          *cMaxWidth = 0;
          *cMaxHeight = 0;
          *cFramerate = 0;
          *interleaved = 0;
          break;
      }
    }
    break;

    default:
      *cMaxWidth = 0;
      *cMaxHeight = 0;
      *cFramerate = 0;
      *interleaved = 0;
      break;
  }
  return resolution;
}

static gchar *
wfd_make_path_from_uri (GstRTSPClient * client, const GstRTSPUrl * uri)
{
  gchar *path;

  GST_DEBUG_OBJECT (client, "Got URI host : %s", uri->host);
  GST_DEBUG_OBJECT (client, "Got URI abspath : %s", uri->abspath);

  path = g_strdup ("/wfd1.0/streamid=0");

  return path;
}

static void
handle_wfd_play (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (priv != NULL);

  wfd_set_keep_alive_condition(_client);

  priv->stats_timer_id = g_timeout_add (2000, wfd_display_rtp_stats, _client);

  g_signal_emit (client,
      gst_rtsp_client_wfd_signals[SIGNAL_WFD_PLAYING_DONE], 0, NULL);
}

static void
handle_wfd_response (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  guint8 *data = NULL;
  guint size = 0;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (priv != NULL);

  GST_INFO_OBJECT (_client, "Handling response..");

  if (!ctx) {
    GST_ERROR_OBJECT (_client, "Context is NULL");
    goto error;
  }

  if (!ctx->response) {
    GST_ERROR_OBJECT (_client, "Response is NULL");
    goto error;
  }

  /* parsing the GET_PARAMTER response */
  res = gst_rtsp_message_get_body (ctx->response, (guint8 **) & data, &size);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (_client, "Failed to get body of response...");
    return;
  }

  GST_INFO_OBJECT (_client, "Response body is %d", size);
  if (size > 0) {
    if (!priv->m3_done) {
      GstWFDResult wfd_res;
      GstWFDMessage *msg = NULL;
      /* Parse M3 response from sink */
      wfd_res = gst_wfd_message_new (&msg);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client, "Failed to create wfd message...");
        goto error;
      }

      wfd_res = gst_wfd_message_init (msg);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client, "Failed to init wfd message...");
        goto error;
      }

      wfd_res = gst_wfd_message_parse_buffer (data, size, msg);

      GST_DEBUG_OBJECT (client, "M3 response server side message body: %s",
          gst_wfd_message_as_text (msg));

      /* Get the audio formats supported by WFDSink */
      if (msg->audio_codecs) {
        wfd_res =
            gst_wfd_message_get_supported_audio_format (msg, &priv->caCodec,
            &priv->cFreq, &priv->cChanels, &priv->cBitwidth, &priv->caLatency);
        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd support audio formats...");
          goto error;
        }
      }

      /* Get the Video formats supported by WFDSink */
      wfd_res =
          gst_wfd_message_get_supported_video_format (msg, &priv->cvCodec,
          &priv->cNative, &priv->cNativeResolution,
          (guint64 *) & priv->cCEAResolution,
          (guint64 *) & priv->cVESAResolution,
          (guint64 *) & priv->cHHResolution, &priv->cProfile, &priv->cLevel,
          &priv->cvLatency, &priv->cMaxHeight, &priv->cMaxWidth,
          &priv->cmin_slice_size, &priv->cslice_enc_params,
          &priv->cframe_rate_control);
      if (wfd_res != GST_WFD_OK) {
        GST_WARNING_OBJECT (client,
            "Failed to get wfd supported video formats...");
        goto error;
      }

      if (msg->client_rtp_ports) {
        /* Get the RTP ports preferred by WFDSink */
        wfd_res =
            gst_wfd_message_get_prefered_rtp_ports (msg, &priv->ctrans,
            &priv->cprofile, &priv->clowertrans, &priv->crtp_port0,
            &priv->crtp_port1);
        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd prefered RTP ports...");
          goto error;
        }
      }

      if (msg->display_edid) {
        guint32 edid_block_count = 0;
        gchar *edid_payload = NULL;
        priv->edid_supported = FALSE;
        /* Get the display edid preferred by WFDSink */
        GST_DEBUG_OBJECT (client, "Going to gst_wfd_message_get_display_edid");
        wfd_res =
            gst_wfd_message_get_display_edid (msg, &priv->edid_supported,
            &edid_block_count, &edid_payload);
        if (wfd_res != GST_WFD_OK) {
          GST_ERROR_OBJECT (client, "Failed to get wfd display edid...");
          goto error;
        }
        GST_DEBUG_OBJECT (client, " edid supported: %d edid_block_count: %d",
            priv->edid_supported, edid_block_count);
        if (priv->edid_supported) {
          priv->edid_hres = 0;
          priv->edid_vres = 0;
          priv->edid_hres =
              (guint32) (((edid_payload[54 + 4] >> 4) << 8) | edid_payload[54 +
                  2]);
          priv->edid_vres =
              (guint32) (((edid_payload[54 + 7] >> 4) << 8) | edid_payload[54 +
                  5]);
          GST_DEBUG_OBJECT (client, " edid supported Hres: %d Wres: %d",
              priv->edid_hres, priv->edid_vres);
          if ((priv->edid_hres < 640) || (priv->edid_vres < 480)
              || (priv->edid_hres > 1920) || (priv->edid_vres > 1080)) {
            priv->edid_hres = 0;
            priv->edid_vres = 0;
            priv->edid_supported = FALSE;
            GST_WARNING_OBJECT (client, " edid invalid resolutions");
          }
        }
      }

      if (msg->content_protection) {
#if 0
        /*Get the hdcp version and tcp port by WFDSink */
        wfd_res =
            gst_wfd_message_get_contentprotection_type (msg,
            &priv->hdcp_version, &priv->hdcp_tcpport);
        GST_DEBUG ("hdcp version =%d, tcp port = %d", priv->hdcp_version,
            priv->hdcp_tcpport);
        if (priv->hdcp_version > 0 && priv->hdcp_tcpport > 0)
          priv->protection_enabled = TRUE;

        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd content protection...");
          goto error;
        }
#else
        GST_WARNING_OBJECT (client, "Don't use content protection");
#endif
      }

      g_signal_emit (_client,
          gst_rtsp_client_wfd_signals[SIGNAL_WFD_GET_PARAMETER_REQUEST], 0,
          ctx);
    } else {
      /* TODO-WFD: Handle another GET_PARAMETER response with body */
    }
  } else if (size == 0) {
    if (!priv->m1_done) {
      GST_INFO_OBJECT (_client, "M1 response is done");
      priv->m1_done = TRUE;
    } else if (!priv->m4_done) {
      GST_INFO_OBJECT (_client, "M4 response is done");
      priv->m4_done = TRUE;

      gst_rtsp_wfd_client_trigger_request (_client, WFD_TRIGGER_SETUP);
    } else {
      g_mutex_lock(&priv->keep_alive_lock);
      if (priv->keep_alive_flag == FALSE) {
        GST_INFO_OBJECT (_client, "M16 response is done");
        priv->keep_alive_flag = TRUE;
      }
      g_mutex_unlock(&priv->keep_alive_lock);
    }
  }

  return;

error:
  return;
}

static gboolean
handle_wfd_options_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMethod options;
  gchar *tmp = NULL;
  gchar *str = NULL;
  gchar *user_agent = NULL;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);

  options = GST_RTSP_OPTIONS |
      GST_RTSP_PAUSE |
      GST_RTSP_PLAY |
      GST_RTSP_SETUP |
      GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

  str = gst_rtsp_options_as_text (options);

  /*append WFD specific method */
  tmp = g_strdup (", org.wfa.wfd1.0");
  g_strlcat (str, tmp, strlen (tmp) + strlen (str) + 1);

  gst_rtsp_message_init_response (ctx->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), ctx->request);

  gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_PUBLIC, str);
  g_free (str);
  str = NULL;

  res =
      gst_rtsp_message_get_header (ctx->request, GST_RTSP_HDR_USER_AGENT,
      &user_agent, 0);
  if (res == GST_RTSP_OK) {
    gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_USER_AGENT,
        user_agent);
  } else {
    return FALSE;
  }

  res = gst_rtsp_client_send_message (client, NULL, ctx->response);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
    return FALSE;
  }

  GST_DEBUG_OBJECT (client, "Sent M2 response...");

  g_signal_emit (_client,
      gst_rtsp_client_wfd_signals[SIGNAL_WFD_OPTIONS_REQUEST], 0, ctx);

  return TRUE;
}

static gboolean
handle_wfd_get_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  guint8 *data = NULL;
  guint size = 0;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);

  /* parsing the GET_PARAMTER request */
  res = gst_rtsp_message_get_body (ctx->request, (guint8 **) & data, &size);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (_client, "Failed to get body of request...");
    return FALSE;
  }

  if (size == 0) {
    send_generic_wfd_response (_client, GST_RTSP_STS_OK, ctx);
  } else {
    /* TODO-WFD: Handle other GET_PARAMETER request from sink */
  }

  return TRUE;
}

static gboolean
handle_wfd_set_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  guint8 *data = NULL;
  guint size = 0;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);

  res = gst_rtsp_message_get_body (ctx->request, &data, &size);
  if (res != GST_RTSP_OK)
    goto bad_request;

  if (size == 0) {
    /* no body, keep-alive request */
    send_generic_wfd_response (_client, GST_RTSP_STS_OK, ctx);
  } else {
    if (data != NULL) {
      GST_INFO_OBJECT (_client, "SET_PARAMETER Request : %s(%d)", data, size);
      if (g_strcmp0 ((const gchar *) data, "wfd_idr_request"))
        send_generic_wfd_response (_client, GST_RTSP_STS_OK, ctx);
#if 0
      else
        /* TODO-WFD : Handle other set param request */
        send_generic_wfd_response (_client, GST_RTSP_STS_OK, ctx);
#endif
    } else {
      goto bad_request;
    }
  }

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("_client %p: bad request", _client);
    send_generic_wfd_response (_client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
}

#if 0
static gboolean
gst_rtsp_wfd_client_parse_methods (GstRTSPWFDClient * client,
    GstRTSPMessage * response)
{
  GstRTSPHeaderField field;
  gchar *respoptions;
  gchar **options;
  gint indx = 0;
  gint i;
  gboolean found_wfd_method = FALSE;

  /* reset supported methods */
  client->supported_methods = 0;

  /* Try Allow Header first */
  field = GST_RTSP_HDR_ALLOW;
  while (TRUE) {
    respoptions = NULL;
    gst_rtsp_message_get_header (response, field, &respoptions, indx);
    if (indx == 0 && !respoptions) {
      /* if no Allow header was found then try the Public header... */
      field = GST_RTSP_HDR_PUBLIC;
      gst_rtsp_message_get_header (response, field, &respoptions, indx);
    }
    if (!respoptions)
      break;

    /* If we get here, the server gave a list of supported methods, parse
     * them here. The string is like:
     *
     * OPTIONS,  PLAY, SETUP, ...
     */
    options = g_strsplit (respoptions, ",", 0);

    for (i = 0; options[i]; i++) {
      gchar *stripped;
      gint method;

      stripped = g_strstrip (options[i]);
      method = gst_rtsp_find_method (stripped);

      if (!g_ascii_strcasecmp ("org.wfa.wfd1.0", stripped))
        found_wfd_method = TRUE;

      /* keep bitfield of supported methods */
      if (method != GST_RTSP_INVALID)
        client->supported_methods |= method;
    }
    g_strfreev (options);

    indx++;
  }

  if (!found_wfd_method) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : org.wfa.wfd1.0...");
    goto no_required_methods;
  }

  /* Checking mandatory method */
  if (!(client->supported_methods & GST_RTSP_SET_PARAMETER)) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : SET_PARAMETER...");
    goto no_required_methods;
  }

  /* Checking mandatory method */
  if (!(client->supported_methods & GST_RTSP_GET_PARAMETER)) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : GET_PARAMETER...");
    goto no_required_methods;
  }

  if (!(client->supported_methods & GST_RTSP_OPTIONS)) {
    GST_INFO_OBJECT (client, "assuming OPTIONS is supported by client...");
    client->supported_methods |= GST_RTSP_OPTIONS;
  }

  return TRUE;

/* ERRORS */
no_required_methods:
  {
    GST_ELEMENT_ERROR (client, RESOURCE, OPEN_READ, (NULL),
        ("WFD Client does not support mandatory methods."));
    return FALSE;
  }
}
#endif

typedef enum
{
  M1_REQ_MSG,
  M1_RES_MSG,
  M2_REQ_MSG,
  M2_RES_MSG,
  M3_REQ_MSG,
  M3_RES_MSG,
  M4_REQ_MSG,
  M4_RES_MSG,
  M5_REQ_MSG,
  TEARDOWN_TRIGGER,
  PLAY_TRIGGER,
  PAUSE_TRIGGER,
} GstWFDMessageType;

static gboolean
_set_negotiated_audio_codec (GstRTSPWFDClient *client,
    guint audio_codec)
{
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  GstRTSPMediaFactory *factory = NULL;
  GstRTSPMountPoints *mount_points = NULL;
  gchar *path = NULL;
  gint matched = 0;
  gboolean ret = TRUE;

  if (!(mount_points = gst_rtsp_client_get_mount_points (parent_client))) {
    ret = FALSE;
    GST_ERROR_OBJECT (client, "Failed to set negotiated audio codec: no mount points...");
    goto no_mount_points;
  }

  path = g_strdup(WFD_MOUNT_POINT);
  if (!path) {
    ret = FALSE;
    GST_ERROR_OBJECT (client, "Failed to set negotiated audio codec: no path...");
    goto no_path;
  }

  if (!(factory = gst_rtsp_mount_points_match (mount_points,
          path, &matched))) {
    GST_ERROR_OBJECT (client, "Failed to set negotiated audio codec: no factory...");
    ret = FALSE;
    goto no_factory;
  }

  gst_rtsp_media_factory_wfd_set_audio_codec (factory,
      audio_codec);
  ret = TRUE;

  g_object_unref(factory);

no_factory:
  g_free(path);
no_path:
  g_object_unref(mount_points);
no_mount_points:
  return ret;
}

static gboolean
_set_negotiated_resolution(GstRTSPWFDClient *client,
    guint32 width, guint32 height)
{
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  GstRTSPMediaFactory *factory = NULL;
  GstRTSPMountPoints *mount_points = NULL;
  gchar *path = NULL;
  gint matched = 0;
  gboolean ret = TRUE;

  if (!(mount_points = gst_rtsp_client_get_mount_points (parent_client))) {
    ret = FALSE;
    GST_ERROR_OBJECT (client, "Failed to set negotiated resolution: no mount points...");
    goto no_mount_points;
  }

  path = g_strdup(WFD_MOUNT_POINT);
  if (!path) {
    ret = FALSE;
    GST_ERROR_OBJECT (client, "Failed to set negotiated resolution: no path...");
    goto no_path;
  }

  if (!(factory = gst_rtsp_mount_points_match (mount_points,
          path, &matched))) {
    GST_ERROR_OBJECT (client, "Failed to set negotiated resolution: no factory...");
    ret = FALSE;
    goto no_factory;
  }

  gst_rtsp_media_factory_wfd_set_negotiated_resolution(factory,
      width, height);
  ret = TRUE;

  g_object_unref(factory);

no_factory:
  g_free(path);
no_path:
  g_object_unref(mount_points);
no_mount_points:
  return ret;
}

static void
_set_wfd_message_body (GstRTSPWFDClient * client, GstWFDMessageType msg_type,
    gchar ** data, guint * len)
{
  GString *buf = NULL;
  GstWFDMessage *msg = NULL;
  GstWFDResult wfd_res = GST_WFD_EINVAL;
  GstRTSPWFDClientPrivate *priv = NULL;
  priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (priv != NULL);

  buf = g_string_new ("");
  g_return_if_fail (buf != NULL);

  if (msg_type == M3_REQ_MSG) {
    /* create M3 request to be sent */
    wfd_res = gst_wfd_message_new (&msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to create wfd message...");
      goto error;
    }

    wfd_res = gst_wfd_message_init (msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to init wfd message...");
      goto error;
    }

    /* set the supported audio formats by the WFD server */
    wfd_res =
        gst_wfd_message_set_supported_audio_format (msg, GST_WFD_AUDIO_UNKNOWN,
        GST_WFD_FREQ_UNKNOWN, GST_WFD_CHANNEL_UNKNOWN, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported audio formats on wfd message...");
      goto error;
    }

    /* set the supported Video formats by the WFD server */
    wfd_res =
        gst_wfd_message_set_supported_video_format (msg, GST_WFD_VIDEO_UNKNOWN,
        GST_WFD_VIDEO_CEA_RESOLUTION, GST_WFD_CEA_UNKNOWN, GST_WFD_CEA_UNKNOWN,
        GST_WFD_VESA_UNKNOWN, GST_WFD_HH_UNKNOWN, GST_WFD_H264_UNKNOWN_PROFILE,
        GST_WFD_H264_LEVEL_UNKNOWN, 0, 0, 0, 0, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported video formats on wfd message...");
      goto error;
    }

    wfd_res = gst_wfd_message_set_display_edid (msg, 0, 0, NULL);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set display edid type on wfd message...");
      goto error;
    }

    if (priv->protection_enabled) {
      wfd_res =
          gst_wfd_message_set_contentprotection_type (msg, GST_WFD_HDCP_NONE,
          0);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client,
            "Failed to set supported content protection type on wfd message...");
        goto error;
      }
    }

    /* set the preffered RTP ports for the WFD server */
    wfd_res =
        gst_wfd_messge_set_prefered_rtp_ports (msg, GST_WFD_RTSP_TRANS_UNKNOWN,
        GST_WFD_RTSP_PROFILE_UNKNOWN, GST_WFD_RTSP_LOWER_TRANS_UNKNOWN, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported video formats on wfd message...");
      goto error;
    }

    *data = gst_wfd_message_param_names_as_text (msg);
    if (*data == NULL) {
      GST_ERROR_OBJECT (client, "Failed to get wfd message as text...");
      goto error;
    } else {
      *len = strlen (*data);
    }
  } else if (msg_type == M4_REQ_MSG) {
    GstRTSPUrl *url = NULL;

    GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
    GstRTSPConnection *connection =
        gst_rtsp_client_get_connection (parent_client);

    /* Parameters for the preffered audio formats */
    GstWFDAudioFormats taudiocodec = GST_WFD_AUDIO_UNKNOWN;
    GstWFDAudioFreq taudiofreq = GST_WFD_FREQ_UNKNOWN;
    GstWFDAudioChannels taudiochannels = GST_WFD_CHANNEL_UNKNOWN;

    /* Parameters for the preffered video formats */
    GstWFDVideoCEAResolution tcCEAResolution = GST_WFD_CEA_UNKNOWN;
    GstWFDVideoVESAResolution tcVESAResolution = GST_WFD_VESA_UNKNOWN;
    GstWFDVideoHHResolution tcHHResolution = GST_WFD_HH_UNKNOWN;
    GstWFDVideoH264Profile tcProfile;
    GstWFDVideoH264Level tcLevel;
    guint64 resolution_supported = 0;

    url = gst_rtsp_connection_get_url (connection);
    if (url == NULL) {
      GST_ERROR_OBJECT (client, "Failed to get connection URL");
      return;
    }

    /* Logic to negotiate with information of M3 response */
    /* create M4 request to be sent */
    wfd_res = gst_wfd_message_new (&msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to create wfd message...");
      goto error;
    }

    wfd_res = gst_wfd_message_init (msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to init wfd message...");
      goto error;
    }

    g_string_append_printf (buf, "rtsp://");

    if (priv->host_address) {
      g_string_append (buf, priv->host_address);
    } else {
      GST_ERROR_OBJECT (client, "Failed to get host address");
      if (buf) g_string_free (buf, TRUE);
      goto error;
    }

    g_string_append_printf (buf, "/wfd1.0/streamid=0");
    wfd_res =
        gst_wfd_message_set_presentation_url (msg, g_string_free (buf, FALSE),
        NULL);

    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to set presentation url");
      goto error;
    }

    taudiocodec = wfd_get_prefered_audio_codec (priv->audio_codec, priv->caCodec);
    priv->caCodec = taudiocodec;
    if (!_set_negotiated_audio_codec(client, priv->caCodec)) {
      GST_ERROR_OBJECT (client, "Failed to set negotiated "
          "audio codec to media factory...");
    }

    if (priv->cFreq & GST_WFD_FREQ_48000)
      taudiofreq = GST_WFD_FREQ_48000;
    else if (priv->cFreq & GST_WFD_FREQ_44100)
      taudiofreq = GST_WFD_FREQ_44100;
    priv->cFreq = taudiofreq;

    /* TODO-WFD: Currently only 2 channels is present */
    if (priv->cChanels & GST_WFD_CHANNEL_8)
      taudiochannels = GST_WFD_CHANNEL_2;
    else if (priv->cChanels & GST_WFD_CHANNEL_6)
      taudiochannels = GST_WFD_CHANNEL_2;
    else if (priv->cChanels & GST_WFD_CHANNEL_4)
      taudiochannels = GST_WFD_CHANNEL_2;
    else if (priv->cChanels & GST_WFD_CHANNEL_2)
      taudiochannels = GST_WFD_CHANNEL_2;
    priv->cChanels = taudiochannels;

    wfd_res =
        gst_wfd_message_set_prefered_audio_format (msg, taudiocodec, taudiofreq,
        taudiochannels, priv->cBitwidth, priv->caLatency);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (priv, "Failed to set preffered audio formats...");
      goto error;
    }

    /* Set the preffered video formats */
    priv->cvCodec = GST_WFD_VIDEO_H264;
    priv->cProfile = tcProfile = GST_WFD_H264_BASE_PROFILE;
    priv->cLevel = tcLevel = GST_WFD_H264_LEVEL_3_1;

    resolution_supported = priv->video_resolution_supported;

    /* TODO-WFD: Need to verify this logic
       if(priv->edid_supported) {
       if (priv->edid_hres < 1920) resolution_supported = resolution_supported & 0x8C7F;
       if (priv->edid_hres < 1280) resolution_supported = resolution_supported & 0x1F;
       if (priv->edid_hres < 720) resolution_supported = resolution_supported & 0x01;
       }
     */

    if (priv->video_native_resolution == GST_WFD_VIDEO_CEA_RESOLUTION) {
      tcCEAResolution =
          wfd_get_prefered_resolution (resolution_supported,
          priv->cCEAResolution, priv->video_native_resolution, &priv->cMaxWidth,
          &priv->cMaxHeight, &priv->cFramerate, &priv->cInterleaved);
      GST_DEBUG
          ("wfd negotiated resolution: %08x, width: %d, height: %d, framerate: %d, interleaved: %d",
          tcCEAResolution, priv->cMaxWidth, priv->cMaxHeight, priv->cFramerate,
          priv->cInterleaved);
    } else if (priv->video_native_resolution == GST_WFD_VIDEO_VESA_RESOLUTION) {
      tcVESAResolution =
          wfd_get_prefered_resolution (resolution_supported,
          priv->cVESAResolution, priv->video_native_resolution,
          &priv->cMaxWidth, &priv->cMaxHeight, &priv->cFramerate,
          &priv->cInterleaved);
      GST_DEBUG
          ("wfd negotiated resolution: %08x, width: %d, height: %d, framerate: %d, interleaved: %d",
          tcVESAResolution, priv->cMaxWidth, priv->cMaxHeight, priv->cFramerate,
          priv->cInterleaved);
    } else if (priv->video_native_resolution == GST_WFD_VIDEO_HH_RESOLUTION) {
      tcHHResolution =
          wfd_get_prefered_resolution (resolution_supported,
          priv->cHHResolution, priv->video_native_resolution, &priv->cMaxWidth,
          &priv->cMaxHeight, &priv->cFramerate, &priv->cInterleaved);
      GST_DEBUG
          ("wfd negotiated resolution: %08x, width: %d, height: %d, framerate: %d, interleaved: %d",
          tcHHResolution, priv->cMaxWidth, priv->cMaxHeight, priv->cFramerate,
          priv->cInterleaved);
    }

    if (!_set_negotiated_resolution(client, priv->cMaxWidth,
          priv->cMaxHeight)) {
      GST_ERROR_OBJECT (client, "Failed to set negotiated "
          "resolution to media factory...");
    }

    wfd_res =
        gst_wfd_message_set_prefered_video_format (msg, priv->cvCodec,
        priv->video_native_resolution, GST_WFD_CEA_UNKNOWN, tcCEAResolution,
        tcVESAResolution, tcHHResolution, tcProfile, tcLevel, priv->cvLatency,
        priv->cMaxWidth, priv->cMaxHeight, priv->cmin_slice_size,
        priv->cslice_enc_params, priv->cframe_rate_control);

    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to set preffered video formats...");
      goto error;
    }

    /* set the preffered RTP ports for the WFD server */
    wfd_res =
        gst_wfd_messge_set_prefered_rtp_ports (msg, GST_WFD_RTSP_TRANS_RTP,
        GST_WFD_RTSP_PROFILE_AVP, GST_WFD_RTSP_LOWER_TRANS_UDP, priv->crtp_port0, priv->crtp_port1);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported video formats on wfd message...");
      goto error;
    }

    *data = gst_wfd_message_as_text (msg);
    if (*data == NULL) {
      GST_ERROR_OBJECT (client, "Failed to get wfd message as text...");
      goto error;
    } else {
      *len = strlen (*data);
    }
  } else if (msg_type == M5_REQ_MSG) {
    g_string_append (buf, "wfd_trigger_method: SETUP");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else if (msg_type == TEARDOWN_TRIGGER) {
    g_string_append (buf, "wfd_trigger_method: TEARDOWN");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else if (msg_type == PLAY_TRIGGER) {
    g_string_append (buf, "wfd_trigger_method: PLAY");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else if (msg_type == PAUSE_TRIGGER) {
    g_string_append (buf, "wfd_trigger_method: PAUSE");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else {
    return;
  }

  return;

error:
  *data = NULL;
  *len = 0;

  return;
}

/**
* gst_prepare_request:
* @client: client object
* @request : requst message to be prepared
* @method : RTSP method of the request
* @url : url need to be in the request
* @message_type : WFD message type
* @trigger_type : trigger method to be used for M5 mainly
*
* Prepares request based on @method & @message_type
*
* Returns: a #GstRTSPResult.
*/
GstRTSPResult
gst_prepare_request (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMethod method, gchar * url)
{
  GstRTSPResult res = GST_RTSP_OK;
  gchar *str = NULL;

  if (method == GST_RTSP_GET_PARAMETER || method == GST_RTSP_SET_PARAMETER) {
    g_free (url);
    url = g_strdup ("rtsp://localhost/wfd1.0");
  }

  GST_DEBUG_OBJECT (client, "Preparing request: %d", method);

  /* initialize the request */
  res = gst_rtsp_message_init_request (request, method, url);
  if (res < 0) {
    GST_ERROR ("init request failed");
    return res;
  }

  switch (method) {
      /* Prepare OPTIONS request to send */
    case GST_RTSP_OPTIONS:{
      /* add wfd specific require filed "org.wfa.wfd1.0" */
      str = g_strdup ("org.wfa.wfd1.0");
      res = gst_rtsp_message_add_header (request, GST_RTSP_HDR_REQUIRE, str);
      if (res < 0) {
        GST_ERROR ("Failed to add header");
        g_free (str);
        return res;
      }

      g_free (str);
      break;
    }

      /* Prepare GET_PARAMETER request */
    case GST_RTSP_GET_PARAMETER:{
      gchar *msg = NULL;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res < 0) {
        GST_ERROR ("Failed to add header");
        return res;
      }

      _set_wfd_message_body (client, M3_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M3 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }

      /* Prepare SET_PARAMETER request */
    case GST_RTSP_SET_PARAMETER:{
      gchar *msg = NULL;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, M4_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M4 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }

    default:{
    }
  }

  return res;

error:
  return GST_RTSP_ERROR;
}

GstRTSPResult
prepare_trigger_request (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstWFDTriggerType trigger_type, gchar * url)
{
  GstRTSPResult res = GST_RTSP_OK;

  /* initialize the request */
  res = gst_rtsp_message_init_request (request, GST_RTSP_SET_PARAMETER, url);
  if (res < 0) {
    GST_ERROR ("init request failed");
    return res;
  }

  switch (trigger_type) {
    case WFD_TRIGGER_SETUP:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, M5_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M5 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
    case WFD_TRIGGER_TEARDOWN:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, TEARDOWN_TRIGGER, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("Trigger TEARDOWN server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
    case WFD_TRIGGER_PLAY:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, PLAY_TRIGGER, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("Trigger PLAY server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
    case WFD_TRIGGER_PAUSE:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, PAUSE_TRIGGER, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("Trigger PAUSE server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
      /* TODO-WFD: implement to handle other trigger type */
    default:{
    }
  }

  return res;

error:
  return res;
}


void
gst_send_request (GstRTSPWFDClient * client, GstRTSPSession * session,
    GstRTSPMessage * request)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  /* remove any previous header */
  gst_rtsp_message_remove_header (request, GST_RTSP_HDR_SESSION, -1);

  /* add the new session header for new session ids */
  if (session) {
    guint timeout;
    const gchar *sessionid = NULL;
    gchar *str;

    sessionid = gst_rtsp_session_get_sessionid (session);
    GST_INFO_OBJECT (client, "Session id : %s", sessionid);

    timeout = gst_rtsp_session_get_timeout (session);
    if (timeout != DEFAULT_WFD_TIMEOUT)
      str = g_strdup_printf ("%s; timeout=%d", sessionid, timeout);
    else
      str = g_strdup (sessionid);

    gst_rtsp_message_take_header (request, GST_RTSP_HDR_SESSION, str);
  }
#if 0
  if (gst_debug_category_get_threshold (rtsp_wfd_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (request);
  }
#endif
  res = gst_rtsp_client_send_message (parent_client, session, request);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
  }

  gst_rtsp_message_unset (request);
}

/**
* prepare_response:
* @client: client object
* @request : requst message received
* @response : response to be prepare based on request
* @method : RTSP method
*
* prepare response to the request based on @method & @message_type
*
* Returns: a #GstRTSPResult.
*/
GstRTSPResult
prepare_response (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMessage * response, GstRTSPMethod method)
{
  GstRTSPResult res = GST_RTSP_OK;

  switch (method) {
      /* prepare OPTIONS response */
    case GST_RTSP_OPTIONS:{
      GstRTSPMethod options;
      gchar *tmp = NULL;
      gchar *str = NULL;
      gchar *user_agent = NULL;

      options = GST_RTSP_OPTIONS |
          GST_RTSP_PAUSE |
          GST_RTSP_PLAY |
          GST_RTSP_SETUP |
          GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

      str = gst_rtsp_options_as_text (options);

      /*append WFD specific method */
      tmp = g_strdup (", org.wfa.wfd1.0");
      g_strlcat (str, tmp, strlen (tmp) + strlen (str) + 1);

      gst_rtsp_message_init_response (response, GST_RTSP_STS_OK,
          gst_rtsp_status_as_text (GST_RTSP_STS_OK), request);

      gst_rtsp_message_add_header (response, GST_RTSP_HDR_PUBLIC, str);
      g_free (str);
      str = NULL;
      res =
          gst_rtsp_message_get_header (request, GST_RTSP_HDR_USER_AGENT,
          &user_agent, 0);
      if (res == GST_RTSP_OK) {
        gst_rtsp_message_add_header (response, GST_RTSP_HDR_USER_AGENT,
            user_agent);
      } else
        res = GST_RTSP_OK;
      break;
    }
    default:
      GST_ERROR_OBJECT (client, "Unhandled method...");
      return GST_RTSP_EINVAL;
      break;
  }

  return res;
}

static void
send_generic_wfd_response (GstRTSPWFDClient * client, GstRTSPStatusCode code,
    GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  res = gst_rtsp_client_send_message (parent_client, NULL, ctx->response);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
  }
}


static GstRTSPResult
handle_M1_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };

  res = gst_prepare_request (client, &request, GST_RTSP_OPTIONS, (gchar *) "*");
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M1 request....\n");
    return res;
  }

  GST_DEBUG_OBJECT (client, "Sending M1 request.. (OPTIONS request)");

  gst_send_request (client, NULL, &request);

  return res;
}

/**
* handle_M3_message:
* @client: client object
*
* Handles M3 WFD message.
* This API will send M3 message (GET_PARAMETER) to WFDSink to query supported formats by the WFDSink.
* After getting supported formats info, this API will set those values on WFDConfigMessage obj
*
* Returns: a #GstRTSPResult.
*/
static GstRTSPResult
handle_M3_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = gst_prepare_request (client, &request, GST_RTSP_GET_PARAMETER, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M3 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending GET_PARAMETER request message (M3)...");

  gst_send_request (client, NULL, &request);

  return res;

error:
  return res;
}

static GstRTSPResult
handle_M4_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = gst_prepare_request (client, &request, GST_RTSP_SET_PARAMETER, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M4 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending SET_PARAMETER request message (M4)...");

  gst_send_request (client, NULL, &request);

  return res;

error:
  return res;
}

GstRTSPResult
gst_rtsp_wfd_client_trigger_request (GstRTSPWFDClient * client,
    GstWFDTriggerType type)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = prepare_trigger_request (client, &request, type, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M5 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending trigger request message...: %d", type);

  gst_send_request (client, NULL, &request);

  return res;

error:
  return res;
}

GstRTSPResult
gst_rtsp_wfd_client_set_video_supported_resolution (GstRTSPWFDClient * client,
    guint64 supported_reso)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_val_if_fail (priv != NULL, GST_RTSP_EINVAL);

  priv->video_resolution_supported = supported_reso;
  GST_DEBUG ("Resolution : %"G_GUINT64_FORMAT, supported_reso);

  return res;
}

GstRTSPResult
gst_rtsp_wfd_client_set_video_native_resolution (GstRTSPWFDClient * client,
    guint64 native_reso)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_val_if_fail (priv != NULL, GST_RTSP_EINVAL);

  priv->video_native_resolution = native_reso;
  GST_DEBUG ("Native Resolution : %"G_GUINT64_FORMAT, native_reso);

  return res;
}

GstRTSPResult
gst_rtsp_wfd_client_set_audio_codec (GstRTSPWFDClient * client,
    guint8 audio_codec)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_val_if_fail (priv != NULL, GST_RTSP_EINVAL);

  priv->audio_codec = audio_codec;
  GST_DEBUG ("Audio codec : %d", audio_codec);

  return res;
}

static gboolean
wfd_ckeck_keep_alive_response (gpointer userdata)
{
  GstRTSPWFDClient *client = (GstRTSPWFDClient *)userdata;
  GstRTSPWFDClientPrivate *priv = NULL;
  if (!client) {
    return FALSE;
  }

  priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, GST_RTSP_EINVAL);

  if (priv->keep_alive_flag) {
    return FALSE;
  }
  else {
    GST_INFO ("%p: source error notification", client);

    g_signal_emit (client,
        gst_rtsp_client_wfd_signals[SIGNAL_WFD_KEEP_ALIVE_FAIL], 0,
        NULL);
    return FALSE;
  }
}

/*Sending keep_alive (M16) message.
  Without calling gst_prepare_request function.*/
static GstRTSPResult
handle_M16_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  gchar *url_str = NULL;

  url_str = g_strdup("rtsp://localhost/wfd1.0");

  res = gst_rtsp_message_init_request (&request, GST_RTSP_GET_PARAMETER, url_str);
  if (res < 0) {
    GST_ERROR ("init request failed");
    return FALSE;
  }

  gst_send_request (client, NULL, &request);
  return GST_RTSP_OK;
}

/*CHecking whether source has got response of any request.
 * If yes, keep alive message is sent otherwise error message
 * will be displayed.*/
static gboolean
keep_alive_condition(gpointer userdata)
{
  GstRTSPWFDClient *client;
  GstRTSPWFDClientPrivate *priv;
  GstRTSPResult res;
  client = (GstRTSPWFDClient *)userdata;
  if (!client) {
    return FALSE;
  }
  priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_val_if_fail (priv != NULL, FALSE);

  g_mutex_lock(&priv->keep_alive_lock);
  if(!priv->keep_alive_flag) {
    g_timeout_add(5000, wfd_ckeck_keep_alive_response, client);
  }
  else {
    GST_DEBUG_OBJECT (client, "have received last keep alive message response");
  }

  GST_DEBUG("sending keep alive message");
  res = handle_M16_message(client);
  if(res == GST_RTSP_OK) {
    priv->keep_alive_flag = FALSE;
  } else {
    GST_ERROR_OBJECT (client, "Failed to send Keep Alive Message");
    g_mutex_unlock(&priv->keep_alive_lock);
    return FALSE;
  }

  g_mutex_unlock(&priv->keep_alive_lock);
  return TRUE;
}

static
void wfd_set_keep_alive_condition(GstRTSPWFDClient * client)
{
  g_timeout_add((DEFAULT_WFD_TIMEOUT-5)*1000, keep_alive_condition, client);
}

void
gst_rtsp_wfd_client_set_host_address (GstRTSPWFDClient *client, const gchar * address)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  g_return_if_fail (priv != NULL);

  if (priv->host_address) {
    g_free (priv->host_address);
  }

  priv->host_address = g_strdup (address);
}

guint
gst_rtsp_wfd_client_get_audio_codec(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->caCodec;
}

guint
gst_rtsp_wfd_client_get_audio_freq(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cFreq;
}

guint
gst_rtsp_wfd_client_get_audio_channels(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cChanels;
}

guint
gst_rtsp_wfd_client_get_audio_bit_width(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cBitwidth;
}

guint
gst_rtsp_wfd_client_get_audio_latency(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->caLatency;
}

guint
gst_rtsp_wfd_client_get_video_codec(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cvCodec;
}

guint
gst_rtsp_wfd_client_get_video_native(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cNative;
}

guint64
gst_rtsp_wfd_client_get_video_native_resolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cNativeResolution;
}

guint64
gst_rtsp_wfd_client_get_video_cea_resolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cCEAResolution;
}

guint64
gst_rtsp_wfd_client_get_video_vesa_resolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cVESAResolution;
}

guint64
gst_rtsp_wfd_client_get_video_hh_resolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cHHResolution;
}

guint
gst_rtsp_wfd_client_get_video_profile(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cProfile;
}

guint
gst_rtsp_wfd_client_get_video_level(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cLevel;
}

guint
gst_rtsp_wfd_client_get_video_latency(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cvLatency;
}

guint32
gst_rtsp_wfd_client_get_video_max_height(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cMaxHeight;
}

guint32
gst_rtsp_wfd_client_get_video_max_width(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cMaxWidth;
}

guint32
gst_rtsp_wfd_client_get_video_framerate(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cFramerate;
}

guint32
gst_rtsp_wfd_client_get_video_min_slice_size(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cmin_slice_size;
}

guint32
gst_rtsp_wfd_client_get_video_slice_enc_params(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cslice_enc_params;
}

guint
gst_rtsp_wfd_client_get_video_framerate_control(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->cframe_rate_control;
}

guint32
gst_rtsp_wfd_client_get_rtp_port0(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->crtp_port0;
}

guint32
gst_rtsp_wfd_client_get_rtp_port1(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->crtp_port1;
}

gboolean
gst_rtsp_wfd_client_get_edid_supported(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->edid_supported;
}

guint32
gst_rtsp_wfd_client_get_edid_hresolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->edid_hres;
}

guint32
gst_rtsp_wfd_client_get_edid_vresolution(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->edid_vres;
}

gboolean
gst_rtsp_wfd_client_get_protection_enabled(GstRTSPWFDClient *client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  return priv->protection_enabled;
}

void
gst_rtsp_wfd_client_set_audio_freq(GstRTSPWFDClient *client, guint freq)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cFreq = freq;
}

void
gst_rtsp_wfd_client_set_edid_supported(GstRTSPWFDClient *client, gboolean supported)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->edid_supported = supported;
}

void
gst_rtsp_wfd_client_set_edid_hresolution(GstRTSPWFDClient *client, guint32 reso)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->edid_hres = reso;
}

void
gst_rtsp_wfd_client_set_edid_vresolution(GstRTSPWFDClient *client, guint32 reso)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->edid_vres = reso;
}

void
gst_rtsp_wfd_client_set_protection_enabled(GstRTSPWFDClient *client, gboolean enable)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->protection_enabled = enable;
}

void
gst_rtsp_wfd_client_set_hdcp_version(GstRTSPWFDClient *client, GstWFDHDCPProtection version)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  priv->hdcp_version = version;
}

void
gst_rtsp_wfd_client_set_hdcp_port(GstRTSPWFDClient *client, guint32 port)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_val_if_fail (priv != NULL, 0);

  priv->hdcp_tcpport = port;
}

void gst_rtsp_wfd_client_set_keep_alive_flag(GstRTSPWFDClient *client, gboolean flag)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  g_mutex_lock(&priv->keep_alive_lock);
  if (priv->keep_alive_flag == !(flag))
    priv->keep_alive_flag = flag;
  g_mutex_unlock(&priv->keep_alive_lock);
}

void
gst_rtsp_wfd_client_set_aud_codec (GstRTSPWFDClient *client, guint acodec)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->caCodec = acodec;
}

void
gst_rtsp_wfd_client_set_audio_channels(GstRTSPWFDClient *client, guint channels)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cChanels = channels;
}

void
gst_rtsp_wfd_client_set_audio_bit_width(GstRTSPWFDClient *client, guint bwidth)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cBitwidth = bwidth;
}

void
gst_rtsp_wfd_client_set_audio_latency(GstRTSPWFDClient *client, guint latency)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->caLatency = latency;
}

void
gst_rtsp_wfd_client_set_video_codec(GstRTSPWFDClient *client, guint vcodec)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cvCodec = vcodec;
}

void
gst_rtsp_wfd_client_set_video_native(GstRTSPWFDClient *client, guint native)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cNative = native;
}

void
gst_rtsp_wfd_client_set_vid_native_resolution(GstRTSPWFDClient *client, guint64 res)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cNativeResolution = res;
}

void
gst_rtsp_wfd_client_set_video_cea_resolution(GstRTSPWFDClient *client, guint64 res)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cCEAResolution = res;
}

void
gst_rtsp_wfd_client_set_video_vesa_resolution(GstRTSPWFDClient *client, guint64 res)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cVESAResolution = res;
}

void
gst_rtsp_wfd_client_set_video_hh_resolution(GstRTSPWFDClient *client, guint64 res)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cHHResolution = res;
}

void
gst_rtsp_wfd_client_set_video_profile(GstRTSPWFDClient *client, guint profile)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cProfile = profile;
}

void
gst_rtsp_wfd_client_set_video_level(GstRTSPWFDClient *client, guint level)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cLevel = level;
}

void
gst_rtsp_wfd_client_set_video_latency(GstRTSPWFDClient *client, guint latency)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cvLatency = latency;
}

void
gst_rtsp_wfd_client_set_video_max_height(GstRTSPWFDClient *client, guint32 height)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cMaxHeight = height;
}

void
gst_rtsp_wfd_client_set_video_max_width(GstRTSPWFDClient *client, guint32 width)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cMaxWidth = width;
}

void
gst_rtsp_wfd_client_set_video_framerate(GstRTSPWFDClient *client, guint32 framerate)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cFramerate = framerate;
}

void
gst_rtsp_wfd_client_set_video_min_slice_size(GstRTSPWFDClient *client, guint32 slice_size)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cmin_slice_size = slice_size;
}

void
gst_rtsp_wfd_client_set_video_slice_enc_params(GstRTSPWFDClient *client, guint32 enc_params)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cslice_enc_params = enc_params;
}

void
gst_rtsp_wfd_client_set_video_framerate_control(GstRTSPWFDClient *client, guint framerate)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->cframe_rate_control = framerate;
}

void
gst_rtsp_wfd_client_set_rtp_port0(GstRTSPWFDClient *client, guint32 port)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->crtp_port0 = port;
}

void
gst_rtsp_wfd_client_set_rtp_port1(GstRTSPWFDClient *client, guint32 port)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  g_return_if_fail (priv != NULL);

  priv->crtp_port1 = port;
}
