#include "camera.h"

#include <flutter/standard_method_codec.h>
#include <mfobjects.h>
#include <objbase.h>
#include <strmif.h>
#include <windows.h>

#include <chrono>
#include <sstream>

#include "logging.h"
#include "photo_handler.h"

namespace {

constexpr bool kDebugDisableMirror = false;

std::string GuidToString(const GUID& guid) {
  LPOLESTR wide = nullptr;
  if (FAILED(StringFromCLSID(guid, &wide)) || !wide) return "<guid_error>";
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr,
                                 nullptr);
  std::string out;
  if (size > 0) {
    out.resize(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr,
                        nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
  }
  CoTaskMemFree(wide);
  return out;
}

void LogCurrentVideoType(IMFSourceReader* reader, const char* stage) {
  if (!reader) return;
  ComPtr<IMFMediaType> current_type;
  if (FAILED(reader->GetCurrentMediaType(
          (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type))) {
    DebugLog(std::string(stage) + ": GetCurrentMediaType failed");
    return;
  }

  GUID subtype = GUID_NULL;
  current_type->GetGUID(MF_MT_SUBTYPE, &subtype);
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
  UINT32 stride = 0;
  const bool has_stride =
      SUCCEEDED(current_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride));

  std::ostringstream ss;
  ss << stage << ": subtype=" << GuidToString(subtype) << " size=" << w << "x"
     << h;
  if (has_stride) ss << " defaultStride=" << static_cast<int32_t>(stride);
  DebugLog(ss.str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Camera::Camera(int camera_id, flutter::TextureRegistrar* texture_registrar,
               flutter::MethodChannel<flutter::EncodableValue>* channel,
               CameraConfig config)
    : camera_id_(camera_id),
      texture_registrar_(texture_registrar),
      channel_(channel),
      config_(std::move(config)),
      record_handler_(std::make_unique<RecordHandler>()) {}

Camera::~Camera() {
  Dispose();
}

// ---------------------------------------------------------------------------
// Resolution preset
// ---------------------------------------------------------------------------

std::pair<int, int> Camera::ResolutionForPreset(int preset) {
  switch (preset) {
    case 0: return {320, 240};
    case 1: return {480, 360};
    case 2: return {1280, 720};
    case 3: return {1280, 720};
    case 4: return {1920, 1080};
    default: return {1920, 1080};
  }
}

// ---------------------------------------------------------------------------
// Texture registration
// ---------------------------------------------------------------------------

int64_t Camera::RegisterTexture() {
  texture_ = std::make_unique<CameraTexture>(texture_registrar_);
  texture_id_ = texture_->Register();
  return texture_id_;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool Camera::BuildSourceReader(std::string* error) {
  ComPtr<IMFAttributes> activate_attrs;
  HRESULT hr = MFCreateAttributes(&activate_attrs, 2);
  if (FAILED(hr)) {
    if (error) *error = "MFCreateAttributes failed";
    return false;
  }

  hr = activate_attrs->SetGUID(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    if (error) *error = "SetGUID failed";
    return false;
  }

  hr = activate_attrs->SetString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
      config_.symbolic_link.c_str());
  if (FAILED(hr)) {
    if (error) *error = "SetString symlink failed";
    return false;
  }

  ComPtr<IMFMediaSource> source;
  hr = MFCreateDeviceSource(activate_attrs.Get(), &source);
  if (FAILED(hr)) {
    if (error) *error = "MFCreateDeviceSource failed";
    return false;
  }

  ComPtr<IMFAttributes> reader_attrs;
  hr = MFCreateAttributes(&reader_attrs, 1);
  if (FAILED(hr)) {
    if (error) *error = "MFCreateAttributes reader failed";
    return false;
  }

  hr = reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                               TRUE);
  if (FAILED(hr)) {
    if (error) *error = "SetUINT32 processing failed";
    return false;
  }

  hr = MFCreateSourceReaderFromMediaSource(source.Get(), reader_attrs.Get(),
                                           &reader_);
  if (FAILED(hr)) {
    if (error) *error = "MFCreateSourceReaderFromMediaSource failed";
    return false;
  }

  auto [target_w, target_h] = ResolutionForPreset(config_.resolution_preset);

  auto try_set_type = [&](int w, int h) -> HRESULT {
    ComPtr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return E_FAIL;
    if (FAILED(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return E_FAIL;
    if (FAILED(t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32))) return E_FAIL;
    if (w > 0 && FAILED(MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h)))
      return E_FAIL;
    return reader_->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
  };

  hr = try_set_type(target_w, target_h);
  if (FAILED(hr)) hr = try_set_type(0, 0);
  if (FAILED(hr)) {
    if (error) *error = "SetCurrentMediaType failed";
    return false;
  }

  LogCurrentVideoType(reader_.Get(), "BuildSourceReader selected type");
  return true;
}

void Camera::Initialize(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != CameraState::kCreated) {
      result->Error("already_initialized", "Camera is already initialized");
      return;
    }
    state_ = CameraState::kInitializing;
  }

  std::string build_error;
  if (!BuildSourceReader(&build_error)) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    state_ = CameraState::kCreated;
    result->Error("initialization_failed", build_error);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(pending_init_mutex_);
    pending_init_ = std::move(result);
  }

  running_ = true;
  capture_thread_ = std::thread(&Camera::CaptureLoop, this);

  // P2: init-timeout as a joinable thread with cancellation.
  init_timeout_cancelled_ = false;
  init_timeout_thread_ = std::thread([this]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      std::unique_lock<std::mutex> lk(init_timeout_cancel_mutex_);
      bool timed_out = !init_timeout_cancel_cv_.wait_for(
          lk, std::chrono::seconds(8),
          [this] { return init_timeout_cancelled_; });

      if (timed_out) {
        std::lock_guard<std::mutex> pending_lk(pending_init_mutex_);
        if (pending_init_ && !first_frame_received_.load()) {
          pending_init_->Error(
              "initialization_timeout",
              "Camera initialization timed out — no frames received");
          pending_init_.reset();
        }
      }
    }
    CoUninitialize();
  });
}

// ---------------------------------------------------------------------------
// Capture loop
// ---------------------------------------------------------------------------

void Camera::CaptureLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  static bool first_buffer_log = true;

  while (running_) {
    DWORD stream_index, flags;
    LONGLONG timestamp_100ns;
    ComPtr<IMFSample> sample;

    HRESULT hr = reader_->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, &stream_index, &flags, &timestamp_100ns, &sample);

    // P3: Handle ReadSample flags explicitly.
    if (FAILED(hr)) continue;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (flags & MF_SOURCE_READERF_ERROR) break;

    // Stream tick — no sample data, just a heartbeat from the driver.
    if (flags & MF_SOURCE_READERF_STREAMTICK) continue;

    // Media type changed — re-query dimensions; abort recording if active.
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      ComPtr<IMFMediaType> new_type;
      if (SUCCEEDED(reader_->GetCurrentMediaType(
              (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &new_type))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(new_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        {
          std::lock_guard<std::mutex> lk(dims_mutex_);
          actual_width_  = static_cast<int>(w);
          actual_height_ = static_cast<int>(h);
        }
        if (record_handler_->is_recording()) {
          record_handler_->Stop([](const std::wstring&) {});
          SendError(
              "Camera changed resolution during recording; recording stopped.");
        }
      }
      continue;
    }

    if (!sample) continue;

    // ── First frame: read actual dimensions ──────────────────────────────
    if (!first_frame_received_.load()) {
      ComPtr<IMFMediaType> current_type;
      if (SUCCEEDED(reader_->GetCurrentMediaType(
              (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        {
          std::lock_guard<std::mutex> lk(dims_mutex_);
          actual_width_  = static_cast<int>(w);
          actual_height_ = static_cast<int>(h);
        }
      }
    }

    int cur_w, cur_h;
    {
      std::lock_guard<std::mutex> lk(dims_mutex_);
      cur_w = actual_width_;
      cur_h = actual_height_;
    }

    if (cur_w <= 0 || cur_h <= 0) continue;

    // ── Convert to contiguous buffer ─────────────────────────────────────
    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) continue;

    if (first_buffer_log) {
      LogCurrentVideoType(reader_.Get(), "First frame current type");
      DWORD current_len = 0;
      if (SUCCEEDED(buffer->GetCurrentLength(&current_len))) {
        std::ostringstream ss;
        ss << "First buffer currentLen=" << current_len;
        DebugLog(ss.str());
      }
    }

    const size_t packed_len = static_cast<size_t>(cur_w) * cur_h * 4;

    // P7: reuse persistent buffer — allocate only on dimension change.
    if (packed_frame_.size() != packed_len) packed_frame_.resize(packed_len);

    bool copied = false;

    // Prefer Lock2D to honour source row pitch.
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2d)) &&
        SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
      if (first_buffer_log) {
        std::ostringstream ss;
        ss << "Lock2D succeeded: pitch=" << pitch
           << " expectedRowBytes=" << (cur_w * 4);
        DebugLog(ss.str());
      }
      const int row_bytes = cur_w * 4;
      for (int row = 0; row < cur_h; ++row) {
        const ptrdiff_t src_offset =
            static_cast<ptrdiff_t>((pitch < 0)
                                       ? (cur_h - 1 - row) * pitch
                                       : row * pitch);
        std::memcpy(packed_frame_.data() + static_cast<size_t>(row) * row_bytes,
                    scan0 + src_offset, static_cast<size_t>(row_bytes));
      }
      buffer2d->Unlock2D();
      copied = true;
    }

    if (!copied) {
      BYTE* raw = nullptr;
      DWORD raw_len = 0;
      hr = buffer->Lock(&raw, nullptr, &raw_len);
      if (FAILED(hr)) continue;
      if (first_buffer_log) {
        std::ostringstream ss;
        ss << "Flat lock: rawLen=" << raw_len
           << " expectedPacked=" << packed_len;
        DebugLog(ss.str());
      }
      if (raw_len >= packed_len) {
        std::memcpy(packed_frame_.data(), raw, packed_len);
        copied = true;
      }
      buffer->Unlock();
    }

    if (!copied) continue;
    first_buffer_log = false;

    BYTE* data = packed_frame_.data();
    const DWORD len = static_cast<DWORD>(packed_len);

    // ── P7a: horizontal flip in-place (BGRA → mirrored BGRA) ────────────
    // Must happen before photo + recorder copies so all three outputs
    // (photo, video, preview) are consistently mirrored.
    if (!kDebugDisableMirror) {
      FlipHorizontal(data, cur_w, cur_h);
    }

    // ── Latest frame for photo capture (mirrored BGRA) ────────────────────
    {
      std::lock_guard<std::mutex> lock(latest_frame_mutex_);
      latest_frame_.resize(len);
      std::memcpy(latest_frame_.data(), data, len);
    }

    // ── Feed video recorder (mirrored BGRA = MFVideoFormat_ARGB32) ────────
    if (record_handler_->is_recording()) {
      record_handler_->AppendFrame(data, cur_w, cur_h);
    }

    // ── P7b: R↔B swap (mirrored BGRA → mirrored RGBA for Flutter texture) ─
    if (!kDebugDisableMirror) {
      SwapRBChannels(data, cur_w, cur_h);
    }

    // ── Update preview texture ─────────────────────────────────────────────
    if (!preview_paused_.load() || !first_frame_received_.load()) {
      texture_->Update(data, cur_w, cur_h);
      texture_registrar_->MarkTextureFrameAvailable(texture_id_);
    }

    // ── Push to image stream (non-blocking slot update) ───────────────────
    if (image_streaming_.load()) {
      PostImageStreamFrame(data, cur_w, cur_h);
    }

    // ── Respond to pending init on first frame ────────────────────────────
    if (!first_frame_received_.load() && cur_w > 0) {
      first_frame_received_ = true;

      // P2: cancel the init-timeout thread early.
      {
        std::lock_guard<std::mutex> lk(init_timeout_cancel_mutex_);
        init_timeout_cancelled_ = true;
      }
      init_timeout_cancel_cv_.notify_one();

      // P4: update state under lock.
      {
        std::lock_guard<std::mutex> lk(state_mutex_);
        state_ = CameraState::kRunning;
      }

      std::lock_guard<std::mutex> lock(pending_init_mutex_);
      if (pending_init_) {
        int w, h;
        {
          std::lock_guard<std::mutex> dlk(dims_mutex_);
          w = actual_width_;
          h = actual_height_;
        }
        pending_init_->Success(
            flutter::EncodableValue(flutter::EncodableMap{
                {flutter::EncodableValue("previewWidth"),
                 flutter::EncodableValue(static_cast<double>(w))},
                {flutter::EncodableValue("previewHeight"),
                 flutter::EncodableValue(static_cast<double>(h))},
            }));
        pending_init_.reset();
      }
    }
  }

  CoUninitialize();
}

// ---------------------------------------------------------------------------
// P7a: Horizontal mirror — swaps pixels left↔right, preserves channel order.
// Input/output: BGRA.  Used for photo capture and video recording.
// ---------------------------------------------------------------------------

void Camera::FlipHorizontal(uint8_t* data, int width, int height) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = data + static_cast<size_t>(y) * width * 4;
    int l = 0, r = width - 1;
    while (l < r) {
      uint8_t* lp = row + l * 4;
      uint8_t* rp = row + r * 4;
      // Swap the two 4-byte pixels intact (no channel reorder).
      uint8_t tmp[4];
      std::memcpy(tmp, lp, 4);
      std::memcpy(lp,  rp, 4);
      std::memcpy(rp,  tmp, 4);
      ++l; --r;
    }
  }
}

// ---------------------------------------------------------------------------
// P7b: R↔B channel swap — converts mirrored BGRA to mirrored RGBA in-place.
// Applied only to the data fed to the Flutter texture.
// ---------------------------------------------------------------------------

void Camera::SwapRBChannels(uint8_t* data, int width, int height) {
  const size_t n = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < n; ++i) {
    std::swap(data[i * 4 + 0], data[i * 4 + 2]);  // B ↔ R
  }
}

// ---------------------------------------------------------------------------
// Photo capture
// ---------------------------------------------------------------------------

void Camera::TakePicture(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != CameraState::kRunning && state_ != CameraState::kPaused) {
      result->Error("not_running", "Camera is not running");
      return;
    }
  }

  std::vector<uint8_t> frame_copy;
  int width, height;
  {
    std::lock_guard<std::mutex> lock(latest_frame_mutex_);
    if (latest_frame_.empty()) {
      result->Error("no_frame", "No frame available for capture");
      return;
    }
    frame_copy = latest_frame_;
  }
  {
    std::lock_guard<std::mutex> lk(dims_mutex_);
    width  = actual_width_;
    height = actual_height_;
  }

  std::wstring path = PhotoHandler::GeneratePath(camera_id_);
  std::string write_error;
  if (PhotoHandler::Write(frame_copy.data(), width, height, path,
                          &write_error)) {
    int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0,
                                   nullptr, nullptr);
    std::string path_utf8(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, path_utf8.data(), size,
                        nullptr, nullptr);
    result->Success(flutter::EncodableValue(path_utf8));
  } else {
    result->Error("capture_failed", write_error);
  }
}

// ---------------------------------------------------------------------------
// Video recording
// ---------------------------------------------------------------------------

void Camera::StartVideoRecording(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != CameraState::kRunning && state_ != CameraState::kPaused) {
      result->Error("not_running", "Camera is not running");
      return;
    }
  }
  if (record_handler_->is_recording()) {
    result->Error("already_recording", "Recording is already in progress");
    return;
  }

  WCHAR temp_dir[MAX_PATH];
  GetTempPathW(MAX_PATH, temp_dir);
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::wostringstream ss;
  ss << temp_dir << L"camera_desktop_video_" << now << L".mp4";
  std::wstring path = ss.str();

  int w, h;
  {
    std::lock_guard<std::mutex> lk(dims_mutex_);
    w = actual_width_;
    h = actual_height_;
  }

  std::string rec_error;
  if (record_handler_->Start(path, w, h, config_.target_fps,
                              config_.enable_audio, &rec_error)) {
    result->Success(flutter::EncodableValue(nullptr));
  } else {
    result->Error("recording_failed", rec_error);
  }
}

void Camera::StopVideoRecording(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  DebugLog("Camera::StopVideoRecording called");
  if (!record_handler_->is_recording()) {
    DebugLog("Camera::StopVideoRecording not recording");
    result->Error("not_recording", "No recording in progress");
    return;
  }

  auto* raw_result = result.release();
  DebugLog("Camera::StopVideoRecording invoking RecordHandler::Stop");
  record_handler_->Stop([raw_result](const std::wstring& path) {
    DebugLog("Camera::StopVideoRecording stop callback begin");
    int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0,
                                   nullptr, nullptr);
    std::string path_utf8(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, path_utf8.data(), size,
                        nullptr, nullptr);
    raw_result->Success(flutter::EncodableValue(path_utf8));
    delete raw_result;
    DebugLog("Camera::StopVideoRecording stop callback end");
  });
  DebugLog("Camera::StopVideoRecording returned from RecordHandler::Stop");
}

// ---------------------------------------------------------------------------
// P6: Image stream delivery thread
// ---------------------------------------------------------------------------

void Camera::StartImageStream() {
  if (image_stream_thread_.joinable()) return;  // already running
  image_stream_running_ = true;
  image_streaming_      = true;
  image_stream_thread_  = std::thread(&Camera::ImageStreamLoop, this);
}

void Camera::StopImageStream() {
  image_streaming_      = false;
  image_stream_running_ = false;
  image_stream_cv_.notify_all();
  if (image_stream_thread_.joinable()) image_stream_thread_.join();
  image_stream_thread_ = std::thread{};
}

void Camera::PostImageStreamFrame(const uint8_t* data, int width, int height) {
  const size_t len = static_cast<size_t>(width) * height * 4;
  {
    std::lock_guard<std::mutex> lk(image_stream_mutex_);
    image_stream_slot_.data.assign(data, data + len);
    image_stream_slot_.width  = width;
    image_stream_slot_.height = height;
    image_stream_slot_.dirty  = true;
  }
  image_stream_cv_.notify_one();
}

void Camera::ImageStreamLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  while (image_stream_running_.load()) {
    ImageStreamSlot local;
    {
      std::unique_lock<std::mutex> lk(image_stream_mutex_);
      image_stream_cv_.wait(lk, [this] {
        return image_stream_slot_.dirty || !image_stream_running_.load();
      });
      if (!image_stream_running_.load()) break;
      local = std::move(image_stream_slot_);
      image_stream_slot_.dirty = false;
    }

    channel_->InvokeMethod(
        "imageStreamFrame",
        std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
            {flutter::EncodableValue("cameraId"),
             flutter::EncodableValue(camera_id_)},
            {flutter::EncodableValue("width"),
             flutter::EncodableValue(local.width)},
            {flutter::EncodableValue("height"),
             flutter::EncodableValue(local.height)},
            {flutter::EncodableValue("bytes"),
             flutter::EncodableValue(local.data)},
        }));
  }

  CoUninitialize();
}

// ---------------------------------------------------------------------------
// Preview control
// ---------------------------------------------------------------------------

void Camera::PausePreview() {
  preview_paused_ = true;
  std::lock_guard<std::mutex> lk(state_mutex_);
  if (state_ == CameraState::kRunning) state_ = CameraState::kPaused;
}

void Camera::ResumePreview() {
  preview_paused_ = false;
  std::lock_guard<std::mutex> lk(state_mutex_);
  if (state_ == CameraState::kPaused) state_ = CameraState::kRunning;
}

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

void Camera::SendError(const std::string& description) {
  channel_->InvokeMethod(
      "cameraError",
      std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
          {flutter::EncodableValue("cameraId"),
           flutter::EncodableValue(camera_id_)},
          {flutter::EncodableValue("description"),
           flutter::EncodableValue(description)},
      }));
}

// ---------------------------------------------------------------------------
// P5: Dispose — ordered shutdown
// ---------------------------------------------------------------------------

void Camera::Dispose() {
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ == CameraState::kDisposed ||
        state_ == CameraState::kDisposing) {
      return;
    }
    state_ = CameraState::kDisposing;
  }

  // Fail any pending init.
  {
    std::lock_guard<std::mutex> lock(pending_init_mutex_);
    if (pending_init_) {
      pending_init_->Error("disposed", "Camera disposed during initialization");
      pending_init_.reset();
    }
  }

  // 1. Cancel and join the init-timeout thread (P2).
  {
    std::lock_guard<std::mutex> lk(init_timeout_cancel_mutex_);
    init_timeout_cancelled_ = true;
  }
  init_timeout_cancel_cv_.notify_one();
  if (init_timeout_thread_.joinable()) init_timeout_thread_.join();

  // 2. Stop active recording (P5) — drains writer queue + finalises file.
  if (record_handler_ && record_handler_->is_recording()) {
    record_handler_->Stop([](const std::wstring&) {});
  }

  // 3. Stop image stream if active (P6).
  if (image_stream_thread_.joinable()) {
    StopImageStream();
  }

  // 4. Stop capture: flush reader to unblock blocking ReadSample (P5).
  running_ = false;
  if (reader_) {
    reader_->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  }

  // 5. Join capture thread.
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }

  // 6. Release reader.
  reader_.Reset();

  // 7. Unregister texture.
  if (texture_) {
    texture_->Unregister();
    texture_.reset();
  }

  // 8. Notify Dart.
  channel_->InvokeMethod(
      "cameraClosing",
      std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
          {flutter::EncodableValue("cameraId"),
           flutter::EncodableValue(camera_id_)},
      }));

  // 9. Final state.
  {
    std::lock_guard<std::mutex> lk(state_mutex_);
    state_ = CameraState::kDisposed;
  }
}
