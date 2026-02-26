#pragma once

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/method_result.h>
#include <flutter/texture_registrar.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera_texture.h"
#include "record_handler.h"

using Microsoft::WRL::ComPtr;

enum class CameraState {
  kCreated,
  kInitializing,
  kRunning,
  kPaused,
  kDisposing,
  kDisposed,
};

struct CameraConfig {
  std::wstring symbolic_link;
  int resolution_preset = 4;  // max
  bool enable_audio = false;
  int target_fps = 30;
};

class Camera {
 public:
  Camera(int camera_id, flutter::TextureRegistrar* texture_registrar,
         flutter::MethodChannel<flutter::EncodableValue>* channel,
         CameraConfig config);
  ~Camera();

  int64_t RegisterTexture();

  void Initialize(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void TakePicture(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void StartVideoRecording(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void StopVideoRecording(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void StartImageStream();
  void StopImageStream();
  void PausePreview();
  void ResumePreview();
  void Dispose();

 private:
  static std::pair<int, int> ResolutionForPreset(int preset);

  bool BuildSourceReader(std::string* error);
  void CaptureLoop();

  // P7a: horizontal mirror only (BGRA → mirrored BGRA).
  // Applied before photo / recorder copies so all outputs are mirrored.
  static void FlipHorizontal(uint8_t* data, int width, int height);

  // P7b: R↔B channel swap in-place (mirrored BGRA → mirrored RGBA).
  // Applied after photo / recorder copies, for the Flutter texture only.
  static void SwapRBChannels(uint8_t* data, int width, int height);

  // P6: image-stream delivery thread.
  void ImageStreamLoop();

  void PostImageStreamFrame(const uint8_t* data, int width, int height);
  void SendError(const std::string& description);

  int camera_id_;
  int64_t texture_id_ = -1;
  CameraConfig config_;

  flutter::TextureRegistrar* texture_registrar_;
  flutter::MethodChannel<flutter::EncodableValue>* channel_;
  std::unique_ptr<CameraTexture> texture_;

  ComPtr<IMFSourceReader> reader_;
  std::unique_ptr<RecordHandler> record_handler_;

  // ── Capture thread ───────────────────────────────────────────────────────
  std::thread        capture_thread_;
  std::atomic<bool>  running_{false};
  std::atomic<bool>  preview_paused_{false};
  std::atomic<bool>  image_streaming_{false};

  // ── Latest raw frame for photo capture (mirrored BGRA) ──────────────────
  std::vector<uint8_t> latest_frame_;
  std::mutex           latest_frame_mutex_;

  // ── Persistent per-frame pixel buffer (P7) ───────────────────────────────
  std::vector<uint8_t> packed_frame_;

  // ── Actual camera dimensions (written by capture thread) ─────────────────
  int        actual_width_  = 0;
  int        actual_height_ = 0;
  mutable std::mutex dims_mutex_;

  // ── Camera state (all accesses hold state_mutex_) ────────────────────────
  CameraState        state_ = CameraState::kCreated;
  mutable std::mutex state_mutex_;

  // ── Initialization result + timeout thread ───────────────────────────────
  std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> pending_init_;
  std::mutex   pending_init_mutex_;

  std::atomic<bool>       first_frame_received_{false};
  std::thread             init_timeout_thread_;
  std::mutex              init_timeout_cancel_mutex_;
  std::condition_variable init_timeout_cancel_cv_;
  bool                    init_timeout_cancelled_ = false;

  // ── Image stream delivery (P6) ───────────────────────────────────────────
  struct ImageStreamSlot {
    std::vector<uint8_t> data;
    int  width  = 0;
    int  height = 0;
    bool dirty  = false;
  };

  std::mutex              image_stream_mutex_;
  std::condition_variable image_stream_cv_;
  ImageStreamSlot         image_stream_slot_;
  std::thread             image_stream_thread_;
  std::atomic<bool>       image_stream_running_{false};
};
