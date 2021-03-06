/*
 * (C) Copyright 2015 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include "../../src/gst-plugins/commons/kmselementpadtype.h"

#define KMS_VIDEO_PREFIX "video_src_"
#define KMS_AUDIO_PREFIX "audio_src_"

#define AUDIO_SINK "audio-sink"
#define VIDEO_SINK "video-sink"

static gboolean
quit_main_loop_idle (gpointer data)
{
  GMainLoop *loop = data;

  g_main_loop_quit (loop);
  return FALSE;
}

static void
bus_msg (GstBus * bus, GstMessage * msg, gpointer pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      gchar *error_file = g_strdup_printf ("error-%s", GST_OBJECT_NAME (pipe));

      GST_ERROR ("Error: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, error_file);
      g_free (error_file);
      fail ("Error received on bus");
      break;
    }
    case GST_MESSAGE_WARNING:{
      gchar *warn_file = g_strdup_printf ("warning-%s", GST_OBJECT_NAME (pipe));

      GST_WARNING ("Warning: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, warn_file);
      g_free (warn_file);
      break;
    }
    default:
      break;
  }
}

static void
fakesink_hand_off (GstElement * fakesink, GstBuffer * buf, GstPad * pad,
    gpointer data)
{
  static int count = 0;
  GMainLoop *loop = (GMainLoop *) data;

  if (count++ > 40) {
    g_object_set (G_OBJECT (fakesink), "signal-handoffs", FALSE, NULL);
    g_idle_add (quit_main_loop_idle, loop);
  }
}

static gboolean
timeout_check (gpointer pipeline)
{
  if (GST_IS_BIN (pipeline)) {
    gchar *timeout_file =
        g_strdup_printf ("timeout-%s", GST_OBJECT_NAME (pipeline));

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
        GST_DEBUG_GRAPH_SHOW_ALL, timeout_file);
    g_free (timeout_file);
  }

  return FALSE;
}

typedef struct _KmsConnectData
{
  GstElement *src;
  GstBin *pipe;
  const gchar *pad_prefix;
  gulong id;
} KmsConnectData;

static void
connect_sink (GstElement * element, GstPad * pad, gpointer user_data)
{
  KmsConnectData *data = user_data;

  GST_DEBUG_OBJECT (pad, "New pad %" GST_PTR_FORMAT, element);

  if (!g_str_has_prefix (GST_OBJECT_NAME (pad), data->pad_prefix)) {
    return;
  }

  gst_bin_add (GST_BIN (data->pipe), data->src);

  gst_element_link_pads (data->src, NULL, element, GST_OBJECT_NAME (pad));
  gst_element_sync_state_with_parent (data->src);

  GST_INFO_OBJECT (pad, "Linking %s", data->pad_prefix);

  if (data->id != 0) {
    g_signal_handler_disconnect (element, data->id);
  }
}

static void
kms_connect_data_destroy (gpointer data)
{
  g_slice_free (KmsConnectData, data);
}

static void
connect_sink_async (GstElement * passthrough, GstElement * src,
    GstElement * pipe, const gchar * pad_prefix)
{
  GstPad *pad = gst_element_get_static_pad (passthrough, pad_prefix);

  if (pad) {
    gst_bin_add (GST_BIN (pipe), src);

    gst_element_link_pads (src, NULL, passthrough, pad_prefix);
    gst_element_sync_state_with_parent (src);
    g_object_unref (pad);
  } else {
    KmsConnectData *data = g_slice_new (KmsConnectData);

    data->src = src;
    data->pipe = GST_BIN (pipe);
    data->pad_prefix = pad_prefix;

    data->id =
        g_signal_connect_data (passthrough, "pad-added",
        G_CALLBACK (connect_sink), data,
        (GClosureNotify) kms_connect_data_destroy, 0);
  }
}

static void
connect_sink_on_srcpad_added (GstElement * element, GstPad * pad,
    gpointer user_data)
{
  GstElement *sink;
  GstPad *sinkpad;

  if (g_str_has_prefix (GST_PAD_NAME (pad), KMS_AUDIO_PREFIX)) {
    GST_DEBUG_OBJECT (pad, "Connecting video stream");
    sink = g_object_get_data (G_OBJECT (element), AUDIO_SINK);
  } else if (g_str_has_prefix (GST_PAD_NAME (pad), KMS_VIDEO_PREFIX)) {
    GST_DEBUG_OBJECT (pad, "Connecting audio stream");
    sink = g_object_get_data (G_OBJECT (element), VIDEO_SINK);
  } else {
    GST_TRACE_OBJECT (pad, "Not src pad type");
    return;
  }

  sinkpad = gst_element_get_static_pad (sink, "sink");
  gst_pad_link (pad, sinkpad);
  g_object_unref (sinkpad);
  gst_element_sync_state_with_parent (sink);
}

static gboolean
kms_element_request_srcpad (GstElement * src, KmsElementPadType pad_type)
{
  gchar *padname;

  g_signal_emit_by_name (src, "request-new-srcpad", pad_type, NULL, &padname);
  if (padname == NULL) {
    return FALSE;
  }

  GST_DEBUG_OBJECT (src, "Requested pad %s", padname);
  g_free (padname);

  return TRUE;
}

GST_START_TEST (check_connecion)
{
  GMainLoop *loop = g_main_loop_new (NULL, TRUE);
  GstElement *pipeline = gst_pipeline_new (__FUNCTION__);
  GstElement *videotestsrc = gst_element_factory_make ("videotestsrc", NULL);
  GstElement *fakesink = gst_element_factory_make ("fakesink", NULL);
  GstElement *passthrough = gst_element_factory_make ("passthrough", NULL);

  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

  g_object_set (G_OBJECT (pipeline), "async-handling", TRUE, NULL);
  g_object_set (G_OBJECT (videotestsrc), "is-live", TRUE, NULL);

  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_msg), pipeline);

  g_object_set (G_OBJECT (fakesink), "sync", TRUE, "signal-handoffs", TRUE,
      "async", FALSE, NULL);
  g_signal_connect (G_OBJECT (fakesink), "handoff",
      G_CALLBACK (fakesink_hand_off), loop);

  mark_point ();
  gst_bin_add_many (GST_BIN (pipeline), passthrough, fakesink, NULL);
  mark_point ();
  connect_sink_async (passthrough, videotestsrc, pipeline, "sink_video");
  g_object_set_data (G_OBJECT (passthrough), VIDEO_SINK, fakesink);
  g_signal_connect (passthrough, "pad-added",
      G_CALLBACK (connect_sink_on_srcpad_added), NULL);
  fail_unless (kms_element_request_srcpad (passthrough,
          KMS_ELEMENT_PAD_TYPE_VIDEO));
  mark_point ();
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  mark_point ();
  g_timeout_add_seconds (10, timeout_check, pipeline);

  mark_point ();
  g_main_loop_run (loop);
  mark_point ();

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_bus_remove_signal_watch (bus);
  g_object_unref (bus);
  g_object_unref (pipeline);
  g_main_loop_unref (loop);
}

GST_END_TEST
/* Suite initialization */
static Suite *
passthrough_suite (void)
{
  Suite *s = suite_create ("filterelement");
  TCase *tc_chain = tcase_create ("element");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, check_connecion);

  return s;
}

GST_CHECK_MAIN (passthrough);
