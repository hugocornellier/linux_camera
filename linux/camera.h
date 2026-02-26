#ifndef CAMERA_H_
#define CAMERA_H_

#include <flutter_linux/flutter_linux.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <memory>
#include <string>

#include "camera_texture.h"
#include "device_enumerator.h"
#include "record_handler.h"

enum class CameraState {
  kCreated,
  kInitializing,
  kRunning,
  kPaused,
  kDisposing,
  kDisposed,
};

struct CameraConfig {
  std::string device_path;
  int resolution_preset;
  bool enable_audio;
  int target_width;
  int target_height;
  int target_fps;
};

class Camera {
 public:
  Camera(int camera_id,
         FlTextureRegistrar* texture_registrar,
         FlMethodChannel* method_channel,
         const CameraConfig& config);
  ~Camera();

  int camera_id() const { return camera_id_; }
  int64_t texture_id() const { return texture_id_; }
  CameraState state() const { return state_; }

  // Allocates the texture and registers it. Must be called before Initialize.
  // Returns the texture_id on success, -1 on failure.
  int64_t RegisterTexture();

  // Builds and starts the GStreamer pipeline. Responds to |method_call|
  // asynchronously once the first frame arrives or an error/timeout occurs.
  void Initialize(FlMethodCall* method_call);

  // Captures a still image and saves it to a temporary JPEG file.
  // Responds to |method_call| with the file path or an error.
  void TakePicture(FlMethodCall* method_call);

  // Pauses/resumes the live preview.
  void PausePreview();
  void ResumePreview();

  // Starts video recording (silent — no audio).
  void StartVideoRecording(FlMethodCall* method_call);

  // Stops video recording and returns the file path.
  void StopVideoRecording(FlMethodCall* method_call);

  // Starts/stops sending raw frame data to Dart via method channel.
  void StartImageStream();
  void StopImageStream();

  // Tears down the pipeline and releases all resources.
  void Dispose();

 private:
  bool BuildPipeline(GError** error);
  void RespondToPendingInit(bool success, const char* error_message);

  // GStreamer callbacks (static with user_data = Camera*).
  static GstFlowReturn OnNewSample(GstAppSink* sink, gpointer user_data);
  static gboolean OnBusMessage(GstBus* bus, GstMessage* msg,
                               gpointer user_data);
  static gboolean OnInitTimeout(gpointer user_data);

  // Sends an error event to Dart via the method channel.
  void SendError(const std::string& description);

  int camera_id_;
  int64_t texture_id_;
  CameraState state_;
  CameraConfig config_;

  FlTextureRegistrar* texture_registrar_;  // Not owned.
  FlMethodChannel* method_channel_;        // Not owned.
  CameraTexture* texture_;                 // Owned (GObject ref).

  GstElement* pipeline_;
  GstElement* tee_;       // For branching preview + recording.
  GstElement* appsink_;
  guint bus_watch_id_;
  guint init_timeout_id_;

  std::unique_ptr<RecordHandler> record_handler_;

  // Pending async initialization — stores the FlMethodCall until first frame.
  FlMethodCall* pending_init_call_;
  bool first_frame_received_;
  bool preview_paused_;
  std::atomic<bool> image_streaming_;

  int actual_width_;
  int actual_height_;
};

#endif  // CAMERA_H_
