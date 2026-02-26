#include "camera.h"
#include "photo_handler.h"

#include <gst/video/video.h>

#include <cstdio>
#include <cstring>

static const guint kInitTimeoutMs = 8000;

Camera::Camera(int camera_id,
               FlTextureRegistrar* texture_registrar,
               FlMethodChannel* method_channel,
               const CameraConfig& config)
    : camera_id_(camera_id),
      texture_id_(-1),
      state_(CameraState::kCreated),
      config_(config),
      texture_registrar_(texture_registrar),
      method_channel_(method_channel),
      texture_(nullptr),
      pipeline_(nullptr),
      tee_(nullptr),
      appsink_(nullptr),
      bus_watch_id_(0),
      init_timeout_id_(0),
      record_handler_(std::make_unique<RecordHandler>()),
      pending_init_call_(nullptr),
      first_frame_received_(false),
      preview_paused_(false),
      image_streaming_(false),
      actual_width_(0),
      actual_height_(0) {}

Camera::~Camera() {
  Dispose();
}

int64_t Camera::RegisterTexture() {
  texture_ = camera_texture_new();
  FlTexture* fl_tex = camera_texture_as_fl_texture(texture_);
  if (!fl_texture_registrar_register_texture(texture_registrar_, fl_tex)) {
    g_object_unref(texture_);
    texture_ = nullptr;
    return -1;
  }
  texture_id_ = fl_texture_get_id(fl_tex);
  return texture_id_;
}

void Camera::Initialize(FlMethodCall* method_call) {
  if (state_ != CameraState::kCreated) {
    g_autoptr(FlValue) error_details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "already_initialized",
                                 "Camera is already initialized or disposed",
                                 error_details, nullptr);
    return;
  }

  state_ = CameraState::kInitializing;
  pending_init_call_ = FL_METHOD_CALL(g_object_ref(method_call));
  first_frame_received_ = false;

  GError* error = nullptr;
  if (!BuildPipeline(&error)) {
    RespondToPendingInit(false, error->message);
    g_error_free(error);
    state_ = CameraState::kCreated;
    return;
  }

  // Set pipeline to PLAYING.
  GstStateChangeReturn ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    RespondToPendingInit(false, "Failed to start GStreamer pipeline");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    appsink_ = nullptr;
    state_ = CameraState::kCreated;
    return;
  }

  // Set a timeout for initialization — if no frame arrives in time, fail.
  init_timeout_id_ =
      g_timeout_add(kInitTimeoutMs, Camera::OnInitTimeout, this);
}

bool Camera::BuildPipeline(GError** error) {
  // Build pipeline with a tee to support branching for recording:
  //   v4l2src ! videoconvert ! caps ! tee name=t
  //     t. ! queue ! appsink (preview)
  //     t. ! [recording branch, added later by RecordHandler]
  gchar* pipeline_str = g_strdup_printf(
      "v4l2src device=%s "
      "! videoconvert "
      "! videoflip method=horizontal-flip "
      "! video/x-raw,format=RGBA,width=%d,height=%d "
      "! tee name=t "
      "t. ! queue name=preview_queue ! "
      "appsink name=sink emit-signals=true max-buffers=2 drop=true "
      "sync=false",
      config_.device_path.c_str(), config_.target_width,
      config_.target_height);

  pipeline_ = gst_parse_launch(pipeline_str, error);
  g_free(pipeline_str);

  if (!pipeline_) {
    return false;
  }

  // Get the tee element (needed for recording branch attachment).
  tee_ = gst_bin_get_by_name(GST_BIN(pipeline_), "t");
  if (!tee_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to find tee in pipeline");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return false;
  }
  // Release our ref (pipeline holds one).
  gst_object_unref(tee_);

  // Get the appsink element.
  appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
  if (!appsink_) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to find appsink in pipeline");
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    return false;
  }

  // Connect the new-sample signal.
  GstAppSinkCallbacks callbacks = {};
  callbacks.new_sample = Camera::OnNewSample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this,
                             nullptr);

  // Set up bus watch for error messages.
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  bus_watch_id_ = gst_bus_add_watch(bus, Camera::OnBusMessage, this);
  gst_object_unref(bus);

  // Release our ref on the appsink (pipeline holds one).
  gst_object_unref(appsink_);

  return true;
}

void Camera::RespondToPendingInit(bool success, const char* error_message) {
  if (!pending_init_call_) return;

  // Cancel the timeout.
  if (init_timeout_id_ > 0) {
    g_source_remove(init_timeout_id_);
    init_timeout_id_ = 0;
  }

  if (success) {
    g_autoptr(FlValue) result = fl_value_new_map();
    fl_value_set_string_take(result, "previewWidth",
                             fl_value_new_float((double)actual_width_));
    fl_value_set_string_take(result, "previewHeight",
                             fl_value_new_float((double)actual_height_));
    fl_method_call_respond_success(pending_init_call_, result, nullptr);
  } else {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(pending_init_call_, "initialization_failed",
                                 error_message ? error_message : "Unknown error",
                                 details, nullptr);
  }

  g_object_unref(pending_init_call_);
  pending_init_call_ = nullptr;
}

GstFlowReturn Camera::OnNewSample(GstAppSink* sink, gpointer user_data) {
  Camera* self = static_cast<Camera*>(user_data);

  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_ERROR;

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  int width = GST_VIDEO_INFO_WIDTH(&info);
  int height = GST_VIDEO_INFO_HEIGHT(&info);
  int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  // Handle first-frame initialization response.
  bool is_first_frame = !self->first_frame_received_;
  if (is_first_frame) {
    self->first_frame_received_ = true;
    self->actual_width_ = width;
    self->actual_height_ = height;
    self->state_ = CameraState::kRunning;
  }

  // Update the texture only if preview is not paused (or if this is the first
  // frame, which we need for initialization).
  if (!self->preview_paused_ || is_first_frame) {
    if (stride == width * 4) {
      // No padding — direct copy.
      camera_texture_update(self->texture_, map.data, width, height);
    } else {
      // Stride has padding — copy row-by-row, stripping padding.
      size_t tight_size = (size_t)width * height * 4;
      uint8_t* tight = (uint8_t*)g_malloc(tight_size);
      for (int row = 0; row < height; row++) {
        memcpy(tight + row * width * 4, map.data + row * stride, width * 4);
      }
      camera_texture_update(self->texture_, tight, width, height);
      g_free(tight);
    }

    // Notify Flutter that a new frame is available.
    fl_texture_registrar_mark_texture_frame_available(
        self->texture_registrar_,
        camera_texture_as_fl_texture(self->texture_));
  }

  // Send frame to Dart image stream if streaming is active.
  if (self->image_streaming_) {
    size_t frame_size = (size_t)width * height * 4;
    uint8_t* frame_copy = (uint8_t*)g_malloc(frame_size);
    if (stride == width * 4) {
      memcpy(frame_copy, map.data, frame_size);
    } else {
      for (int row = 0; row < height; row++) {
        memcpy(frame_copy + row * width * 4, map.data + row * stride,
               width * 4);
      }
    }

    struct ImageStreamData {
      FlMethodChannel* channel;
      int camera_id;
      uint8_t* pixels;
      int width;
      int height;
      size_t size;
    };

    auto* stream_data = new ImageStreamData();
    stream_data->channel = self->method_channel_;
    stream_data->camera_id = self->camera_id_;
    stream_data->pixels = frame_copy;
    stream_data->width = width;
    stream_data->height = height;
    stream_data->size = frame_size;

    g_idle_add(
        [](gpointer user_data) -> gboolean {
          auto* data = static_cast<ImageStreamData*>(user_data);

          g_autoptr(FlValue) args = fl_value_new_map();
          fl_value_set_string_take(args, "cameraId",
                                   fl_value_new_int(data->camera_id));
          fl_value_set_string_take(args, "width",
                                   fl_value_new_int(data->width));
          fl_value_set_string_take(args, "height",
                                   fl_value_new_int(data->height));
          fl_value_set_string_take(
              args, "bytes",
              fl_value_new_uint8_list(data->pixels, data->size));

          fl_method_channel_invoke_method(data->channel, "imageStreamFrame",
                                          args, nullptr, nullptr, nullptr);

          g_free(data->pixels);
          delete data;
          return G_SOURCE_REMOVE;
        },
        stream_data);
  }

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);

  // Dispatch init response to the main thread (OnNewSample runs on the
  // GStreamer streaming thread, but fl_method_call_respond_* must be called
  // from the main GLib thread).
  if (is_first_frame) {
    g_idle_add(
        [](gpointer user_data) -> gboolean {
          Camera* cam = static_cast<Camera*>(user_data);
          cam->RespondToPendingInit(true, nullptr);
          return G_SOURCE_REMOVE;
        },
        self);
  }

  return GST_FLOW_OK;
}

gboolean Camera::OnBusMessage(GstBus* bus, GstMessage* msg,
                              gpointer user_data) {
  Camera* self = static_cast<Camera*>(user_data);

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &err, &debug);

      if (self->state_ == CameraState::kInitializing) {
        // Initialization failed — respond to pending call.
        self->RespondToPendingInit(false, err->message);
        self->state_ = CameraState::kCreated;
      } else if (self->state_ == CameraState::kRunning ||
                 self->state_ == CameraState::kPaused) {
        // Runtime error — send event to Dart.
        self->SendError(err->message);
      }

      g_error_free(err);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_EOS: {
      // End of stream (e.g., device unplugged).
      if (self->state_ == CameraState::kRunning ||
          self->state_ == CameraState::kPaused) {
        self->SendError("Camera stream ended unexpectedly");
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}

gboolean Camera::OnInitTimeout(gpointer user_data) {
  Camera* self = static_cast<Camera*>(user_data);
  self->init_timeout_id_ = 0;

  if (self->state_ == CameraState::kInitializing) {
    self->RespondToPendingInit(
        false, "Camera initialization timed out — no frames received");
    // Stop the pipeline.
    if (self->pipeline_) {
      gst_element_set_state(self->pipeline_, GST_STATE_NULL);
    }
    self->state_ = CameraState::kCreated;
  }
  return G_SOURCE_REMOVE;
}

void Camera::SendError(const std::string& description) {
  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, "cameraId",
                           fl_value_new_int(camera_id_));
  fl_value_set_string_take(args, "description",
                           fl_value_new_string(description.c_str()));
  fl_method_channel_invoke_method(method_channel_, "cameraError", args,
                                  nullptr, nullptr, nullptr);
}

void Camera::TakePicture(FlMethodCall* method_call) {
  if (state_ != CameraState::kRunning && state_ != CameraState::kPaused) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "not_running",
                                 "Camera is not running", details, nullptr);
    return;
  }

  // Generate a unique temporary file path.
  gchar* tmp_path =
      g_strdup_printf("%s/camera_desktop_%d_%ld.jpg", g_get_tmp_dir(),
                      camera_id_, g_get_real_time());

  GError* error = nullptr;
  if (PhotoHandler::TakePicture(appsink_, tmp_path, &error)) {
    g_autoptr(FlValue) result = fl_value_new_string(tmp_path);
    fl_method_call_respond_success(method_call, result, nullptr);
  } else {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(
        method_call, "capture_failed",
        error ? error->message : "Failed to capture image", details, nullptr);
    if (error) g_error_free(error);
  }

  g_free(tmp_path);
}

void Camera::StartVideoRecording(FlMethodCall* method_call) {
  if (state_ != CameraState::kRunning && state_ != CameraState::kPaused) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "not_running",
                                 "Camera is not running", details, nullptr);
    return;
  }

  // Set up the recording branch on first use.
  if (!record_handler_->is_recording()) {
    GError* error = nullptr;
    if (!record_handler_->Setup(pipeline_, tee_, actual_width_,
                                actual_height_, config_.target_fps,
                                config_.enable_audio, &error)) {
      g_autoptr(FlValue) details = fl_value_new_null();
      fl_method_call_respond_error(
          method_call, "recording_setup_failed",
          error ? error->message : "Failed to set up recording", details,
          nullptr);
      if (error) g_error_free(error);
      return;
    }
  }

  // Generate output path.
  gchar* tmp_path =
      g_strdup_printf("%s/camera_desktop_%d_%ld.mp4", g_get_tmp_dir(),
                      camera_id_, g_get_real_time());

  GError* error = nullptr;
  if (!record_handler_->StartRecording(tmp_path, &error)) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(
        method_call, "recording_start_failed",
        error ? error->message : "Failed to start recording", details,
        nullptr);
    if (error) g_error_free(error);
    g_free(tmp_path);
    return;
  }

  g_free(tmp_path);
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

void Camera::StopVideoRecording(FlMethodCall* method_call) {
  if (!record_handler_->is_recording()) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "not_recording",
                                 "No recording in progress", details, nullptr);
    return;
  }

  // StopRecording responds asynchronously when the file is finalized.
  record_handler_->StopRecording(method_call);
}

void Camera::StartImageStream() {
  image_streaming_ = true;
}

void Camera::StopImageStream() {
  image_streaming_ = false;
}

void Camera::PausePreview() {
  preview_paused_ = true;
}

void Camera::ResumePreview() {
  preview_paused_ = false;
}

void Camera::Dispose() {
  if (state_ == CameraState::kDisposed || state_ == CameraState::kDisposing) {
    return;
  }
  state_ = CameraState::kDisposing;

  // Cancel pending init if still waiting.
  if (pending_init_call_) {
    RespondToPendingInit(false, "Camera disposed during initialization");
  }

  // Stop the pipeline.
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_watch_id_ > 0) {
      g_source_remove(bus_watch_id_);
      bus_watch_id_ = 0;
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    appsink_ = nullptr;
  }

  // Unregister the texture.
  if (texture_ && texture_registrar_) {
    fl_texture_registrar_unregister_texture(
        texture_registrar_, camera_texture_as_fl_texture(texture_));
    g_object_unref(texture_);
    texture_ = nullptr;
  }

  // Send closing event to Dart.
  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, "cameraId",
                           fl_value_new_int(camera_id_));
  fl_method_channel_invoke_method(method_channel_, "cameraClosing", args,
                                  nullptr, nullptr, nullptr);

  state_ = CameraState::kDisposed;
}
