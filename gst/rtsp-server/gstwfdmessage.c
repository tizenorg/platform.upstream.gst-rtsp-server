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
/*
 * Unless otherwise indicated, Source Code is licensed under MIT license.
 * See further explanation attached in License Statement (distributed in the file
 * LICENSE).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * SECTION:gstwfdmessage
 * @short_description: Helper methods for dealing with WFD messages
 *
 * <refsect2>
 * <para>
 * The GstWFDMessage helper functions makes it easy to parse and create WFD
 * messages.
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "gstwfdmessage.h"

#define EDID_BLOCK_SIZE 128
#define EDID_BLOCK_COUNT_MAX_SIZE 256
#define MAX_PORT_SIZE 65535

#define FREE_STRING(field)              g_free (field); (field) = NULL
#define REPLACE_STRING(field, val)      FREE_STRING(field); (field) = g_strdup (val)

#define INIT_ARRAY(field, type, init_func)              \
G_STMT_START {                                          \
  if (field) {                                          \
    guint i;                                            \
    for(i = 0; i < (field)->len; i++)                   \
      init_func (&g_array_index ((field), type, i));    \
    g_array_set_size ((field), 0);                      \
  }                                                     \
  else                                                  \
    (field) = g_array_new (FALSE, TRUE, sizeof (type)); \
} G_STMT_END

#define FREE_ARRAY(field)         \
G_STMT_START {                    \
  if (field)                      \
    g_array_free ((field), TRUE); \
  (field) = NULL;                 \
} G_STMT_END

#define DEFINE_STRING_SETTER(field)                                     \
GstWFDResult gst_wfd_message_set_##field (GstWFDMessage *msg, const gchar *val) { \
  g_free (msg->field);                                                  \
  msg->field = g_strdup (val);                                          \
  return GST_WFD_OK;                                                    \
}
#define DEFINE_STRING_GETTER(field)                                     \
const gchar* gst_wfd_message_get_##field (const GstWFDMessage *msg) {   \
  return msg->field;                                                    \
}

#define DEFINE_ARRAY_LEN(field)                                         \
guint gst_wfd_message_##field##_len (const GstWFDMessage *msg) {        \
  return msg->field->len;                                               \
}
#define DEFINE_ARRAY_GETTER(method, field, type)                        \
const type * gst_wfd_message_get_##method (const GstWFDMessage *msg, guint idx) {  \
  return &g_array_index (msg->field, type, idx);                        \
}
#define DEFINE_PTR_ARRAY_GETTER(method, field, type)                    \
const type gst_wfd_message_get_##method (const GstWFDMessage *msg, guint idx) {    \
  return g_array_index (msg->field, type, idx);                         \
}
#define DEFINE_ARRAY_INSERT(method, field, intype, dup_method, type)         \
GstWFDResult gst_wfd_message_insert_##method (GstWFDMessage *msg, gint idx, intype val) {   \
  type vt;                                                              \
  type* v = &vt;                                                         \
  dup_method (v, val);                                                  \
  if (idx == -1)                                                        \
    g_array_append_val (msg->field, vt);                                \
  else                                                                  \
    g_array_insert_val (msg->field, idx, vt);                           \
  return GST_WFD_OK;                                                    \
}

#define DEFINE_ARRAY_REPLACE(method, field, intype, free_method, dup_method, type)         \
GstWFDResult gst_wfd_message_replace_##method (GstWFDMessage *msg, guint idx, intype val) {   \
  type *v = &g_array_index (msg->field, type, idx);                   \
  free_method (v);                                                    \
  dup_method (v, val);                                                  \
  return GST_WFD_OK;                                                    \
}
#define DEFINE_ARRAY_REMOVE(method, field, type, free_method)                        \
GstWFDResult gst_wfd_message_remove_##method (GstWFDMessage *msg, guint idx) {  \
  type *v = &g_array_index (msg->field, type, idx);                     \
  free_method (v);                                                      \
  g_array_remove_index (msg->field, idx);                               \
  return GST_WFD_OK;                                                    \
}
#define DEFINE_ARRAY_ADDER(method, type)                                \
GstWFDResult gst_wfd_message_add_##method (GstWFDMessage *msg, const type val) {   \
  return gst_wfd_message_insert_##method (msg, -1, val);                \
}

#define dup_string(v,val) ((*v) = g_strdup (val))
#define INIT_STR_ARRAY(field) \
    INIT_ARRAY (field, gchar *, free_string)
#define DEFINE_STR_ARRAY_GETTER(method, field) \
    DEFINE_PTR_ARRAY_GETTER(method, field, gchar *)
#define DEFINE_STR_ARRAY_INSERT(method, field) \
    DEFINE_ARRAY_INSERT (method, field, const gchar *, dup_string, gchar *)
#define DEFINE_STR_ARRAY_ADDER(method, field) \
    DEFINE_ARRAY_ADDER (method, gchar *)
#define DEFINE_STR_ARRAY_REPLACE(method, field) \
    DEFINE_ARRAY_REPLACE (method, field, const gchar *, free_string, dup_string, gchar *)
#define DEFINE_STR_ARRAY_REMOVE(method, field) \
    DEFINE_ARRAY_REMOVE (method, field, gchar *, free_string)

static GstWFDMessage *gst_wfd_message_boxed_copy (GstWFDMessage * orig);
static void gst_wfd_message_boxed_free (GstWFDMessage * msg);

G_DEFINE_BOXED_TYPE (GstWFDMessage, gst_wfd_message, gst_wfd_message_boxed_copy,
    gst_wfd_message_boxed_free);

static GstWFDMessage *
gst_wfd_message_boxed_copy (GstWFDMessage * orig)
{
  GstWFDMessage *copy;

  if (gst_wfd_message_copy (orig, &copy) == GST_WFD_OK)
    return copy;

  return NULL;
}

static void
gst_wfd_message_boxed_free (GstWFDMessage * msg)
{
  gst_wfd_message_free (msg);
}

/**
 * gst_wfd_message_new:
 * @msg: (out) (transfer full): pointer to new #GstWFDMessage
 *
 * Allocate a new GstWFDMessage and store the result in @msg.
 *
 * Returns: a #GstWFDResult.
 */
GstWFDResult
gst_wfd_message_new (GstWFDMessage ** msg)
{
  GstWFDMessage *newmsg;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  newmsg = g_new0 (GstWFDMessage, 1);

  *msg = newmsg;

  return gst_wfd_message_init (newmsg);
}

/**
 * gst_wfd_message_init:
 * @msg: a #GstWFDMessage
 *
 * Initialize @msg so that its contents are as if it was freshly allocated
 * with gst_wfd_message_new(). This function is mostly used to initialize a message
 * allocated on the stack. gst_wfd_message_uninit() undoes this operation.
 *
 * When this function is invoked on newly allocated data (with malloc or on the
 * stack), its contents should be set to 0 before calling this function.
 *
 * Returns: a #GstWFDResult.
 */
GstWFDResult
gst_wfd_message_init (GstWFDMessage * msg)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  return GST_WFD_OK;
}

/**
 * gst_wfd_message_uninit:
 * @msg: a #GstWFDMessage
 *
 * Free all resources allocated in @msg. @msg should not be used anymore after
 * this function. This function should be used when @msg was allocated on the
 * stack and initialized with gst_wfd_message_init().
 *
 * Returns: a #GstWFDResult.
 */
GstWFDResult
gst_wfd_message_uninit (GstWFDMessage * msg)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (msg->audio_codecs) {
    guint i = 0;
    if (msg->audio_codecs->list) {
      for (; i < msg->audio_codecs->count; i++) {
        FREE_STRING(msg->audio_codecs->list[i].audio_format);
        msg->audio_codecs->list[i].modes = 0;
        msg->audio_codecs->list[i].latency = 0;
      }
      FREE_STRING(msg->audio_codecs->list);
    }
    FREE_STRING(msg->audio_codecs);
  }

  if (msg->video_formats) {
    FREE_STRING(msg->video_formats->list);
    FREE_STRING(msg->video_formats);
  }

  if (msg->video_3d_formats) {
    FREE_STRING(msg->video_3d_formats->list);
    FREE_STRING(msg->video_3d_formats);
  }

  if (msg->content_protection) {
    if (msg->content_protection->hdcp2_spec) {
      FREE_STRING(msg->content_protection->hdcp2_spec->hdcpversion);
      FREE_STRING(msg->content_protection->hdcp2_spec->TCPPort);
      FREE_STRING(msg->content_protection->hdcp2_spec);
    }
    FREE_STRING(msg->content_protection);
  }

  if (msg->display_edid) {
    if (msg->display_edid->edid_payload)
      FREE_STRING(msg->display_edid->edid_payload);
    FREE_STRING(msg->display_edid);
  }

  if (msg->coupled_sink) {
    if (msg->coupled_sink->coupled_sink_cap) {
      FREE_STRING(msg->coupled_sink->coupled_sink_cap->sink_address);
      FREE_STRING(msg->coupled_sink->coupled_sink_cap);
    }
    FREE_STRING(msg->coupled_sink);
  }

  if (msg->trigger_method) {
    FREE_STRING(msg->trigger_method->wfd_trigger_method);
    FREE_STRING(msg->trigger_method);
  }

  if (msg->presentation_url) {
    FREE_STRING(msg->presentation_url->wfd_url0);
    FREE_STRING(msg->presentation_url->wfd_url1);
    FREE_STRING(msg->presentation_url);
  }

  if (msg->client_rtp_ports) {
    FREE_STRING(msg->client_rtp_ports->profile);
    FREE_STRING(msg->client_rtp_ports->mode);
    FREE_STRING(msg->client_rtp_ports);
  }

  if (msg->route) {
    FREE_STRING(msg->route->destination);
    FREE_STRING(msg->route);
  }

  if (msg->I2C) {
    FREE_STRING(msg->I2C);
  }

  if (msg->av_format_change_timing) {
    FREE_STRING(msg->av_format_change_timing);
  }

  if (msg->preferred_display_mode) {
    FREE_STRING(msg->preferred_display_mode);
  }

  if (msg->standby_resume_capability) {
    FREE_STRING(msg->standby_resume_capability);
  }

  if (msg->standby) {
    FREE_STRING(msg->standby);
  }

  if (msg->connector_type) {
    FREE_STRING(msg->connector_type);
  }

  if (msg->idr_request) {
    FREE_STRING(msg->idr_request);
  }

  return GST_WFD_OK;
}

/**
 * gst_wfd_message_copy:
 * @msg: a #GstWFDMessage
 * @copy: (out) (transfer full): pointer to new #GstWFDMessage
 *
 * Allocate a new copy of @msg and store the result in @copy. The value in
 * @copy should be release with gst_wfd_message_free function.
 *
 * Returns: a #GstWFDResult
 *
 * Since: 1.6
 */
GstWFDResult
gst_wfd_message_copy (const GstWFDMessage * msg, GstWFDMessage ** copy)
{
  GstWFDResult ret;
  GstWFDMessage *cp;

  if (msg == NULL)
    return GST_WFD_EINVAL;

  ret = gst_wfd_message_new (copy);
  if (ret != GST_WFD_OK)
    return ret;

  cp = *copy;

  /* TODO-WFD */
  if (msg->client_rtp_ports) {
    cp->client_rtp_ports = g_malloc (sizeof (GstWFDClientRtpPorts));
    if (cp->client_rtp_ports) {
      cp->client_rtp_ports->profile = g_strdup (msg->client_rtp_ports->profile);
      cp->client_rtp_ports->rtp_port0 = msg->client_rtp_ports->rtp_port0;
      cp->client_rtp_ports->rtp_port1 = msg->client_rtp_ports->rtp_port1;
      cp->client_rtp_ports->mode = g_strdup (msg->client_rtp_ports->mode);
    }
  }

  return GST_WFD_OK;
}


static void
_read_string_space_ended (gchar * dest, guint size, gchar * src)
{
  guint idx = 0;

  while (!g_ascii_isspace (*src) && *src != '\0') {
    if (idx < size - 1)
      dest[idx++] = *src;
    src++;
  }

  if (size > 0)
    dest[idx] = '\0';

  return;
}

static void
_read_string_attr_and_value (gchar * attr, gchar * value, guint tsize,
    guint vsize, gchar del, gchar * src)
{
  guint idx;

  idx = 0;

  while (*src != del && *src != '\0') {
    if (idx < tsize - 1)
      attr[idx++] = *src;
    src++;
  }

  if (tsize > 0)
    attr[idx] = '\0';

  src++;
  idx = 0;

  while (*src != '\0') {
    if (idx < vsize - 1)
      value[idx++] = *src;
    src++;
  }

  if (vsize > 0)
    value[idx] = '\0';

  return;
}

static void
gst_wfd_parse_attribute (gchar * buffer, GstWFDMessage * msg)
{
  gchar attr[8192] = { 0 };
  gchar value[8192] = { 0 };
  gchar temp[8192] = { 0 };
  gchar *p = buffer;
  gchar *v = value;

#define WFD_SKIP_SPACE(q) if (*q && g_ascii_isspace (*q)) q++
#define WFD_SKIP_EQUAL(q) if (*q && *q == '=') q++
#define WFD_SKIP_COMMA(q) if (*q && g_ascii_ispunct (*q)) q++
#define WFD_READ_STRING(field) _read_string_space_ended (temp, sizeof (temp), v); v+=strlen(temp); REPLACE_STRING (field, temp)
#define WFD_READ_UINT32(field) _read_string_space_ended (temp, sizeof (temp), v); v+=strlen(temp); field = strtoul (temp, NULL, 16)
#define WFD_READ_UINT32_DIGIT(field) _read_string_space_ended (temp, sizeof (temp), v); v+=strlen(temp); field = strtoul (temp, NULL, 10)

  _read_string_attr_and_value (attr, value, sizeof (attr), sizeof (value), ':',
      p);

  if (!g_strcmp0 (attr, GST_STRING_WFD_AUDIO_CODECS)) {
    msg->audio_codecs = g_new0 (GstWFDAudioCodeclist, 1);
    if (strlen (v)) {
      guint i = 0;
      msg->audio_codecs->count = strlen (v) / 16;
      msg->audio_codecs->list =
          g_new0 (GstWFDAudioCodec, msg->audio_codecs->count);
      for (; i < msg->audio_codecs->count; i++) {
        WFD_SKIP_SPACE (v);
        WFD_READ_STRING (msg->audio_codecs->list[i].audio_format);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->audio_codecs->list[i].modes);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->audio_codecs->list[i].latency);
        WFD_SKIP_COMMA (v);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_VIDEO_FORMATS)) {
    msg->video_formats = g_new0 (GstWFDVideoCodeclist, 1);
    if (strlen (v)) {
      msg->video_formats->count = 1;
      msg->video_formats->list = g_new0 (GstWFDVideoCodec, 1);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->native);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->
          list->preferred_display_mode_supported);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.profile);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.level);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.CEA_Support);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.VESA_Support);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.HH_Support);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.latency);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.min_slice_size);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.slice_enc_params);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_formats->list->H264_codec.
          misc_params.frame_rate_control_support);
      WFD_SKIP_SPACE (v);
      if (msg->video_formats->list->preferred_display_mode_supported == 1) {
        WFD_READ_UINT32 (msg->video_formats->list->H264_codec.max_hres);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->video_formats->list->H264_codec.max_vres);
        WFD_SKIP_SPACE (v);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_3D_VIDEO_FORMATS)) {
    msg->video_3d_formats = g_new0 (GstWFD3DFormats, 1);
    if (strlen (v)) {
      msg->video_3d_formats->count = 1;
      msg->video_3d_formats->list = g_new0 (GstWFD3dCapList, 1);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->native);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->
          list->preferred_display_mode_supported);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.profile);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.level);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.
          misc_params.video_3d_capability);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.
          misc_params.latency);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.
          misc_params.min_slice_size);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.
          misc_params.slice_enc_params);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->video_3d_formats->list->H264_codec.
          misc_params.frame_rate_control_support);
      WFD_SKIP_SPACE (v);
      if (msg->video_formats->list->preferred_display_mode_supported == 1) {
        WFD_READ_UINT32 (msg->video_formats->list->H264_codec.max_hres);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->video_formats->list->H264_codec.max_vres);
        WFD_SKIP_SPACE (v);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_CONTENT_PROTECTION)) {
    msg->content_protection = g_new0 (GstWFDContentProtection, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      msg->content_protection->hdcp2_spec = g_new0 (GstWFDHdcp2Spec, 1);
      if (strstr (v, "none")) {
        msg->content_protection->hdcp2_spec->hdcpversion = g_strdup ("none");
      } else {
        WFD_READ_STRING (msg->content_protection->hdcp2_spec->hdcpversion);
        WFD_SKIP_SPACE (v);
        WFD_READ_STRING (msg->content_protection->hdcp2_spec->TCPPort);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_DISPLAY_EDID)) {
    msg->display_edid = g_new0 (GstWFDDisplayEdid, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      if (strstr (v, "none")) {
        msg->display_edid->edid_supported = 0;
      } else {
        msg->display_edid->edid_supported = 1;
        WFD_READ_UINT32 (msg->display_edid->edid_block_count);
        WFD_SKIP_SPACE (v);
        if (msg->display_edid->edid_block_count) {
          gchar *edid_string = v;
          int i = 0, j = 0;
          guint32 payload_size =
              EDID_BLOCK_SIZE * msg->display_edid->edid_block_count;
          msg->display_edid->edid_payload = g_malloc (payload_size);
          for (;
              i < (EDID_BLOCK_SIZE * msg->display_edid->edid_block_count * 2);
              j++) {
            int k = 0, kk = 0;
            if (edid_string[i] > 0x29 && edid_string[i] < 0x40)
              k = edid_string[i] - 48;
            else if (edid_string[i] > 0x60 && edid_string[i] < 0x67)
              k = edid_string[i] - 87;
            else if (edid_string[i] > 0x40 && edid_string[i] < 0x47)
              k = edid_string[i] - 55;

            if (edid_string[i + 1] > 0x29 && edid_string[i + 1] < 0x40)
              kk = edid_string[i + 1] - 48;
            else if (edid_string[i + 1] > 0x60 && edid_string[i + 1] < 0x67)
              kk = edid_string[i + 1] - 87;
            else if (edid_string[i + 1] > 0x40 && edid_string[i + 1] < 0x47)
              kk = edid_string[i + 1] - 55;

            msg->display_edid->edid_payload[j] = (k << 4) | kk;
            i += 2;
          }
          //memcpy(msg->display_edid->edid_payload, v, payload_size);
          v += (payload_size * 2);
        } else
          v += strlen (v);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_COUPLED_SINK)) {
    msg->coupled_sink = g_new0 (GstWFDCoupledSink, 1);
    if (strlen (v)) {
      msg->coupled_sink->coupled_sink_cap = g_new0 (GstWFDCoupled_sink_cap, 1);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->coupled_sink->coupled_sink_cap->status);
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->coupled_sink->coupled_sink_cap->sink_address);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_TRIGGER_METHOD)) {
    msg->trigger_method = g_new0 (GstWFDTriggerMethod, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->trigger_method->wfd_trigger_method);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_PRESENTATION_URL)) {
    msg->presentation_url = g_new0 (GstWFDPresentationUrl, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->presentation_url->wfd_url0);
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->presentation_url->wfd_url1);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_CLIENT_RTP_PORTS)) {
    msg->client_rtp_ports = g_new0 (GstWFDClientRtpPorts, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->client_rtp_ports->profile);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32_DIGIT (msg->client_rtp_ports->rtp_port0);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32_DIGIT (msg->client_rtp_ports->rtp_port1);
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->client_rtp_ports->mode);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_ROUTE)) {
    msg->route = g_new0 (GstWFDRoute, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      WFD_READ_STRING (msg->route->destination);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_I2C)) {
    msg->I2C = g_new0 (GstWFDI2C, 1);
    if (strlen (v)) {
      msg->I2C->I2CPresent = TRUE;
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32_DIGIT (msg->I2C->I2C_port);
      if (msg->I2C->I2C_port)
        msg->I2C->I2CPresent = TRUE;
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_AV_FORMAT_CHANGE_TIMING)) {
    msg->av_format_change_timing = g_new0 (GstWFDAVFormatChangeTiming, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->av_format_change_timing->PTS);
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->av_format_change_timing->DTS);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_PREFERRED_DISPLAY_MODE)) {
    msg->preferred_display_mode = g_new0 (GstWFDPreferredDisplayMode, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      if (!strstr (v, "none")) {
        msg->preferred_display_mode->displaymodesupported = FALSE;
      } else {
        WFD_READ_UINT32 (msg->preferred_display_mode->p_clock);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->HB);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->HSPOL_HSOFF);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->HSW);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->V);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->VB);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->VSPOL_VSOFF);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->VSW);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->VBS3D);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->V2d_s3d_modes);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->P_depth);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.profile);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.level);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.CEA_Support);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.VESA_Support);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.HH_Support);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.latency);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.min_slice_size);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.slice_enc_params);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.
            misc_params.frame_rate_control_support);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.max_hres);
        WFD_SKIP_SPACE (v);
        WFD_READ_UINT32 (msg->preferred_display_mode->H264_codec.max_vres);
        WFD_SKIP_SPACE (v);
      }
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_STANDBY_RESUME_CAPABILITY)) {
    msg->standby_resume_capability = g_new0 (GstWFDStandbyResumeCapability, 1);
    if (strlen (v)) {
      WFD_SKIP_SPACE (v);
      if (!g_strcmp0 (v, "supported"))
        msg->standby_resume_capability->standby_resume_cap = TRUE;
      else
        msg->standby_resume_capability->standby_resume_cap = FALSE;
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_STANDBY)) {
    msg->standby = g_new0 (GstWFDStandby, 1);
    msg->standby->wfd_standby = TRUE;
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_CONNECTOR_TYPE)) {
    msg->connector_type = g_new0 (GstWFDConnectorType, 1);
    if (strlen (v)) {
      msg->connector_type->supported = TRUE;
      WFD_SKIP_SPACE (v);
      WFD_READ_UINT32 (msg->connector_type->connector_type);
    }
  } else if (!g_strcmp0 (attr, GST_STRING_WFD_IDR_REQUEST)) {
    msg->idr_request = g_new0 (GstWFDIdrRequest, 1);
    msg->idr_request->idr_request = TRUE;
  }
  return;
}

/**
 * gst_wfd_message_parse_buffer:
 * @data: the start of the buffer
 * @size: the size of the buffer
 * @msg: the result #GstSDPMessage
 *
 * Parse the contents of @size bytes pointed to by @data and store the result in
 * @msg.
 *
 * Returns: #GST_SDP_OK on success.
 */
GstWFDResult
gst_wfd_message_parse_buffer (const guint8 * data, guint size,
    GstWFDMessage * msg)
{
  gchar *p;
  gchar buffer[255] = { 0 };
  guint idx = 0;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (data != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (size != 0, GST_WFD_EINVAL);

  p = (gchar *) data;
  while (TRUE) {

    if (*p == '\0')
      break;

    idx = 0;
    while (*p != '\n' && *p != '\r' && *p != '\0') {
      if (idx < sizeof (buffer) - 1)
        buffer[idx++] = *p;
      p++;
    }
    buffer[idx] = '\0';
    gst_wfd_parse_attribute (buffer, msg);

    if (*p == '\0')
      break;
    p += 2;
  }
  return GST_WFD_OK;
}

/**
 * gst_wfd_message_free:
 * @msg: a #GstWFDMessage
 *
 * Free all resources allocated by @msg. @msg should not be used anymore after
 * this function. This function should be used when @msg was dynamically
 * allocated with gst_wfd_message_new().
 *
 * Returns: a #GstWFDResult.
 */
GstWFDResult
gst_wfd_message_free (GstWFDMessage * msg)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  gst_wfd_message_uninit (msg);
  g_free (msg);

  return GST_WFD_OK;
}

/**
 * gst_wfd_message_as_text:
 * @msg: a #GstWFDMessage
 *
 * Convert the contents of @msg to a text string.
 *
 * Returns: A dynamically allocated string representing the WFD description.
 */
gchar *
gst_wfd_message_as_text (const GstWFDMessage * msg)
{
  /* change all vars so they match rfc? */
  GString *lines;
  guint i;

  g_return_val_if_fail (msg != NULL, NULL);

  lines = g_string_new ("");

  /* list of audio codecs */
  if (msg->audio_codecs) {
    g_string_append_printf (lines, GST_STRING_WFD_AUDIO_CODECS);
    if (msg->audio_codecs->list) {
      g_string_append_printf (lines, ":");
      for (i = 0; i < msg->audio_codecs->count; i++) {
        g_string_append_printf (lines, " %s",
            msg->audio_codecs->list[i].audio_format);
        g_string_append_printf (lines, " %08x",
            msg->audio_codecs->list[i].modes);
        g_string_append_printf (lines, " %02x",
            msg->audio_codecs->list[i].latency);
        if ((i + 1) < msg->audio_codecs->count)
          g_string_append_printf (lines, ",");
      }
    }
    g_string_append_printf (lines, "\r\n");
  }

  /* list of video codecs */
  if (msg->video_formats) {
    g_string_append_printf (lines, GST_STRING_WFD_VIDEO_FORMATS);
    if (msg->video_formats->list) {
      g_string_append_printf (lines, ":");
      g_string_append_printf (lines, " %02x", msg->video_formats->list->native);
      g_string_append_printf (lines, " %02x",
          msg->video_formats->list->preferred_display_mode_supported);
      g_string_append_printf (lines, " %02x",
          msg->video_formats->list->H264_codec.profile);
      g_string_append_printf (lines, " %02x",
          msg->video_formats->list->H264_codec.level);
      g_string_append_printf (lines, " %08x",
          msg->video_formats->list->H264_codec.misc_params.CEA_Support);
      g_string_append_printf (lines, " %08x",
          msg->video_formats->list->H264_codec.misc_params.VESA_Support);
      g_string_append_printf (lines, " %08x",
          msg->video_formats->list->H264_codec.misc_params.HH_Support);
      g_string_append_printf (lines, " %02x",
          msg->video_formats->list->H264_codec.misc_params.latency);
      g_string_append_printf (lines, " %04x",
          msg->video_formats->list->H264_codec.misc_params.min_slice_size);
      g_string_append_printf (lines, " %04x",
          msg->video_formats->list->H264_codec.misc_params.slice_enc_params);
      g_string_append_printf (lines, " %02x",
          msg->video_formats->list->H264_codec.
          misc_params.frame_rate_control_support);

      if (msg->video_formats->list->H264_codec.max_hres)
        g_string_append_printf (lines, " %04x",
            msg->video_formats->list->H264_codec.max_hres);
      else
        g_string_append_printf (lines, " none");

      if (msg->video_formats->list->H264_codec.max_vres)
        g_string_append_printf (lines, " %04x",
            msg->video_formats->list->H264_codec.max_vres);
      else
        g_string_append_printf (lines, " none");
    }
    g_string_append_printf (lines, "\r\n");
  }

  /* list of video 3D codecs */
  if (msg->video_3d_formats) {
    g_string_append_printf (lines, GST_STRING_WFD_3D_VIDEO_FORMATS);
    g_string_append_printf (lines, ":");
    if (msg->video_3d_formats->list) {
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->native);
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->preferred_display_mode_supported);
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->H264_codec.profile);
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->H264_codec.level);
      g_string_append_printf (lines, " %16x",
          msg->video_3d_formats->list->H264_codec.
          misc_params.video_3d_capability);
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->H264_codec.misc_params.latency);
      g_string_append_printf (lines, " %04x",
          msg->video_3d_formats->list->H264_codec.misc_params.min_slice_size);
      g_string_append_printf (lines, " %04x",
          msg->video_3d_formats->list->H264_codec.misc_params.slice_enc_params);
      g_string_append_printf (lines, " %02x",
          msg->video_3d_formats->list->H264_codec.
          misc_params.frame_rate_control_support);
      if (msg->video_3d_formats->list->H264_codec.max_hres)
        g_string_append_printf (lines, " %04x",
            msg->video_3d_formats->list->H264_codec.max_hres);
      else
        g_string_append_printf (lines, " none");
      if (msg->video_3d_formats->list->H264_codec.max_vres)
        g_string_append_printf (lines, " %04x",
            msg->video_3d_formats->list->H264_codec.max_vres);
      else
        g_string_append_printf (lines, " none");
    } else {
      g_string_append_printf (lines, " none");
    }
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->content_protection) {
    g_string_append_printf (lines, GST_STRING_WFD_CONTENT_PROTECTION);
    g_string_append_printf (lines, ":");
    if (msg->content_protection->hdcp2_spec) {
      if (msg->content_protection->hdcp2_spec->hdcpversion) {
        g_string_append_printf (lines, " %s",
            msg->content_protection->hdcp2_spec->hdcpversion);
        g_string_append_printf (lines, " %s",
            msg->content_protection->hdcp2_spec->TCPPort);
      } else {
        g_string_append_printf (lines, " none");
      }
    } else {
      g_string_append_printf (lines, " none");
    }
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->display_edid) {
    g_string_append_printf (lines, GST_STRING_WFD_DISPLAY_EDID);
    g_string_append_printf (lines, ":");
    if (msg->display_edid->edid_supported) {
      if (msg->display_edid->edid_block_count > 0 &&
          msg->display_edid->edid_block_count <= EDID_BLOCK_COUNT_MAX_SIZE) {
        g_string_append_printf (lines," %04x", msg->display_edid->edid_block_count);
        g_string_append_printf(lines, " %s", msg->display_edid->edid_payload);
      } else
        g_string_append_printf (lines, " none");
    } else {
      g_string_append_printf (lines, " none");
    }
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->coupled_sink) {
    g_string_append_printf (lines, GST_STRING_WFD_COUPLED_SINK);
    g_string_append_printf (lines, ":");
    if (msg->coupled_sink->coupled_sink_cap) {
      g_string_append_printf (lines, " %02x",
          msg->coupled_sink->coupled_sink_cap->status);
      if (msg->coupled_sink->coupled_sink_cap->sink_address)
        g_string_append_printf (lines, " %s",
            msg->coupled_sink->coupled_sink_cap->sink_address);
      else
        g_string_append_printf (lines, " none");
    } else {
      g_string_append_printf (lines, " none");
    }
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->trigger_method) {
    g_string_append_printf (lines, GST_STRING_WFD_TRIGGER_METHOD);
    g_string_append_printf (lines, ":");
    g_string_append_printf (lines, " %s",
        msg->trigger_method->wfd_trigger_method);
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->presentation_url) {
    g_string_append_printf (lines, GST_STRING_WFD_PRESENTATION_URL);
    g_string_append_printf (lines, ":");
    if (msg->presentation_url->wfd_url0)
      g_string_append_printf (lines, " %s", msg->presentation_url->wfd_url0);
    else
      g_string_append_printf (lines, " none");
    if (msg->presentation_url->wfd_url1)
      g_string_append_printf (lines, " %s", msg->presentation_url->wfd_url1);
    else
      g_string_append_printf (lines, " none");
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->client_rtp_ports) {
    g_string_append_printf (lines, GST_STRING_WFD_CLIENT_RTP_PORTS);
    if (msg->client_rtp_ports->profile) {
      g_string_append_printf (lines, ":");
      g_string_append_printf (lines, " %s", msg->client_rtp_ports->profile);
      g_string_append_printf (lines, " %d", msg->client_rtp_ports->rtp_port0);
      g_string_append_printf (lines, " %d", msg->client_rtp_ports->rtp_port1);
      g_string_append_printf (lines, " %s", msg->client_rtp_ports->mode);
    }
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->route) {
    g_string_append_printf (lines, GST_STRING_WFD_ROUTE);
    g_string_append_printf (lines, ":");
    g_string_append_printf (lines, " %s", msg->route->destination);
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->I2C) {
    g_string_append_printf (lines, GST_STRING_WFD_I2C);
    g_string_append_printf (lines, ":");
    if (msg->I2C->I2CPresent)
      g_string_append_printf (lines, " %x", msg->I2C->I2C_port);
    else
      g_string_append_printf (lines, " none");
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->av_format_change_timing) {
    g_string_append_printf (lines, GST_STRING_WFD_AV_FORMAT_CHANGE_TIMING);
    g_string_append_printf (lines, ":");
    g_string_append_printf (lines, " %010llx",
        msg->av_format_change_timing->PTS);
    g_string_append_printf (lines, " %010llx",
        msg->av_format_change_timing->DTS);
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->preferred_display_mode) {
    g_string_append_printf (lines, GST_STRING_WFD_PREFERRED_DISPLAY_MODE);
    g_string_append_printf (lines, ":");
    if (msg->preferred_display_mode->displaymodesupported) {
      g_string_append_printf (lines, " %06llx",
          msg->preferred_display_mode->p_clock);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->H);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->HB);
      g_string_append_printf (lines, " %04x",
          msg->preferred_display_mode->HSPOL_HSOFF);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->HSW);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->V);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->VB);
      g_string_append_printf (lines, " %04x",
          msg->preferred_display_mode->VSPOL_VSOFF);
      g_string_append_printf (lines, " %04x", msg->preferred_display_mode->VSW);
      g_string_append_printf (lines, " %02x",
          msg->preferred_display_mode->VBS3D);
      g_string_append_printf (lines, " %02x",
          msg->preferred_display_mode->V2d_s3d_modes);
      g_string_append_printf (lines, " %02x",
          msg->preferred_display_mode->P_depth);
    } else
      g_string_append_printf (lines, " none");
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->standby_resume_capability) {
    g_string_append_printf (lines, GST_STRING_WFD_STANDBY_RESUME_CAPABILITY);
    g_string_append_printf (lines, ":");
    if (msg->standby_resume_capability->standby_resume_cap)
      g_string_append_printf (lines, " supported");
    else
      g_string_append_printf (lines, " none");
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->standby) {
    g_string_append_printf (lines, GST_STRING_WFD_STANDBY);
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->connector_type) {
    g_string_append_printf (lines, GST_STRING_WFD_CONNECTOR_TYPE);
    g_string_append_printf (lines, ":");
    if (msg->connector_type->connector_type)
      g_string_append_printf (lines, " %02x",
          msg->connector_type->connector_type);
    else
      g_string_append_printf (lines, " none");
    g_string_append_printf (lines, "\r\n");
  }

  if (msg->idr_request) {
    g_string_append_printf (lines, GST_STRING_WFD_IDR_REQUEST);
    g_string_append_printf (lines, "\r\n");
  }

  return g_string_free (lines, FALSE);
}

gchar *
gst_wfd_message_param_names_as_text (const GstWFDMessage * msg)
{
  /* change all vars so they match rfc? */
  GString *lines;
  g_return_val_if_fail (msg != NULL, NULL);

  lines = g_string_new ("");

  /* list of audio codecs */
  if (msg->audio_codecs) {
    g_string_append_printf (lines, GST_STRING_WFD_AUDIO_CODECS);
    g_string_append_printf (lines, "\r\n");
  }
  /* list of video codecs */
  if (msg->video_formats) {
    g_string_append_printf (lines, GST_STRING_WFD_VIDEO_FORMATS);
    g_string_append_printf (lines, "\r\n");
  }
  /* list of video 3D codecs */
  if (msg->video_3d_formats) {
    g_string_append_printf (lines, GST_STRING_WFD_3D_VIDEO_FORMATS);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->content_protection) {
    g_string_append_printf (lines, GST_STRING_WFD_CONTENT_PROTECTION);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->display_edid) {
    g_string_append_printf (lines, GST_STRING_WFD_DISPLAY_EDID);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->coupled_sink) {
    g_string_append_printf (lines, GST_STRING_WFD_COUPLED_SINK);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->trigger_method) {
    g_string_append_printf (lines, GST_STRING_WFD_TRIGGER_METHOD);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->presentation_url) {
    g_string_append_printf (lines, GST_STRING_WFD_PRESENTATION_URL);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->client_rtp_ports) {
    g_string_append_printf (lines, GST_STRING_WFD_CLIENT_RTP_PORTS);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->route) {
    g_string_append_printf (lines, GST_STRING_WFD_ROUTE);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->I2C) {
    g_string_append_printf (lines, GST_STRING_WFD_I2C);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->av_format_change_timing) {
    g_string_append_printf (lines, GST_STRING_WFD_AV_FORMAT_CHANGE_TIMING);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->preferred_display_mode) {
    g_string_append_printf (lines, GST_STRING_WFD_PREFERRED_DISPLAY_MODE);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->standby_resume_capability) {
    g_string_append_printf (lines, GST_STRING_WFD_STANDBY_RESUME_CAPABILITY);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->standby) {
    g_string_append_printf (lines, GST_STRING_WFD_STANDBY);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->connector_type) {
    g_string_append_printf (lines, GST_STRING_WFD_CONNECTOR_TYPE);
    g_string_append_printf (lines, "\r\n");
  }
  if (msg->idr_request) {
    g_string_append_printf (lines, GST_STRING_WFD_IDR_REQUEST);
    g_string_append_printf (lines, "\r\n");
  }

  return g_string_free (lines, FALSE);
}

/**
 * gst_wfd_message_dump:
 * @msg: a #GstWFDMessage
 *
 * Dump the parsed contents of @msg to stdout.
 *
 * Returns: a #GstWFDResult.
 */
GstWFDResult
gst_wfd_message_dump (const GstWFDMessage * msg)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (msg->audio_codecs) {
    guint i = 0;
    g_print ("Audio supported formats : \n");
    for (; i < msg->audio_codecs->count; i++) {
      g_print ("Codec: %s\n", msg->audio_codecs->list[i].audio_format);
      if (!strcmp (msg->audio_codecs->list[i].audio_format, "LPCM")) {
        if (msg->audio_codecs->list[i].modes & GST_WFD_FREQ_44100)
          g_print ("	Freq: %d\n", 44100);
        if (msg->audio_codecs->list[i].modes & GST_WFD_FREQ_48000)
          g_print ("	Freq: %d\n", 48000);
        g_print ("	Channels: %d\n", 2);
      }
      if (!strcmp (msg->audio_codecs->list[i].audio_format, "AAC")) {
        g_print ("	Freq: %d\n", 48000);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_2)
          g_print ("	Channels: %d\n", 2);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_4)
          g_print ("	Channels: %d\n", 4);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_6)
          g_print ("	Channels: %d\n", 6);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_8)
          g_print ("	Channels: %d\n", 8);
      }
      if (!strcmp (msg->audio_codecs->list[i].audio_format, "AC3")) {
        g_print ("	Freq: %d\n", 48000);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_2)
          g_print ("	Channels: %d\n", 2);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_4)
          g_print ("	Channels: %d\n", 4);
        if (msg->audio_codecs->list[i].modes & GST_WFD_CHANNEL_6)
          g_print ("	Channels: %d\n", 6);
      }
      g_print ("	Bitwidth: %d\n", 16);
      g_print ("	Latency: %d\n", msg->audio_codecs->list[i].latency);
    }
  }


  if (msg->video_formats) {
    g_print ("Video supported formats : \n");
    if (msg->video_formats->list) {
      guint nativeindex = 0;
      g_print ("Codec: H264\n");
      if ((msg->video_formats->list->native & 0x7) ==
          GST_WFD_VIDEO_CEA_RESOLUTION) {
        g_print ("	Native type: CEA\n");
      } else if ((msg->video_formats->list->native & 0x7) ==
          GST_WFD_VIDEO_VESA_RESOLUTION) {
        g_print ("	Native type: VESA\n");
      } else if ((msg->video_formats->list->native & 0x7) ==
          GST_WFD_VIDEO_HH_RESOLUTION) {
        g_print ("	Native type: HH\n");
      }
      nativeindex = msg->video_formats->list->native >> 3;
      g_print ("	Resolution: %d\n", (1 << nativeindex));

      if (msg->video_formats->list->
          H264_codec.profile & GST_WFD_H264_BASE_PROFILE) {
        g_print ("	Profile: BASE\n");
      } else if (msg->video_formats->list->
          H264_codec.profile & GST_WFD_H264_HIGH_PROFILE) {
        g_print ("	Profile: HIGH\n");
      }
      if (msg->video_formats->list->H264_codec.level & GST_WFD_H264_LEVEL_3_1) {
        g_print ("	Level: 3.1\n");
      } else if (msg->video_formats->list->
          H264_codec.level & GST_WFD_H264_LEVEL_3_2) {
        g_print ("	Level: 3.2\n");
      } else if (msg->video_formats->list->
          H264_codec.level & GST_WFD_H264_LEVEL_4) {
        g_print ("	Level: 4\n");
      } else if (msg->video_formats->list->
          H264_codec.level & GST_WFD_H264_LEVEL_4_1) {
        g_print ("	Level: 4.1\n");
      } else if (msg->video_formats->list->
          H264_codec.level & GST_WFD_H264_LEVEL_4_2) {
        g_print ("	Level: 4.2\n");
      }
      g_print ("	Latency: %d\n",
          msg->video_formats->list->H264_codec.misc_params.latency);
      g_print ("	min_slice_size: %x\n",
          msg->video_formats->list->H264_codec.misc_params.min_slice_size);
      g_print ("	slice_enc_params: %x\n",
          msg->video_formats->list->H264_codec.misc_params.slice_enc_params);
      g_print ("	frame_rate_control_support: %x\n",
          msg->video_formats->list->H264_codec.
          misc_params.frame_rate_control_support);
      if (msg->video_formats->list->H264_codec.max_hres) {
        g_print ("	Max Height: %04d\n",
            msg->video_formats->list->H264_codec.max_hres);
      }
      if (msg->video_formats->list->H264_codec.max_vres) {
        g_print ("	Max Width: %04d\n",
            msg->video_formats->list->H264_codec.max_vres);
      }
    }
  }

  if (msg->video_3d_formats) {
    g_print ("wfd_3d_formats");
    g_print ("\r\n");
  }

  if (msg->content_protection) {
    g_print (GST_STRING_WFD_CONTENT_PROTECTION);
    g_print ("\r\n");
  }

  if (msg->display_edid) {
    g_print (GST_STRING_WFD_DISPLAY_EDID);
    g_print ("\r\n");
  }

  if (msg->coupled_sink) {
    g_print (GST_STRING_WFD_COUPLED_SINK);
    g_print ("\r\n");
  }

  if (msg->trigger_method) {
    g_print ("	Trigger type: %s\n", msg->trigger_method->wfd_trigger_method);
  }

  if (msg->presentation_url) {
    g_print (GST_STRING_WFD_PRESENTATION_URL);
    g_print ("\r\n");
  }

  if (msg->client_rtp_ports) {
    g_print (" Client RTP Ports : \n");
    if (msg->client_rtp_ports->profile) {
      g_print ("%s\n", msg->client_rtp_ports->profile);
      g_print ("	%d\n", msg->client_rtp_ports->rtp_port0);
      g_print ("	%d\n", msg->client_rtp_ports->rtp_port1);
      g_print ("	%s\n", msg->client_rtp_ports->mode);
    }
    g_print ("\r\n");
  }

  if (msg->route) {
    g_print (GST_STRING_WFD_ROUTE);
    g_print ("\r\n");
  }

  if (msg->I2C) {
    g_print (GST_STRING_WFD_I2C);
    g_print ("\r\n");
  }

  if (msg->av_format_change_timing) {
    g_print (GST_STRING_WFD_AV_FORMAT_CHANGE_TIMING);
    g_print ("\r\n");
  }

  if (msg->preferred_display_mode) {
    g_print (GST_STRING_WFD_PREFERRED_DISPLAY_MODE);
    g_print ("\r\n");
  }

  if (msg->standby_resume_capability) {
    g_print (GST_STRING_WFD_STANDBY_RESUME_CAPABILITY);
    g_print ("\r\n");
  }

  if (msg->standby) {
    g_print (GST_STRING_WFD_STANDBY);
    g_print ("\r\n");
  }

  if (msg->connector_type) {
    g_print (GST_STRING_WFD_CONNECTOR_TYPE);
    g_print ("\r\n");
  }

  if (msg->idr_request) {
    g_print (GST_STRING_WFD_IDR_REQUEST);
    g_print ("\r\n");
  }

  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_supported_audio_format (GstWFDMessage * msg,
    GstWFDAudioFormats a_codec,
    guint a_freq, guint a_channels, guint a_bitwidth, guint32 a_latency)
{
  guint i = 0;
  guint pcm = 0, aac = 0, ac3 = 0;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->audio_codecs)
    msg->audio_codecs = g_new0 (GstWFDAudioCodeclist, 1);

  if (a_codec != GST_WFD_AUDIO_UNKNOWN) {

    if (a_codec & GST_WFD_AUDIO_LPCM)
      msg->audio_codecs->count++;
    if (a_codec & GST_WFD_AUDIO_AAC)
      msg->audio_codecs->count++;
    if (a_codec & GST_WFD_AUDIO_AC3)
      msg->audio_codecs->count++;
    
    msg->audio_codecs->list =
        g_new0 (GstWFDAudioCodec, msg->audio_codecs->count);
    for (; i < msg->audio_codecs->count; i++) {
      if ((a_codec & GST_WFD_AUDIO_LPCM) && (!pcm)) {
        msg->audio_codecs->list[i].audio_format = g_strdup ("LPCM");
        msg->audio_codecs->list[i].modes = a_freq;
        msg->audio_codecs->list[i].latency = a_latency;
        pcm = 1;
      } else if ((a_codec & GST_WFD_AUDIO_AAC) && (!aac)) {
        msg->audio_codecs->list[i].audio_format = g_strdup ("AAC");
        msg->audio_codecs->list[i].modes = a_channels;
        msg->audio_codecs->list[i].latency = a_latency;
        aac = 1;
      } else if ((a_codec & GST_WFD_AUDIO_AC3) && (!ac3)) {
        msg->audio_codecs->list[i].audio_format = g_strdup ("AC3");
        msg->audio_codecs->list[i].modes = a_channels;
        msg->audio_codecs->list[i].latency = a_latency;
        ac3 = 1;
      }
    }
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_prefered_audio_format (GstWFDMessage * msg,
    GstWFDAudioFormats a_codec,
    GstWFDAudioFreq a_freq,
    GstWFDAudioChannels a_channels, guint a_bitwidth, guint32 a_latency)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->audio_codecs)
    msg->audio_codecs = g_new0 (GstWFDAudioCodeclist, 1);

  msg->audio_codecs->list = g_new0 (GstWFDAudioCodec, 1);
  msg->audio_codecs->count = 1;
  if (a_codec == GST_WFD_AUDIO_LPCM) {
    msg->audio_codecs->list->audio_format = g_strdup ("LPCM");
    msg->audio_codecs->list->modes = a_freq;
    msg->audio_codecs->list->latency = a_latency;
  } else if (a_codec == GST_WFD_AUDIO_AAC) {
    msg->audio_codecs->list->audio_format = g_strdup ("AAC");
    msg->audio_codecs->list->modes = a_channels;
    msg->audio_codecs->list->latency = a_latency;
  } else if (a_codec == GST_WFD_AUDIO_AC3) {
    msg->audio_codecs->list->audio_format = g_strdup ("AC3");
    msg->audio_codecs->list->modes = a_channels;
    msg->audio_codecs->list->latency = a_latency;
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_supported_audio_format (GstWFDMessage * msg,
    guint * a_codec,
    guint * a_freq, guint * a_channels, guint * a_bitwidth, guint32 * a_latency)
{
  guint i = 0;
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (msg->audio_codecs != NULL, GST_WFD_EINVAL);

  for (; i < msg->audio_codecs->count; i++) {
    if (!g_strcmp0 (msg->audio_codecs->list[i].audio_format, "LPCM")) {
      *a_codec |= GST_WFD_AUDIO_LPCM;
      *a_freq |= msg->audio_codecs->list[i].modes;
      *a_channels |= GST_WFD_CHANNEL_2;
      *a_bitwidth = 16;
      *a_latency = msg->audio_codecs->list[i].latency;
    } else if (!g_strcmp0 (msg->audio_codecs->list[i].audio_format, "AAC")) {
      *a_codec |= GST_WFD_AUDIO_AAC;
      *a_freq |= GST_WFD_FREQ_48000;
      *a_channels |= msg->audio_codecs->list[i].modes;
      *a_bitwidth = 16;
      *a_latency = msg->audio_codecs->list[i].latency;
    } else if (!g_strcmp0 (msg->audio_codecs->list[i].audio_format, "AC3")) {
      *a_codec |= GST_WFD_AUDIO_AC3;
      *a_freq |= GST_WFD_FREQ_48000;
      *a_channels |= msg->audio_codecs->list[i].modes;
      *a_bitwidth = 16;
      *a_latency = msg->audio_codecs->list[i].latency;
    }
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_prefered_audio_format (GstWFDMessage * msg,
    GstWFDAudioFormats * a_codec,
    GstWFDAudioFreq * a_freq,
    GstWFDAudioChannels * a_channels, guint * a_bitwidth, guint32 * a_latency)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!g_strcmp0 (msg->audio_codecs->list->audio_format, "LPCM")) {
    *a_codec = GST_WFD_AUDIO_LPCM;
    *a_freq = msg->audio_codecs->list->modes;
    *a_channels = GST_WFD_CHANNEL_2;
    *a_bitwidth = 16;
    *a_latency = msg->audio_codecs->list->latency;
  } else if (!g_strcmp0 (msg->audio_codecs->list->audio_format, "AAC")) {
    *a_codec = GST_WFD_AUDIO_AAC;
    *a_freq = GST_WFD_FREQ_48000;
    *a_channels = msg->audio_codecs->list->modes;
    *a_bitwidth = 16;
    *a_latency = msg->audio_codecs->list->latency;
  } else if (!g_strcmp0 (msg->audio_codecs->list->audio_format, "AC3")) {
    *a_codec = GST_WFD_AUDIO_AC3;
    *a_freq = GST_WFD_FREQ_48000;
    *a_channels = msg->audio_codecs->list->modes;
    *a_bitwidth = 16;
    *a_latency = msg->audio_codecs->list->latency;
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_supported_video_format (GstWFDMessage * msg,
    GstWFDVideoCodecs v_codec,
    GstWFDVideoNativeResolution v_native,
    guint64 v_native_resolution,
    guint64 v_cea_resolution,
    guint64 v_vesa_resolution,
    guint64 v_hh_resolution,
    guint v_profile,
    guint v_level,
    guint32 v_latency,
    guint32 v_max_height,
    guint32 v_max_width,
    guint32 min_slice_size, guint32 slice_enc_params, guint frame_rate_control)
{
  guint nativeindex = 0;
  guint64 temp = v_native_resolution;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->video_formats)
    msg->video_formats = g_new0 (GstWFDVideoCodeclist, 1);

  if (v_codec != GST_WFD_VIDEO_UNKNOWN) {
    msg->video_formats->list = g_new0 (GstWFDVideoCodec, 1);
    while (temp) {
      nativeindex++;
      temp >>= 1;
    }

    msg->video_formats->list->native = nativeindex - 1;
    msg->video_formats->list->native <<= 3;

    if (v_native == GST_WFD_VIDEO_VESA_RESOLUTION)
      msg->video_formats->list->native |= 1;
    else if (v_native == GST_WFD_VIDEO_HH_RESOLUTION)
      msg->video_formats->list->native |= 2;

    msg->video_formats->list->preferred_display_mode_supported = 1;
    msg->video_formats->list->H264_codec.profile = v_profile;
    msg->video_formats->list->H264_codec.level = v_level;
    msg->video_formats->list->H264_codec.max_hres = v_max_height;
    msg->video_formats->list->H264_codec.max_vres = v_max_width;
    msg->video_formats->list->H264_codec.misc_params.CEA_Support =
        v_cea_resolution;
    msg->video_formats->list->H264_codec.misc_params.VESA_Support =
        v_vesa_resolution;
    msg->video_formats->list->H264_codec.misc_params.HH_Support =
        v_hh_resolution;
    msg->video_formats->list->H264_codec.misc_params.latency = v_latency;
    msg->video_formats->list->H264_codec.misc_params.min_slice_size =
        min_slice_size;
    msg->video_formats->list->H264_codec.misc_params.slice_enc_params =
        slice_enc_params;
    msg->video_formats->list->H264_codec.
        misc_params.frame_rate_control_support = frame_rate_control;
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_prefered_video_format (GstWFDMessage * msg,
    GstWFDVideoCodecs v_codec,
    GstWFDVideoNativeResolution v_native,
    guint64 v_native_resolution,
    GstWFDVideoCEAResolution v_cea_resolution,
    GstWFDVideoVESAResolution v_vesa_resolution,
    GstWFDVideoHHResolution v_hh_resolution,
    GstWFDVideoH264Profile v_profile,
    GstWFDVideoH264Level v_level,
    guint32 v_latency,
    guint32 v_max_height,
    guint32 v_max_width,
    guint32 min_slice_size, guint32 slice_enc_params, guint frame_rate_control)
{
  guint nativeindex = 0;
  guint64 temp = v_native_resolution;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->video_formats)
    msg->video_formats = g_new0 (GstWFDVideoCodeclist, 1);
  msg->video_formats->list = g_new0 (GstWFDVideoCodec, 1);

  while (temp) {
    nativeindex++;
    temp >>= 1;
  }

  if (nativeindex)
    msg->video_formats->list->native = nativeindex - 1;
  msg->video_formats->list->native <<= 3;

  if (v_native == GST_WFD_VIDEO_VESA_RESOLUTION)
    msg->video_formats->list->native |= 1;
  else if (v_native == GST_WFD_VIDEO_HH_RESOLUTION)
    msg->video_formats->list->native |= 2;

  msg->video_formats->list->preferred_display_mode_supported = 0;
  msg->video_formats->list->H264_codec.profile = v_profile;
  msg->video_formats->list->H264_codec.level = v_level;
  msg->video_formats->list->H264_codec.max_hres = v_max_height;
  msg->video_formats->list->H264_codec.max_vres = v_max_width;
  msg->video_formats->list->H264_codec.misc_params.CEA_Support =
      v_cea_resolution;
  msg->video_formats->list->H264_codec.misc_params.VESA_Support =
      v_vesa_resolution;
  msg->video_formats->list->H264_codec.misc_params.HH_Support = v_hh_resolution;
  msg->video_formats->list->H264_codec.misc_params.latency = v_latency;
  msg->video_formats->list->H264_codec.misc_params.min_slice_size =
      min_slice_size;
  msg->video_formats->list->H264_codec.misc_params.slice_enc_params =
      slice_enc_params;
  msg->video_formats->list->H264_codec.misc_params.frame_rate_control_support =
      frame_rate_control;
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_supported_video_format (GstWFDMessage * msg,
    GstWFDVideoCodecs * v_codec,
    GstWFDVideoNativeResolution * v_native,
    guint64 * v_native_resolution,
    guint64 * v_cea_resolution,
    guint64 * v_vesa_resolution,
    guint64 * v_hh_resolution,
    guint * v_profile,
    guint * v_level,
    guint32 * v_latency,
    guint32 * v_max_height,
    guint32 * v_max_width,
    guint32 * min_slice_size,
    guint32 * slice_enc_params, guint * frame_rate_control)
{
  guint nativeindex = 0;

  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  *v_codec = GST_WFD_VIDEO_H264;
  *v_native = msg->video_formats->list->native & 0x7;
  nativeindex = msg->video_formats->list->native >> 3;
  *v_native_resolution = ((guint64) 1) << nativeindex;
  *v_profile = msg->video_formats->list->H264_codec.profile;
  *v_level = msg->video_formats->list->H264_codec.level;
  *v_max_height = msg->video_formats->list->H264_codec.max_hres;
  *v_max_width = msg->video_formats->list->H264_codec.max_vres;
  *v_cea_resolution =
      msg->video_formats->list->H264_codec.misc_params.CEA_Support;
  *v_vesa_resolution =
      msg->video_formats->list->H264_codec.misc_params.VESA_Support;
  *v_hh_resolution =
      msg->video_formats->list->H264_codec.misc_params.HH_Support;
  *v_latency = msg->video_formats->list->H264_codec.misc_params.latency;
  *min_slice_size =
      msg->video_formats->list->H264_codec.misc_params.min_slice_size;
  *slice_enc_params =
      msg->video_formats->list->H264_codec.misc_params.slice_enc_params;
  *frame_rate_control =
      msg->video_formats->list->H264_codec.
      misc_params.frame_rate_control_support;
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_prefered_video_format (GstWFDMessage * msg,
    GstWFDVideoCodecs * v_codec,
    GstWFDVideoNativeResolution * v_native,
    guint64 * v_native_resolution,
    GstWFDVideoCEAResolution * v_cea_resolution,
    GstWFDVideoVESAResolution * v_vesa_resolution,
    GstWFDVideoHHResolution * v_hh_resolution,
    GstWFDVideoH264Profile * v_profile,
    GstWFDVideoH264Level * v_level,
    guint32 * v_latency,
    guint32 * v_max_height,
    guint32 * v_max_width,
    guint32 * min_slice_size,
    guint32 * slice_enc_params, guint * frame_rate_control)
{
  guint nativeindex = 0;
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  *v_codec = GST_WFD_VIDEO_H264;
  *v_native = msg->video_formats->list->native & 0x7;
  nativeindex = msg->video_formats->list->native >> 3;
  *v_native_resolution = ((guint64) 1) << nativeindex;
  *v_profile = msg->video_formats->list->H264_codec.profile;
  *v_level = msg->video_formats->list->H264_codec.level;
  *v_max_height = msg->video_formats->list->H264_codec.max_hres;
  *v_max_width = msg->video_formats->list->H264_codec.max_vres;
  *v_cea_resolution =
      msg->video_formats->list->H264_codec.misc_params.CEA_Support;
  *v_vesa_resolution =
      msg->video_formats->list->H264_codec.misc_params.VESA_Support;
  *v_hh_resolution =
      msg->video_formats->list->H264_codec.misc_params.HH_Support;
  *v_latency = msg->video_formats->list->H264_codec.misc_params.latency;
  *min_slice_size =
      msg->video_formats->list->H264_codec.misc_params.min_slice_size;
  *slice_enc_params =
      msg->video_formats->list->H264_codec.misc_params.slice_enc_params;
  *frame_rate_control =
      msg->video_formats->list->H264_codec.
      misc_params.frame_rate_control_support;
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_display_edid (GstWFDMessage * msg,
    gboolean edid_supported, guint32 edid_blockcount, gchar * edid_playload)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  if (!msg->display_edid)
    msg->display_edid = g_new0 (GstWFDDisplayEdid, 1);
  msg->display_edid->edid_supported = edid_supported;
  if (!edid_supported)
    return GST_WFD_OK;
  if (edid_blockcount > 0 && edid_blockcount <= EDID_BLOCK_COUNT_MAX_SIZE) {
    msg->display_edid->edid_block_count = edid_blockcount;
    msg->display_edid->edid_payload = g_malloc(EDID_BLOCK_SIZE * edid_blockcount);
    if (msg->display_edid->edid_payload)
      memcpy(msg->display_edid->edid_payload, edid_playload, EDID_BLOCK_SIZE * edid_blockcount);
    else
      msg->display_edid->edid_supported = FALSE;
  } else
    msg->display_edid->edid_supported = FALSE;

  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_display_edid (GstWFDMessage * msg,
    gboolean * edid_supported,
    guint32 * edid_blockcount, gchar ** edid_playload)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (edid_supported != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (edid_blockcount != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (edid_playload != NULL, GST_WFD_EINVAL);

  *edid_supported = FALSE;
  if (msg->display_edid) {
    if (msg->display_edid->edid_supported) {
      *edid_blockcount = msg->display_edid->edid_block_count;
      if (msg->display_edid->edid_block_count > 0 && msg->display_edid->edid_block_count <= EDID_BLOCK_COUNT_MAX_SIZE) {
        char *temp;
        temp = g_malloc0(EDID_BLOCK_SIZE * msg->display_edid->edid_block_count);
        if (temp) {
          memcpy(temp, msg->display_edid->edid_payload, EDID_BLOCK_SIZE * msg->display_edid->edid_block_count);
          *edid_playload = temp;
          *edid_supported = TRUE;
        }
      }
    }
  }
  return GST_WFD_OK;
}


GstWFDResult
gst_wfd_message_set_contentprotection_type (GstWFDMessage * msg,
    GstWFDHDCPProtection hdcpversion, guint32 TCPPort)
{
  char str[11] = { 0, };
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (TCPPort <= MAX_PORT_SIZE, GST_WFD_EINVAL);

  if (!msg->content_protection)
    msg->content_protection = g_new0 (GstWFDContentProtection, 1);
  if (hdcpversion == GST_WFD_HDCP_NONE)
    return GST_WFD_OK;
  msg->content_protection->hdcp2_spec = g_new0 (GstWFDHdcp2Spec, 1);
  if (hdcpversion == GST_WFD_HDCP_2_0)
    msg->content_protection->hdcp2_spec->hdcpversion = g_strdup ("HDCP2.0");
  else if (hdcpversion == GST_WFD_HDCP_2_1)
    msg->content_protection->hdcp2_spec->hdcpversion = g_strdup ("HDCP2.1");
  snprintf (str, sizeof (str), "port=%d", TCPPort);
  msg->content_protection->hdcp2_spec->TCPPort = g_strdup (str);
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_contentprotection_type (GstWFDMessage * msg,
    GstWFDHDCPProtection * hdcpversion, guint32 * TCPPort)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  if (msg->content_protection && msg->content_protection->hdcp2_spec) {
    char *result = NULL;
    char *ptr = NULL;
    if (!g_strcmp0 (msg->content_protection->hdcp2_spec->hdcpversion, "none")) {
      *hdcpversion = GST_WFD_HDCP_NONE;
      *TCPPort = 0;
      return GST_WFD_OK;
    }
    if (!g_strcmp0 (msg->content_protection->hdcp2_spec->hdcpversion,
            "HDCP2.0"))
      *hdcpversion = GST_WFD_HDCP_2_0;
    else if (!g_strcmp0 (msg->content_protection->hdcp2_spec->hdcpversion,
            "HDCP2.1"))
      *hdcpversion = GST_WFD_HDCP_2_1;
    else {
      *hdcpversion = GST_WFD_HDCP_NONE;
      *TCPPort = 0;
      return GST_WFD_OK;
    }

    if (msg->content_protection->hdcp2_spec->TCPPort) {
      result = strtok_r (msg->content_protection->hdcp2_spec->TCPPort, "=", &ptr);
      while (result != NULL && ptr != NULL) {
        result = strtok_r (NULL, "=", &ptr);
        *TCPPort = atoi (result);
        break;
      }
    } else {
      *TCPPort = 0;
    }
  } else
    *hdcpversion = GST_WFD_HDCP_NONE;
  return GST_WFD_OK;
}


GstWFDResult
gst_wfd_messge_set_prefered_rtp_ports (GstWFDMessage * msg,
    GstWFDRTSPTransMode trans,
    GstWFDRTSPProfile profile,
    GstWFDRTSPLowerTrans lowertrans, guint32 rtp_port0, guint32 rtp_port1)
{
  GString *lines;
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->client_rtp_ports)
    msg->client_rtp_ports = g_new0 (GstWFDClientRtpPorts, 1);

  if (trans != GST_WFD_RTSP_TRANS_UNKNOWN) {
    lines = g_string_new ("");
    if (trans == GST_WFD_RTSP_TRANS_RTP)
      g_string_append_printf (lines, "RTP");
    else if (trans == GST_WFD_RTSP_TRANS_RDT)
      g_string_append_printf (lines, "RDT");

    if (profile == GST_WFD_RTSP_PROFILE_AVP)
      g_string_append_printf (lines, "/AVP");
    else if (profile == GST_WFD_RTSP_PROFILE_SAVP)
      g_string_append_printf (lines, "/SAVP");

    if (lowertrans == GST_WFD_RTSP_LOWER_TRANS_UDP)
      g_string_append_printf (lines, "/UDP;unicast");
    else if (lowertrans == GST_WFD_RTSP_LOWER_TRANS_UDP_MCAST)
      g_string_append_printf (lines, "/UDP;multicast");
    else if (lowertrans == GST_WFD_RTSP_LOWER_TRANS_TCP)
      g_string_append_printf (lines, "/TCP;unicast");
    else if (lowertrans == GST_WFD_RTSP_LOWER_TRANS_HTTP)
      g_string_append_printf (lines, "/HTTP");

    msg->client_rtp_ports->profile = g_string_free (lines, FALSE);
    msg->client_rtp_ports->rtp_port0 = rtp_port0;
    msg->client_rtp_ports->rtp_port1 = rtp_port1;
    msg->client_rtp_ports->mode = g_strdup ("mode=play");
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_prefered_rtp_ports (GstWFDMessage * msg,
    GstWFDRTSPTransMode * trans,
    GstWFDRTSPProfile * profile,
    GstWFDRTSPLowerTrans * lowertrans, guint32 * rtp_port0, guint32 * rtp_port1)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail (msg->client_rtp_ports != NULL, GST_WFD_EINVAL);

  if (g_strrstr (msg->client_rtp_ports->profile, "RTP"))
    *trans = GST_WFD_RTSP_TRANS_RTP;
  if (g_strrstr (msg->client_rtp_ports->profile, "RDT"))
    *trans = GST_WFD_RTSP_TRANS_RDT;
  if (g_strrstr (msg->client_rtp_ports->profile, "AVP"))
    *profile = GST_WFD_RTSP_PROFILE_AVP;
  if (g_strrstr (msg->client_rtp_ports->profile, "SAVP"))
    *profile = GST_WFD_RTSP_PROFILE_SAVP;
  if (g_strrstr (msg->client_rtp_ports->profile, "UDP;unicast"))
    *lowertrans = GST_WFD_RTSP_LOWER_TRANS_UDP;
  if (g_strrstr (msg->client_rtp_ports->profile, "UDP;multicast"))
    *lowertrans = GST_WFD_RTSP_LOWER_TRANS_UDP_MCAST;
  if (g_strrstr (msg->client_rtp_ports->profile, "TCP;unicast"))
    *lowertrans = GST_WFD_RTSP_LOWER_TRANS_TCP;
  if (g_strrstr (msg->client_rtp_ports->profile, "HTTP"))
    *lowertrans = GST_WFD_RTSP_LOWER_TRANS_HTTP;

  *rtp_port0 = msg->client_rtp_ports->rtp_port0;
  *rtp_port1 = msg->client_rtp_ports->rtp_port1;

  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_presentation_url (GstWFDMessage * msg, gchar * wfd_url0,
    gchar * wfd_url1)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (!msg->presentation_url)
    msg->presentation_url = g_new0 (GstWFDPresentationUrl, 1);
  if (wfd_url0)
    msg->presentation_url->wfd_url0 = g_strdup (wfd_url0);
  if (wfd_url1)
    msg->presentation_url->wfd_url1 = g_strdup (wfd_url1);
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_presentation_url (GstWFDMessage * msg, gchar ** wfd_url0,
    gchar ** wfd_url1)
{
  g_return_val_if_fail (msg != NULL, GST_WFD_EINVAL);

  if (msg->presentation_url) {
    *wfd_url0 = g_strdup (msg->presentation_url->wfd_url0);
    *wfd_url1 = g_strdup (msg->presentation_url->wfd_url1);
  }
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_set_av_format_change_timing(GstWFDMessage *msg, guint64 PTS, guint64 DTS)
{
  g_return_val_if_fail(msg != NULL, GST_WFD_EINVAL);

  if (!msg->av_format_change_timing)
    msg->av_format_change_timing = g_new0(GstWFDAVFormatChangeTiming, 1);

  msg->av_format_change_timing->PTS = PTS;
  msg->av_format_change_timing->DTS = DTS;
  return GST_WFD_OK;
}

GstWFDResult
gst_wfd_message_get_av_format_change_timing(GstWFDMessage *msg, guint64 *PTS, guint64 *DTS)
{
  g_return_val_if_fail(msg != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail(PTS != NULL, GST_WFD_EINVAL);
  g_return_val_if_fail(DTS != NULL, GST_WFD_EINVAL);

  if (msg->av_format_change_timing) {
    *PTS = msg->av_format_change_timing->PTS;
    *DTS = msg->av_format_change_timing->DTS;
  }

  return GST_WFD_OK;
}
