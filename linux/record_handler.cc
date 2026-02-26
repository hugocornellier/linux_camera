#include "record_handler.h"

#include <cstdio>

// Video encoder candidates in order of preference.
static const char* kEncoderCandidates[] = {
    "x264enc",
    "vah264enc",
    "vaapih264enc",
    "openh264enc",
};
static const int kNumEncoderCandidates = 4;

// Audio encoder candidates in order of preference.
static const char* kAudioEncoderCandidates[] = {
    "opusenc",
    "avenc_aac",
    "voaacenc",
    "lamemp3enc",
};
static const int kNumAudioEncoderCandidates = 4;

RecordHandler::RecordHandler()
    : pipeline_(nullptr),
      tee_(nullptr),
      queue_(nullptr),
      valve_(nullptr),
      videoconvert_(nullptr),
      encoder_(nullptr),
      muxer_(nullptr),
      filesink_(nullptr),
      audio_source_(nullptr),
      audio_convert_(nullptr),
      audio_resample_(nullptr),
      audio_encoder_(nullptr),
      audio_queue_(nullptr),
      audio_valve_(nullptr),
      is_recording_(false),
      is_setup_(false),
      has_audio_(false),
      pending_stop_call_(nullptr) {}

RecordHandler::~RecordHandler() {
  if (pending_stop_call_) {
    g_object_unref(pending_stop_call_);
    pending_stop_call_ = nullptr;
  }
}

std::string RecordHandler::DetectEncoder() {
  for (int i = 0; i < kNumEncoderCandidates; i++) {
    GstElementFactory* factory =
        gst_element_factory_find(kEncoderCandidates[i]);
    if (factory) {
      gst_object_unref(factory);
      return kEncoderCandidates[i];
    }
  }
  return "";
}

std::string RecordHandler::DetectAudioEncoder() {
  for (int i = 0; i < kNumAudioEncoderCandidates; i++) {
    GstElementFactory* factory =
        gst_element_factory_find(kAudioEncoderCandidates[i]);
    if (factory) {
      gst_object_unref(factory);
      return kAudioEncoderCandidates[i];
    }
  }
  return "";
}

bool RecordHandler::Setup(GstElement* pipeline, GstElement* tee,
                          int width, int height, int fps, bool enable_audio,
                          GError** error) {
  if (is_setup_) return true;

  encoder_name_ = DetectEncoder();
  if (encoder_name_.empty()) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "No H.264 encoder available. Install gstreamer1.0-plugins-ugly "
                "(x264enc) or gstreamer1.0-vaapi (vaapih264enc).");
    return false;
  }

  pipeline_ = pipeline;
  tee_ = tee;

  // Create video recording branch elements.
  queue_ = gst_element_factory_make("queue", "rec_queue");
  valve_ = gst_element_factory_make("valve", "rec_valve");
  videoconvert_ = gst_element_factory_make("videoconvert", "rec_convert");
  encoder_ = gst_element_factory_make(encoder_name_.c_str(), "rec_encoder");
  muxer_ = gst_element_factory_make("matroskamux", "rec_mux");
  filesink_ = gst_element_factory_make("filesink", "rec_filesink");

  if (!queue_ || !valve_ || !videoconvert_ || !encoder_ ||
      !muxer_ || !filesink_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to create recording pipeline elements");
    return false;
  }

  // Configure the valve to start closed (dropping all data).
  g_object_set(valve_, "drop", TRUE, nullptr);

  // Configure the queue for recording.
  g_object_set(queue_, "max-size-buffers", 100, "max-size-time", (guint64)0,
               "max-size-bytes", 0, nullptr);

  // Configure encoder settings based on type.
  if (encoder_name_ == "x264enc") {
    g_object_set(encoder_, "tune", 4 /* zerolatency */, "speed-preset", 2
                 /* superfast */, "bitrate", 4000, nullptr);
  } else if (encoder_name_ == "openh264enc") {
    g_object_set(encoder_, "bitrate", 4000000, nullptr);
  }

  // Add all video elements to the pipeline.
  gst_bin_add_many(GST_BIN(pipeline_), queue_, valve_,
                   videoconvert_, encoder_, muxer_, filesink_, nullptr);

  // Link: queue → valve → videoconvert → encoder → muxer → filesink
  if (!gst_element_link_many(queue_, valve_, videoconvert_,
                             encoder_, muxer_, filesink_, nullptr)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to link recording pipeline elements");
    return false;
  }

  // Link tee to the recording queue.
  GstPad* tee_pad = gst_element_request_pad_simple(tee_, "src_%u");
  GstPad* queue_pad = gst_element_get_static_pad(queue_, "sink");
  GstPadLinkReturn link_ret = gst_pad_link(tee_pad, queue_pad);
  gst_object_unref(queue_pad);
  gst_object_unref(tee_pad);

  if (link_ret != GST_PAD_LINK_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to link tee to recording branch");
    return false;
  }

  // Sync video element states with the pipeline.
  gst_element_sync_state_with_parent(queue_);
  gst_element_sync_state_with_parent(valve_);
  gst_element_sync_state_with_parent(videoconvert_);
  gst_element_sync_state_with_parent(encoder_);
  gst_element_sync_state_with_parent(muxer_);
  gst_element_sync_state_with_parent(filesink_);

  // Set up audio branch if requested.
  if (enable_audio) {
    GError* audio_error = nullptr;
    if (SetupAudioBranch(&audio_error)) {
      has_audio_ = true;
    } else {
      // Audio setup failed — log warning but continue without audio.
      g_warning("Audio setup failed: %s. Recording without audio.",
                audio_error ? audio_error->message : "unknown error");
      if (audio_error) g_error_free(audio_error);
      has_audio_ = false;
    }
  }

  is_setup_ = true;
  return true;
}

bool RecordHandler::SetupAudioBranch(GError** error) {
  audio_encoder_name_ = DetectAudioEncoder();
  if (audio_encoder_name_.empty()) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "No audio encoder available");
    return false;
  }

  audio_source_ = gst_element_factory_make("autoaudiosrc", "rec_audio_src");
  audio_convert_ = gst_element_factory_make("audioconvert", "rec_audio_conv");
  audio_resample_ =
      gst_element_factory_make("audioresample", "rec_audio_resample");
  audio_encoder_ = gst_element_factory_make(audio_encoder_name_.c_str(),
                                             "rec_audio_enc");
  audio_queue_ = gst_element_factory_make("queue", "rec_audio_queue");
  audio_valve_ = gst_element_factory_make("valve", "rec_audio_valve");

  if (!audio_source_ || !audio_convert_ || !audio_resample_ ||
      !audio_encoder_ || !audio_queue_ || !audio_valve_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to create audio pipeline elements");
    return false;
  }

  // Start with audio valve closed.
  g_object_set(audio_valve_, "drop", TRUE, nullptr);

  // Add audio elements to pipeline.
  gst_bin_add_many(GST_BIN(pipeline_), audio_source_, audio_queue_,
                   audio_valve_, audio_convert_, audio_resample_,
                   audio_encoder_, nullptr);

  // Link: autoaudiosrc → queue → valve → audioconvert → audioresample →
  // encoder
  if (!gst_element_link_many(audio_source_, audio_queue_, audio_valve_,
                             audio_convert_, audio_resample_, audio_encoder_,
                             nullptr)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to link audio pipeline elements");
    return false;
  }

  // Link audio encoder to the muxer.
  GstPad* audio_src = gst_element_get_static_pad(audio_encoder_, "src");
  GstPad* mux_audio_sink =
      gst_element_request_pad_simple(muxer_, "audio_%u");
  if (!audio_src || !mux_audio_sink) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to get audio pads for muxer");
    if (audio_src) gst_object_unref(audio_src);
    if (mux_audio_sink) gst_object_unref(mux_audio_sink);
    return false;
  }

  GstPadLinkReturn ret = gst_pad_link(audio_src, mux_audio_sink);
  gst_object_unref(audio_src);
  gst_object_unref(mux_audio_sink);

  if (ret != GST_PAD_LINK_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to link audio encoder to muxer");
    return false;
  }

  // Sync audio element states.
  gst_element_sync_state_with_parent(audio_source_);
  gst_element_sync_state_with_parent(audio_queue_);
  gst_element_sync_state_with_parent(audio_valve_);
  gst_element_sync_state_with_parent(audio_convert_);
  gst_element_sync_state_with_parent(audio_resample_);
  gst_element_sync_state_with_parent(audio_encoder_);

  return true;
}

bool RecordHandler::StartRecording(const std::string& output_path,
                                   GError** error) {
  if (is_recording_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Recording is already in progress");
    return false;
  }

  if (!is_setup_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Recording pipeline not set up");
    return false;
  }

  output_path_ = output_path;

  // Reset the muxer and filesink states to accept new data.
  gst_element_set_state(muxer_, GST_STATE_NULL);
  gst_element_set_state(filesink_, GST_STATE_NULL);
  g_object_set(filesink_, "location", output_path.c_str(), nullptr);
  gst_element_sync_state_with_parent(muxer_);
  gst_element_sync_state_with_parent(filesink_);

  // Open the video valve to let data flow.
  g_object_set(valve_, "drop", FALSE, nullptr);

  // Open the audio valve if audio is enabled.
  if (has_audio_ && audio_valve_) {
    g_object_set(audio_valve_, "drop", FALSE, nullptr);
  }

  is_recording_ = true;
  return true;
}

struct StopRecordingData {
  RecordHandler* handler;
  FlMethodCall* method_call;
  std::string output_path;
};

GstPadProbeReturn RecordHandler::OnEosEvent(GstPad* pad,
                                            GstPadProbeInfo* info,
                                            gpointer user_data) {
  if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS) {
    return GST_PAD_PROBE_PASS;
  }

  StopRecordingData* data = static_cast<StopRecordingData*>(user_data);

  // Respond on the main thread.
  g_idle_add(
      [](gpointer user_data) -> gboolean {
        StopRecordingData* data = static_cast<StopRecordingData*>(user_data);

        g_autoptr(FlValue) result =
            fl_value_new_string(data->output_path.c_str());
        fl_method_call_respond_success(data->method_call, result, nullptr);
        g_object_unref(data->method_call);

        data->handler->is_recording_ = false;
        delete data;
        return G_SOURCE_REMOVE;
      },
      data);

  return GST_PAD_PROBE_REMOVE;
}

void RecordHandler::StopRecording(FlMethodCall* method_call) {
  if (!is_recording_) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "not_recording",
                                 "No recording in progress", details, nullptr);
    return;
  }

  // Close the video valve to stop new data flowing.
  g_object_set(valve_, "drop", TRUE, nullptr);

  // Close the audio valve if audio is enabled.
  if (has_audio_ && audio_valve_) {
    g_object_set(audio_valve_, "drop", TRUE, nullptr);
  }

  // Set up an EOS probe on the filesink's sink pad to know when the file is
  // done.
  GstPad* filesink_pad = gst_element_get_static_pad(filesink_, "sink");

  StopRecordingData* data = new StopRecordingData();
  data->handler = this;
  data->method_call = FL_METHOD_CALL(g_object_ref(method_call));
  data->output_path = output_path_;

  if (filesink_pad) {
    gst_pad_add_probe(filesink_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      RecordHandler::OnEosEvent, data, nullptr);
    gst_object_unref(filesink_pad);
  }

  // Send EOS to the encoder's sink pad to flush the recording branch.
  GstPad* encoder_sink = gst_element_get_static_pad(encoder_, "sink");
  if (encoder_sink) {
    gst_pad_send_event(encoder_sink, gst_event_new_eos());
    gst_object_unref(encoder_sink);
  }

  // Also send EOS on the audio branch if present.
  if (has_audio_ && audio_encoder_) {
    GstPad* audio_enc_sink =
        gst_element_get_static_pad(audio_encoder_, "sink");
    if (audio_enc_sink) {
      gst_pad_send_event(audio_enc_sink, gst_event_new_eos());
      gst_object_unref(audio_enc_sink);
    }
  }

  // If we couldn't set up the probe, respond immediately.
  if (!filesink_pad) {
    g_autoptr(FlValue) result = fl_value_new_string(output_path_.c_str());
    fl_method_call_respond_success(method_call, result, nullptr);
    g_object_unref(data->method_call);
    delete data;
    is_recording_ = false;
  }
}
