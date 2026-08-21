/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

 /*
 * gst-ai-stream-concurrency
 *
 * QIM SDK concurrent multi-stream AI sample app.
 *
 * Runs N concurrent decode + object-detection streams from the same MP4 file.
 * Each stream is fully independent (decode -> tee -> YOLOX inference on NPU ->
 * metadata mux -> overlay) and only the final annotated frames are composed
 * into a grid by a single qtivcomposer for display.
 *
 * Per-stream topology (Topology A -- qtimetamux + qtivoverlay):
 *
 *   filesrc -> qtdemux -> queue -> h264parse -> v4l2h264dec -> NV12 -> queue -> tee
 *     tee [passthrough] -> queue -> qtimetamux
 *     tee [AI] -> queue -> qtimlvconverter -> queue -> qtimltflite -> queue ->
 *                 qtimlpostprocess -> text/x-raw -> queue -> qtimetamux
 *   qtimetamux -> queue -> qtivoverlay -> queue -> qtivcomposer.sink_<i>
 *
 *   qtivcomposer -> queue -> waylandsink
 *
 * Usage:
 *   gst-ai-stream-concurrency -n 4 -i <INPUT_FILE> -m <MODEL_PATH> -l <LABELS_PATH>
 */

#include <stdio.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <glib.h>

#include <gst/sampleapps/gst_sample_apps_utils.h>

/* Maximum number of concurrent streams this app will build. */
#define MAX_STREAMS 24

/* Composed output canvas the grid is laid out within. */
#define CANVAS_WIDTH  1920
#define CANVAS_HEIGHT 1080

/* Per-stream queue slots. */
enum
{
  Q_DEMUX = 0,                  /* qtdemux dynamic pad -> h264parse        */
  Q_PRE_TEE,                    /* NV12 caps -> tee                        */
  Q_PASSTHROUGH,                /* tee -> qtimetamux                       */
  Q_AI_IN,                      /* tee -> qtimlvconverter                  */
  Q_AI_INFER,                   /* qtimlvconverter -> qtimltflite          */
  Q_AI_POST,                    /* qtimltflite -> qtimlpostprocess         */
  Q_AI_META,                    /* qtimlpostprocess -> qtimetamux          */
  Q_OVERLAY_IN,                 /* qtimetamux -> qtivoverlay               */
  Q_OVERLAY_OUT,                /* qtivoverlay -> qtivcomposer             */
  QUEUE_COUNT
};

/* Carries the per-stream link target for the qtdemux pad-added callback. */
typedef struct
{
  GstElement *queue;            /* the Q_DEMUX queue for this stream */
  gint stream_index;
} PadAddedData;

/*
 * Must outlive create_pipe() -- the pad-added callback dereferences this for
 * the whole life of the pipeline, so it cannot live on create_pipe's stack.
 */
static PadAddedData pad_data[MAX_STREAMS];

/* Command line options. */
static gint opt_num_streams = 4;
static gchar *opt_input_file = NULL;
static gchar *opt_model_path = NULL;
static gchar *opt_labels_path = NULL;
static gdouble opt_confidence = 51.0;

static GOptionEntry entries[] = {
  { "num-streams", 'n', 0, G_OPTION_ARG_INT, &opt_num_streams,
      "Number of concurrent streams to run (1.." G_STRINGIFY (MAX_STREAMS) ")",
      "N" },
  { "input", 'i', 0, G_OPTION_ARG_FILENAME, &opt_input_file,
      "Input MP4 file, decoded once per stream", "FILE" },
  { "model", 'm', 0, G_OPTION_ARG_FILENAME, &opt_model_path,
      "TFLite detection model", "FILE" },
  { "labels", 'l', 0, G_OPTION_ARG_FILENAME, &opt_labels_path,
      "Labels file for the detection model", "FILE" },
  { "confidence", 'c', 0, G_OPTION_ARG_DOUBLE, &opt_confidence,
      "Detection confidence threshold (default 51.0)", "VALUE" },
  { NULL, 0, 0, 0, NULL, NULL, NULL }
};

/*
 * Create an element with an indexed instance name. Every element instance in
 * the pipeline must have a unique name, otherwise linking becomes
 * unpredictable once several streams use the same factory.
 */
static GstElement *
make_indexed (const gchar * factory, const gchar * name_fmt, gint index)
{
  GstElement *element = NULL;
  gchar name[64];

  g_snprintf (name, sizeof (name), name_fmt, index);

  element = gst_element_factory_make (factory, name);
  if (!element)
    g_printerr ("Failed to create element %s (%s)\n", name, factory);

  return element;
}

/*
 * Compute a grid that fits n cells: the smallest square-ish layout, filled
 * left-to-right, top-to-bottom. Integer-only so the app does not need libm.
 */
static void
compute_grid (gint n, gint * cols, gint * rows)
{
  gint c = 1;

  while (c * c < n)
    c++;

  *cols = c;
  *rows = (n + c - 1) / c;
}

/* Set position and dimensions on a qtivcomposer sink pad (GValue arrays). */
static void
set_composer_pad (GstElement * composer, const gchar * pad_name,
    gint x, gint y, gint w, gint h)
{
  GstPad *pad = NULL;
  GValue position = G_VALUE_INIT;
  GValue dimension = G_VALUE_INIT;
  GValue val = G_VALUE_INIT;

  pad = gst_element_get_static_pad (composer, pad_name);
  if (!pad) {
    g_printerr ("Failed to get composer pad %s\n", pad_name);
    return;
  }

  g_value_init (&position, GST_TYPE_ARRAY);
  g_value_init (&dimension, GST_TYPE_ARRAY);
  g_value_init (&val, G_TYPE_INT);

  g_value_set_int (&val, x);
  gst_value_array_append_value (&position, &val);
  g_value_set_int (&val, y);
  gst_value_array_append_value (&position, &val);

  g_value_set_int (&val, w);
  gst_value_array_append_value (&dimension, &val);
  g_value_set_int (&val, h);
  gst_value_array_append_value (&dimension, &val);

  g_object_set_property (G_OBJECT (pad), "position", &position);
  g_object_set_property (G_OBJECT (pad), "dimensions", &dimension);

  g_value_unset (&position);
  g_value_unset (&dimension);
  g_value_unset (&val);
  gst_object_unref (pad);
}

/*
 * qtdemux emits pad-added per contained track. Each stream has its own demuxer
 * and its own target queue, so the callback receives the per-stream data.
 *
 * An MP4 holding both video and audio fires this twice; the second (audio)
 * attempt finds the video queue sink already linked and is skipped. That is
 * expected, not an error.
 */
static void
on_pad_added_multi (GstElement * element, GstPad * srcpad, gpointer userdata)
{
  PadAddedData *data = (PadAddedData *) userdata;
  GstPad *sinkpad = NULL;

  sinkpad = gst_element_get_static_pad (data->queue, "sink");
  if (!sinkpad) {
    g_printerr ("Stream %d: failed to get queue sink pad\n",
        data->stream_index);
    return;
  }

  if (gst_pad_is_linked (sinkpad)) {
    gst_object_unref (sinkpad);
    return;
  }

  if (GST_PAD_LINK_FAILED (gst_pad_link (srcpad, sinkpad)))
    g_printerr ("Stream %d: failed to link dynamic pad\n", data->stream_index);

  gst_object_unref (sinkpad);
}

static gboolean
create_pipe (GstAppContext * appctx, gint num_streams,
    const gchar * input_file, const gchar * model_path,
    const gchar * labels_path, gdouble confidence)
{
  GstElement *filesrc[MAX_STREAMS] = { NULL };
  GstElement *qtdemux[MAX_STREAMS] = { NULL };
  GstElement *h264parse[MAX_STREAMS] = { NULL };
  GstElement *v4l2h264dec[MAX_STREAMS] = { NULL };
  GstElement *nv12_caps[MAX_STREAMS] = { NULL };
  GstElement *tee[MAX_STREAMS] = { NULL };
  GstElement *mlvconverter[MAX_STREAMS] = { NULL };
  GstElement *mltflite[MAX_STREAMS] = { NULL };
  GstElement *mlpostprocess[MAX_STREAMS] = { NULL };
  GstElement *metamux[MAX_STREAMS] = { NULL };
  GstElement *overlay[MAX_STREAMS] = { NULL };
  GstElement *queue[MAX_STREAMS][QUEUE_COUNT] = { { NULL } };
  GstElement *composer = NULL;
  GstElement *out_queue = NULL;
  GstElement *waylandsink = NULL;
  GstCaps *caps = NULL;
  GstStructure *delegate_options = NULL;
  gchar settings_str[64];
  gchar pad_name[32];
  gint module_id = -1;
  gint cols = 1, rows = 1;
  gint cell_w = 0, cell_h = 0;
  gint i = 0, j = 0;

  compute_grid (num_streams, &cols, &rows);
  cell_w = CANVAS_WIDTH / cols;
  cell_h = CANVAS_HEIGHT / rows;

  g_print ("Building %d concurrent stream(s) in a %dx%d grid "
      "(%dx%d per cell)\n", num_streams, cols, rows, cell_w, cell_h);

  g_snprintf (settings_str, sizeof (settings_str), "{\"confidence\": %.1f}",
      confidence);

  /* Step 1: shared output stage -- one composer, one display sink. */
  composer = gst_element_factory_make ("qtivcomposer", "composer");
  if (!composer) {
    g_printerr ("Failed to create qtivcomposer\n");
    goto cleanup;
  }

  out_queue = gst_element_factory_make ("queue", "composer_out_queue");
  if (!out_queue) {
    g_printerr ("Failed to create composer output queue\n");
    goto cleanup;
  }

  waylandsink = gst_element_factory_make ("waylandsink", "display");
  if (!waylandsink) {
    g_printerr ("Failed to create waylandsink\n");
    goto cleanup;
  }

  g_object_set (G_OBJECT (waylandsink), "sync", TRUE, "fullscreen", TRUE, NULL);

  /* Step 2: create every per-stream element. */
  for (i = 0; i < num_streams; i++) {
    filesrc[i] = make_indexed ("filesrc", "file_src_%d", i);
    if (!filesrc[i])
      goto cleanup;

    qtdemux[i] = make_indexed ("qtdemux", "demux_%d", i);
    if (!qtdemux[i])
      goto cleanup;

    h264parse[i] = make_indexed ("h264parse", "h264_parse_%d", i);
    if (!h264parse[i])
      goto cleanup;

    v4l2h264dec[i] = make_indexed ("v4l2h264dec", "h264_dec_%d", i);
    if (!v4l2h264dec[i])
      goto cleanup;

    nv12_caps[i] = make_indexed ("capsfilter", "nv12_caps_%d", i);
    if (!nv12_caps[i])
      goto cleanup;

    tee[i] = make_indexed ("tee", "stream_tee_%d", i);
    if (!tee[i])
      goto cleanup;

    mlvconverter[i] = make_indexed ("qtimlvconverter", "preproc_%d", i);
    if (!mlvconverter[i])
      goto cleanup;

    mltflite[i] = make_indexed ("qtimltflite", "inference_%d", i);
    if (!mltflite[i])
      goto cleanup;

    mlpostprocess[i] = make_indexed ("qtimlpostprocess", "postproc_%d", i);
    if (!mlpostprocess[i])
      goto cleanup;

    metamux[i] = make_indexed ("qtimetamux", "meta_mux_%d", i);
    if (!metamux[i])
      goto cleanup;

    overlay[i] = make_indexed ("qtivoverlay", "overlay_%d", i);
    if (!overlay[i])
      goto cleanup;

    for (j = 0; j < QUEUE_COUNT; j++) {
      gchar qname[64];

      g_snprintf (qname, sizeof (qname), "queue_%d_%d", i, j);
      queue[i][j] = gst_element_factory_make ("queue", qname);
      if (!queue[i][j]) {
        g_printerr ("Failed to create %s\n", qname);
        goto cleanup;
      }
    }
  }

  /* Step 3: set properties on the per-stream elements. */
  for (i = 0; i < num_streams; i++) {
    g_object_set (G_OBJECT (filesrc[i]), "location", input_file, NULL);

    /* Zero-copy DMA decode -- enum properties, set by string nick. */
    gst_element_set_enum_property (v4l2h264dec[i], "capture-io-mode", "dmabuf");
    gst_element_set_enum_property (v4l2h264dec[i], "output-io-mode", "dmabuf");

    caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "NV12", NULL);
    g_object_set (G_OBJECT (nv12_caps[i]), "caps", caps, NULL);
    gst_caps_unref (caps);
    caps = NULL;

    /* YOLOX detection on the NPU via the TFLite external (QNN) delegate. */
    delegate_options = gst_structure_from_string (
        "QNNExternalDelegate,backend_type=htp;", NULL);
    if (!delegate_options) {
      g_printerr ("Failed to build external delegate options\n");
      goto cleanup;
    }

    g_object_set (G_OBJECT (mltflite[i]),
        "model", model_path,
        "delegate", GST_ML_TFLITE_DELEGATE_EXTERNAL, NULL);
    g_object_set (G_OBJECT (mltflite[i]),
        "external_delegate_path", "libQnnTFLiteDelegate.so",
        "external_delegate_options", delegate_options, NULL);
    gst_structure_free (delegate_options);
    delegate_options = NULL;

    /* module is an enum -- resolve the nick, never hardcode the integer. */
    module_id = get_enum_value (mlpostprocess[i], "module", "yolov8");
    if (module_id < 0) {
      g_printerr ("qtimlpostprocess module 'yolov8' not found\n");
      goto cleanup;
    }

    g_object_set (G_OBJECT (mlpostprocess[i]),
        "module", module_id,
        "labels", labels_path,
        "settings", settings_str, NULL);
  }

  /* Step 4: add everything to the pipeline. */
  gst_bin_add_many (GST_BIN (appctx->pipeline),
      composer, out_queue, waylandsink, NULL);

  for (i = 0; i < num_streams; i++) {
    gst_bin_add_many (GST_BIN (appctx->pipeline),
        filesrc[i], qtdemux[i], h264parse[i], v4l2h264dec[i], nv12_caps[i],
        tee[i], mlvconverter[i], mltflite[i], mlpostprocess[i], metamux[i],
        overlay[i], NULL);

    for (j = 0; j < QUEUE_COUNT; j++)
      gst_bin_add_many (GST_BIN (appctx->pipeline), queue[i][j], NULL);
  }

  /* Step 5: link the shared output stage. */
  if (!gst_element_link_many (composer, out_queue, waylandsink, NULL)) {
    g_printerr ("Failed to link composer to display sink\n");
    goto cleanup_pipeline;
  }

  /* Step 6: link each stream. */
  for (i = 0; i < num_streams; i++) {
    /* filesrc -> qtdemux; the demuxer's src pad is dynamic. */
    if (!gst_element_link (filesrc[i], qtdemux[i])) {
      g_printerr ("Stream %d: failed to link filesrc to qtdemux\n", i);
      goto cleanup_pipeline;
    }

    /* queue -> h264parse -> decoder -> NV12 -> queue -> tee */
    if (!gst_element_link_many (queue[i][Q_DEMUX], h264parse[i],
            v4l2h264dec[i], nv12_caps[i], queue[i][Q_PRE_TEE], tee[i], NULL)) {
      g_printerr ("Stream %d: failed to link decode chain\n", i);
      goto cleanup_pipeline;
    }

    /* Passthrough branch: tee -> queue -> qtimetamux */
    if (!gst_element_link_many (tee[i], queue[i][Q_PASSTHROUGH], metamux[i],
            NULL)) {
      g_printerr ("Stream %d: failed to link passthrough branch\n", i);
      goto cleanup_pipeline;
    }

    /* AI branch: tee -> queue -> preproc -> queue -> infer -> queue -> post */
    if (!gst_element_link_many (tee[i], queue[i][Q_AI_IN], mlvconverter[i],
            queue[i][Q_AI_INFER], mltflite[i], queue[i][Q_AI_POST],
            mlpostprocess[i], NULL)) {
      g_printerr ("Stream %d: failed to link AI branch\n", i);
      goto cleanup_pipeline;
    }

    /* Detection metadata needs the explicit text/x-raw caps into the mux. */
    caps = gst_caps_from_string ("text/x-raw");
    if (!gst_element_link_filtered (mlpostprocess[i], queue[i][Q_AI_META],
            caps)) {
      g_printerr ("Stream %d: failed to link postprocess metadata\n", i);
      gst_caps_unref (caps);
      caps = NULL;
      goto cleanup_pipeline;
    }
    gst_caps_unref (caps);
    caps = NULL;

    if (!gst_element_link (queue[i][Q_AI_META], metamux[i])) {
      g_printerr ("Stream %d: failed to link metadata into qtimetamux\n", i);
      goto cleanup_pipeline;
    }

    /* qtimetamux -> queue -> qtivoverlay -> queue -> composer.sink_<i> */
    if (!gst_element_link_many (metamux[i], queue[i][Q_OVERLAY_IN], overlay[i],
            queue[i][Q_OVERLAY_OUT], NULL)) {
      g_printerr ("Stream %d: failed to link overlay chain\n", i);
      goto cleanup_pipeline;
    }

    /* Linking in stream order makes composer pads sink_0..sink_(N-1). */
    if (!gst_element_link (queue[i][Q_OVERLAY_OUT], composer)) {
      g_printerr ("Stream %d: failed to link overlay into composer\n", i);
      goto cleanup_pipeline;
    }

    /* Step 7: dynamic pad handling for this stream's demuxer. */
    pad_data[i].queue = queue[i][Q_DEMUX];
    pad_data[i].stream_index = i;
    g_signal_connect (qtdemux[i], "pad-added",
        G_CALLBACK (on_pad_added_multi), &pad_data[i]);
  }

  /* Step 8: lay the grid out. Pads exist only after the links above. */
  for (i = 0; i < num_streams; i++) {
    g_snprintf (pad_name, sizeof (pad_name), "sink_%d", i);
    set_composer_pad (composer, pad_name,
        (i % cols) * cell_w, (i / cols) * cell_h, cell_w, cell_h);
  }

  return TRUE;

cleanup_pipeline:
  if (caps)
    gst_caps_unref (caps);
  if (delegate_options)
    gst_structure_free (delegate_options);
  gst_object_unref (appctx->pipeline);
  appctx->pipeline = NULL;
  return FALSE;

cleanup:
  /* Nothing has been added to the pipeline yet -- unref what was created. */
  if (caps)
    gst_caps_unref (caps);
  if (delegate_options)
    gst_structure_free (delegate_options);
  if (composer)
    gst_object_unref (composer);
  if (out_queue)
    gst_object_unref (out_queue);
  if (waylandsink)
    gst_object_unref (waylandsink);

  for (i = 0; i < MAX_STREAMS; i++) {
    if (filesrc[i])
      gst_object_unref (filesrc[i]);
    if (qtdemux[i])
      gst_object_unref (qtdemux[i]);
    if (h264parse[i])
      gst_object_unref (h264parse[i]);
    if (v4l2h264dec[i])
      gst_object_unref (v4l2h264dec[i]);
    if (nv12_caps[i])
      gst_object_unref (nv12_caps[i]);
    if (tee[i])
      gst_object_unref (tee[i]);
    if (mlvconverter[i])
      gst_object_unref (mlvconverter[i]);
    if (mltflite[i])
      gst_object_unref (mltflite[i]);
    if (mlpostprocess[i])
      gst_object_unref (mlpostprocess[i]);
    if (metamux[i])
      gst_object_unref (metamux[i]);
    if (overlay[i])
      gst_object_unref (overlay[i]);

    for (j = 0; j < QUEUE_COUNT; j++) {
      if (queue[i][j])
        gst_object_unref (queue[i][j]);
    }
  }

  return FALSE;
}

int
main (int argc, char *argv[])
{
  GstAppContext appctx = {};
  GOptionContext *optctx = NULL;
  GError *error = NULL;
  GstBus *bus = NULL;
  guint intrpt_watch_id = 0;
  gint ret = 0;

  /* Parse command line before touching GStreamer state. */
  optctx = g_option_context_new ("- QIM SDK concurrent multi-stream AI app");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_add_group (optctx, gst_init_get_option_group ());

  if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
    g_printerr ("Failed to parse options: %s\n", error->message);
    g_clear_error (&error);
    g_option_context_free (optctx);
    return -1;
  }
  g_option_context_free (optctx);

  if (opt_num_streams < 1 || opt_num_streams > MAX_STREAMS) {
    g_printerr ("Invalid stream count %d -- must be between 1 and %d\n",
        opt_num_streams, MAX_STREAMS);
    return -1;
  }

  if (!opt_input_file || !opt_model_path || !opt_labels_path) {
    g_printerr ("Missing required option.\n"
        "Usage: %s -n <NUM_STREAMS> -i <INPUT_FILE> -m <MODEL_PATH> "
        "-l <LABELS_PATH> [-c <CONFIDENCE>]\n", argv[0]);
    return -1;
  }

  gst_init (&argc, &argv);

  appctx.pipeline = gst_pipeline_new ("stream-ai-concurrency");
  if (!appctx.pipeline) {
    g_printerr ("Failed to create pipeline\n");
    ret = -1;
    goto done;
  }

  appctx.mloop = g_main_loop_new (NULL, FALSE);
  if (!appctx.mloop) {
    g_printerr ("Failed to create main loop\n");
    ret = -1;
    goto done;
  }

  if (!create_pipe (&appctx, opt_num_streams, opt_input_file, opt_model_path,
          opt_labels_path, opt_confidence)) {
    g_printerr ("Failed to build pipeline\n");
    ret = -1;
    goto done;
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (appctx.pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message::error", G_CALLBACK (error_cb), appctx.mloop);
  g_signal_connect (bus, "message::warning", G_CALLBACK (warning_cb),
      appctx.mloop);
  g_signal_connect (bus, "message::eos", G_CALLBACK (eos_cb), appctx.mloop);
  g_signal_connect (bus, "message::state-changed",
      G_CALLBACK (state_changed_cb), appctx.pipeline);
  gst_object_unref (bus);

  intrpt_watch_id = g_unix_signal_add (SIGINT, handle_interrupt_signal, &appctx);

  switch (gst_element_set_state (appctx.pipeline, GST_STATE_PAUSED)) {
    case GST_STATE_CHANGE_FAILURE:
      g_printerr ("Failed to set pipeline to PAUSED\n");
      ret = -1;
      goto done;
    case GST_STATE_CHANGE_NO_PREROLL:
      gst_element_set_state (appctx.pipeline, GST_STATE_PLAYING);
      break;
    case GST_STATE_CHANGE_ASYNC:
    case GST_STATE_CHANGE_SUCCESS:
      break;
  }

  g_main_loop_run (appctx.mloop);

done:
  if (intrpt_watch_id)
    g_source_remove (intrpt_watch_id);

  if (appctx.pipeline) {
    gst_element_set_state (appctx.pipeline, GST_STATE_NULL);
    gst_object_unref (appctx.pipeline);
  }
  if (appctx.mloop)
    g_main_loop_unref (appctx.mloop);

  g_free (opt_input_file);
  g_free (opt_model_path);
  g_free (opt_labels_path);

  gst_deinit ();
  return ret;
}
