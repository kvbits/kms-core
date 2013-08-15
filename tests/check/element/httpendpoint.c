#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <glib.h>

#define WAIT_TIMEOUT 3
//#define LOCATION "http://ci.kurento.com/downloads/sintel_trailer-480p.webm"
#define LOCATION "http://ci.kurento.com/downloads/small.webm"

static GMainLoop *loop = NULL;
GstElement *src_pipeline, *souphttpsrc, *appsink;
GstElement *test_pipeline, *httpep, *fakesink;

static void
bus_msg_cb (GstBus * bus, GstMessage * msg, gpointer pipeline)
{
  switch (msg->type) {
    case GST_MESSAGE_ERROR:{
      GST_ERROR ("%s bus error: %P", GST_ELEMENT_NAME (pipeline), msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
          GST_DEBUG_GRAPH_SHOW_ALL, "bus_error");
      fail ("Error received on %s bus", GST_ELEMENT_NAME (pipeline));
      break;
    }
    case GST_MESSAGE_WARNING:{
      GST_WARNING ("%s bus: %P", GST_ELEMENT_NAME (pipeline), msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline),
          GST_DEBUG_GRAPH_SHOW_ALL, "warning");
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:{
      GST_TRACE ("%s bus event: %P", GST_ELEMENT_NAME (pipeline), msg);
      break;
    }
    default:
      break;
  }
}

static void
recv_sample (GstElement * appsink, gpointer user_data)
{
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;

  g_signal_emit_by_name (appsink, "pull-sample", &sample);
  if (sample == NULL) {
    GST_ERROR ("No sample received");
    return;
  }

  buffer = gst_sample_get_buffer (sample);
  if (buffer == NULL) {
    GST_ERROR ("No buffer received");
    return;
  }

  g_signal_emit_by_name (httpep, "push-buffer", buffer, &ret);

  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send buffer to httpep %s. Ret code %d", ret,
        GST_ELEMENT_NAME (httpep));
    fail ("Can not send buffer to", GST_ELEMENT_NAME (httpep));
  }
}

static gboolean
timer_cb (gpointer data)
{
  /* Agnostic bin might be now ready to go to Playing state */
  GST_INFO ("Connecting appsink to receive buffers");
  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (test_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "timer_dot");

  fakesink = gst_element_factory_make ("fakesink", NULL);

  gst_bin_add (GST_BIN (test_pipeline), fakesink);

  gst_element_set_state (fakesink, GST_STATE_PLAYING);
  gst_element_set_state (httpep, GST_STATE_PLAYING);

  gst_element_link_pads (httpep, "video_src_%u", fakesink, "sink");

  /* Start getting data from Internet */
  gst_element_set_state (src_pipeline, GST_STATE_PLAYING);

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (src_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "src_getting_data");

  return FALSE;
}

static void
appsink_eos_cb (GstElement * appsink, gpointer user_data)
{
  GstFlowReturn ret;

  GST_INFO ("EOS received on %s. Preparing %s to finish the test",
      GST_ELEMENT_NAME (appsink), GST_ELEMENT_NAME (test_pipeline));

  g_signal_emit_by_name (httpep, "end-of-stream", &ret);

  if (ret != GST_FLOW_OK) {
    // something wrong
    GST_ERROR ("Could not send EOS to %s. Ret code %d", ret,
        GST_ELEMENT_NAME (httpep));
    fail ("Can not send buffer to", GST_ELEMENT_NAME (httpep));
  }
}

static void
http_eos_cb (GstElement * appsink, gpointer user_data)
{
  GST_INFO ("EOS received on %s element. Stopping main loop",
      GST_ELEMENT_NAME (httpep));
  g_main_loop_quit (loop);
}

GST_START_TEST (check_push_buffer)
{
  guint bus_watch_id1, bus_watch_id2;
  GstBus *srcbus, *testbus;

  GST_INFO ("Running test check_push_buffer");

  loop = g_main_loop_new (NULL, FALSE);

  /* Create source pipeline */
  src_pipeline = gst_pipeline_new ("src-pipeline");
  souphttpsrc = gst_element_factory_make ("souphttpsrc", NULL);
  appsink = gst_element_factory_make ("appsink", NULL);

  srcbus = gst_pipeline_get_bus (GST_PIPELINE (src_pipeline));

  bus_watch_id1 = gst_bus_add_watch (srcbus, gst_bus_async_signal_func, NULL);
  g_signal_connect (srcbus, "message", G_CALLBACK (bus_msg_cb), src_pipeline);
  g_object_unref (srcbus);

  gst_bin_add_many (GST_BIN (src_pipeline), souphttpsrc, appsink, NULL);
  gst_element_link (souphttpsrc, appsink);

  /* configure objects */
  g_object_set (G_OBJECT (souphttpsrc), "location", LOCATION,
      "is-live", TRUE, "do-timestamp", TRUE, NULL);

  g_object_set (appsink, "emit-signals", TRUE, NULL);
  g_signal_connect (appsink, "new-sample", G_CALLBACK (recv_sample), NULL);
  g_signal_connect (appsink, "eos", G_CALLBACK (appsink_eos_cb), NULL);

  /* Create test pipeline */
  test_pipeline = gst_pipeline_new ("test-pipeline");
  httpep = gst_element_factory_make ("httpendpoint", NULL);

  testbus = gst_pipeline_get_bus (GST_PIPELINE (test_pipeline));

  bus_watch_id2 = gst_bus_add_watch (testbus, gst_bus_async_signal_func, NULL);
  g_signal_connect (testbus, "message", G_CALLBACK (bus_msg_cb), test_pipeline);
  g_object_unref (testbus);

  gst_bin_add (GST_BIN (test_pipeline), httpep);
  g_signal_connect (G_OBJECT (httpep), "eos", G_CALLBACK (http_eos_cb), NULL);

  /* Set pipeline to start state */
  gst_element_set_state (test_pipeline, GST_STATE_PLAYING);

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (test_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "test_entering_main_loop");

  mark_point ();

  g_timeout_add_seconds (WAIT_TIMEOUT, timer_cb, NULL);
  GST_INFO ("Waitig %d second for Agnosticbin to be ready to go to "
      "PLAYING state", WAIT_TIMEOUT);

  g_main_loop_run (loop);

  mark_point ();

  GST_DEBUG ("Main loop stopped");

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (src_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "src_after_main_loop");

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (test_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "test_after_main_loop");

  gst_element_set_state (src_pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (src_pipeline));

  gst_element_set_state (test_pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (test_pipeline));

  g_source_remove (bus_watch_id1);
  g_source_remove (bus_watch_id2);
  g_main_loop_unref (loop);
}

GST_END_TEST static gboolean
quit_main_loop (gpointer user_data)
{
  GST_DEBUG ("Stopping main loop");
  g_main_loop_quit (loop);
  return FALSE;
}

GST_START_TEST (check_pull_buffer)
{
  GstElement *videotestsrc, *timeoverlay;
  guint bus_watch_id1;
  GstBus *srcbus;

  GST_INFO ("Running test check_pull_buffer");

  loop = g_main_loop_new (NULL, FALSE);

  GST_DEBUG ("Preparing source pipeline");

  /* Create gstreamer elements */
  src_pipeline = gst_pipeline_new ("src-pipeline");
  videotestsrc = gst_element_factory_make ("videotestsrc", NULL);
  timeoverlay = gst_element_factory_make ("timeoverlay", NULL);
  httpep = gst_element_factory_make ("httpendpoint", NULL);

  GST_DEBUG ("Adding watcher to the pipeline");
  srcbus = gst_pipeline_get_bus (GST_PIPELINE (src_pipeline));

  bus_watch_id1 = gst_bus_add_watch (srcbus, gst_bus_async_signal_func, NULL);
  g_signal_connect (srcbus, "message", G_CALLBACK (bus_msg_cb), src_pipeline);
  g_object_unref (srcbus);

  GST_DEBUG ("Configuring source pipeline");
  gst_bin_add_many (GST_BIN (src_pipeline), videotestsrc, timeoverlay,
      httpep, NULL);
  gst_element_link (videotestsrc, timeoverlay);
  gst_element_link_pads (timeoverlay, "src", httpep, "video_sink");

  GST_DEBUG ("Configuring elements");
  g_object_set (G_OBJECT (videotestsrc), "is-live", TRUE, "do-timestamp", TRUE,
      "pattern", 18, "num-buffers", 150, NULL);
  g_object_set (G_OBJECT (timeoverlay), "font-desc", "Sans 28", NULL);

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (src_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "entering_main_loop");

  GST_DEBUG ("Starting pipeline");
  gst_element_set_state (src_pipeline, GST_STATE_PLAYING);

  g_timeout_add_seconds (3, quit_main_loop, NULL);

  g_main_loop_run (loop);

  GST_DEBUG ("Main loop stopped");

  GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (src_pipeline),
      GST_DEBUG_GRAPH_SHOW_ALL, "after_main_loop");

  gst_element_set_state (src_pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (src_pipeline));

  GST_DEBUG ("Bus message received");

  g_source_remove (bus_watch_id1);
  g_main_loop_unref (loop);
}

GST_END_TEST
/******************************/
/* HttpEndPoint test suit */
/******************************/
static Suite *
httpendpoint_suite (void)
{
  Suite *s = suite_create ("httpendpoint");
  TCase *tc_chain = tcase_create ("element");

  suite_add_tcase (s, tc_chain);

  /* Simulates GET behaviour */
  tcase_add_test (tc_chain, check_pull_buffer);

  /* Simulates POST behaviour */
  tcase_add_test (tc_chain, check_push_buffer);

  return s;
}

GST_CHECK_MAIN (httpendpoint);