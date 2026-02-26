#include "record_handler.h"

#include <codecapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <objbase.h>
#include <strmif.h>

#include <chrono>
#include <cstring>
#include <sstream>

#include "logging.h"

// ---------------------------------------------------------------------------
// Helpers — static free functions
// ---------------------------------------------------------------------------

static HRESULT SetVideoOutputType(IMFSinkWriter* writer, int width, int height,
                                  int fps, DWORD* out_stream_index) {
  ComPtr<IMFMediaType> out_type;
  HRESULT hr = MFCreateMediaType(&out_type);
  if (FAILED(hr)) return hr;

  hr = out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) return hr;
  hr = out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1);
  if (FAILED(hr)) return hr;
  hr = out_type->SetUINT32(MF_MT_AVG_BITRATE, 4'000'000);
  if (FAILED(hr)) return hr;
  hr = out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (FAILED(hr)) return hr;

  return writer->AddStream(out_type.Get(), out_stream_index);
}

static HRESULT SetVideoInputType(IMFSinkWriter* writer, DWORD stream_index,
                                 int width, int height, int fps) {
  ComPtr<IMFMediaType> in_type;
  HRESULT hr = MFCreateMediaType(&in_type);
  if (FAILED(hr)) return hr;

  hr = in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) return hr;
  hr = in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) return hr;
  hr = MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps, 1);
  if (FAILED(hr)) return hr;
  hr = in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (FAILED(hr)) return hr;
  hr = in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
  if (FAILED(hr)) return hr;

  return writer->SetInputMediaType(stream_index, in_type.Get(), nullptr);
}

static HRESULT SetAudioOutputType(IMFSinkWriter* writer,
                                  DWORD* out_stream_index) {
  ComPtr<IMFMediaType> out_type;
  HRESULT hr = MFCreateMediaType(&out_type);
  if (FAILED(hr)) return hr;

  hr = out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (FAILED(hr)) return hr;
  hr = out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  if (FAILED(hr)) return hr;
  hr = out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  if (FAILED(hr)) return hr;
  hr = out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
  if (FAILED(hr)) return hr;
  hr = out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
  if (FAILED(hr)) return hr;

  return writer->AddStream(out_type.Get(), out_stream_index);
}

static HRESULT OpenAudioCapture(IMFSourceReader** out_reader) {
  ComPtr<IMFAttributes> attrs;
  HRESULT hr = MFCreateAttributes(&attrs, 1);
  if (FAILED(hr)) return hr;

  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
  if (FAILED(hr)) return hr;

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
  if (FAILED(hr) || count == 0) {
    if (devices) {
      for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
      CoTaskMemFree(devices);
    }
    return E_FAIL;
  }

  ComPtr<IMFMediaSource> source;
  hr = devices[0]->ActivateObject(IID_PPV_ARGS(&source));
  for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
  CoTaskMemFree(devices);
  if (FAILED(hr)) return hr;

  return MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, out_reader);
}

static HRESULT ConfigureAudioReader(IMFSourceReader* reader) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (FAILED(hr)) return hr;
  hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  if (FAILED(hr)) return hr;
  hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
  if (FAILED(hr)) return hr;

  return reader->SetCurrentMediaType(
      (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get());
}

// ---------------------------------------------------------------------------
// RecordHandler
// ---------------------------------------------------------------------------

RecordHandler::RecordHandler() = default;

RecordHandler::~RecordHandler() {
  // Signal all threads to stop promptly (no waiting here).
  if (is_recording_.load() || writer_thread_.joinable() ||
      audio_thread_.joinable()) {
    accepting_queue_items_ = false;
    is_recording_          = false;
    audio_running_         = false;
    writer_running_        = false;
    if (audio_reader_) {
      audio_reader_->Flush((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    }
    queue_cv_.notify_all();
  }
  // stop_thread_ holds references into our members (writer_, queue_mutex_,
  // etc.) — it MUST finish before this object is destroyed.
  if (stop_thread_.joinable()) stop_thread_.join();
  // Fallback join for the destructor-path (Stop() was never called).
  if (audio_thread_.joinable()) audio_thread_.join();
  if (writer_thread_.joinable()) writer_thread_.join();
  writer_.Reset();
  audio_reader_.Reset();
}

bool RecordHandler::Start(const std::wstring& path, int width, int height,
                          int fps, bool enable_audio, std::string* error) {
  std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mutex_);

  // Ensure any previous async finalization has completed before we reuse
  // member state (writer_, queue_, counters, etc.).
  if (stop_thread_.joinable()) {
    DebugLog("RecordHandler::Start waiting for previous stop_thread_");
    stop_thread_.join();
    DebugLog("RecordHandler::Start previous stop_thread_ joined");
  }

  if (is_recording_.load()) {
    if (error) *error = "Recording already in progress";
    return false;
  }

  output_path_ = path;
  width_  = width;
  height_ = height;
  fps_    = fps > 0 ? fps : 30;
  frame_duration_100ns_ = 10'000'000LL / fps_;

  // ── Create sink writer — software encoder, default throttling ──────────
  // MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS is intentionally NOT set (defaults
  // to FALSE).  Hardware H264 encoders warm up their pipeline asynchronously
  // and can cause WriteSample / Finalize to block indefinitely during stop.
  // Software encoding is bounded and predictable.
  //
  // MF_SINK_WRITER_DISABLE_THROTTLING is also NOT set.  Disabling throttling
  // removes the natural back-pressure that prevents the encoder queue from
  // filling up and stalling on the first WriteSample call.
  HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr,
                                          &writer_);
  if (FAILED(hr)) {
    if (error) *error = "Failed to create sink writer";
    return false;
  }

  hr = SetVideoOutputType(writer_.Get(), width_, height_, fps_,
                          &video_stream_index_);
  if (FAILED(hr)) {
    if (error) *error = "Failed to set video output type";
    return false;
  }

  hr = SetVideoInputType(writer_.Get(), video_stream_index_, width_, height_,
                         fps_);
  if (FAILED(hr)) {
    if (error) *error = "Failed to set video input type";
    return false;
  }

  has_audio_ = false;
  if (enable_audio) {
    hr = SetAudioOutputType(writer_.Get(), &audio_stream_index_);
    if (SUCCEEDED(hr)) {
      ComPtr<IMFMediaType> audio_in;
      if (SUCCEEDED(MFCreateMediaType(&audio_in)) &&
          SUCCEEDED(audio_in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) &&
          SUCCEEDED(audio_in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) &&
          SUCCEEDED(audio_in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2)) &&
          SUCCEEDED(audio_in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                        44100))) {
        hr = writer_->SetInputMediaType(audio_stream_index_, audio_in.Get(),
                                        nullptr);
      }
      if (SUCCEEDED(hr)) {
        ComPtr<IMFSourceReader> ar;
        if (SUCCEEDED(OpenAudioCapture(&ar)) &&
            SUCCEEDED(ConfigureAudioReader(ar.Get()))) {
          audio_reader_ = ar;
          has_audio_ = true;
        }
      }
    }
    // Audio failures are non-fatal — continue with video only.
  }

  hr = writer_->BeginWriting();
  if (FAILED(hr)) {
    if (error) *error = "Failed to begin writing";
    return false;
  }

  // ── Low-latency codec API (must be after BeginWriting, before writer_thread_) ──
  {
    ICodecAPI* codec_api = nullptr;
    HRESULT codec_hr = writer_->GetServiceForStream(
        video_stream_index_,
        GUID_NULL,
        IID_ICodecAPI,
        reinterpret_cast<void**>(&codec_api));
    if (SUCCEEDED(codec_hr) && codec_api) {
      VARIANT var = {};
      var.vt      = VT_BOOL;
      var.boolVal = VARIANT_TRUE;
      codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
      codec_api->Release();
    }
    // Non-fatal if encoder doesn't support this.
  }

  // ── Record QPC start time for audio timestamp anchoring ─────────────────
  LARGE_INTEGER qpc_freq, qpc_now;
  QueryPerformanceFrequency(&qpc_freq);
  QueryPerformanceCounter(&qpc_now);
  recording_start_100ns_ =
      (qpc_now.QuadPart * 10'000'000LL) / qpc_freq.QuadPart;

  // ── Reset per-recording state ────────────────────────────────────────────
  {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    while (!queue_.empty()) queue_.pop();
  }
  video_frame_counter_ = 0;
  audio_ts_anchored_   = false;
  video_queue_size_    = 0;
  audio_queue_size_    = 0;

  accepting_queue_items_ = true;
  is_recording_ = true;

  // ── Start writer thread (now sole owner of writer_) ──────────────────────
  writer_running_ = true;
  writer_thread_  = std::thread(&RecordHandler::WriterLoop, this);

  // ── Start audio thread (feeds the queue) ────────────────────────────────
  if (has_audio_ && audio_reader_) {
    audio_running_ = true;
    audio_thread_  = std::thread(&RecordHandler::AudioLoop, this);
  }

  return true;
}

void RecordHandler::Stop(std::function<void(const std::wstring&)> on_done) {
  std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mutex_);
  {
    std::ostringstream ss;
    ss << "RecordHandler::Stop begin"
       << " is_recording=" << is_recording_.load()
       << " writer_joinable=" << writer_thread_.joinable()
       << " audio_joinable=" << audio_thread_.joinable()
       << " stop_joinable=" << stop_thread_.joinable()
       << " video_q=" << video_queue_size_.load()
       << " audio_q=" << audio_queue_size_.load();
    DebugLog(ss.str());
  }

  // Already fully idle (including no pending async stop)?
  if (!is_recording_.load() && !audio_thread_.joinable() &&
      !writer_thread_.joinable() && !stop_thread_.joinable()) {
    DebugLog("RecordHandler::Stop early-return (already stopped)");
    on_done(output_path_);
    return;
  }

  // ── Fast path: signal all threads synchronously ─────────────────────────
  // These are cheap flag writes + one MF flush.  They do NOT block.

  accepting_queue_items_ = false;
  is_recording_          = false;
  audio_running_         = false;

  // Flush audio source reader so AudioLoop's blocking ReadSample returns fast.
  if (audio_reader_) {
    DebugLog("RecordHandler::Stop flushing audio reader");
    audio_reader_->Flush((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    DebugLog("RecordHandler::Stop audio reader flush returned");
  }

  // Tell WriterLoop to drain remaining queued frames and call Finalize.
  writer_running_ = false;
  queue_cv_.notify_all();

  // ── Move threads out of member slots ────────────────────────────────────
  // After the moves audio_thread_ / writer_thread_ become unjoinable here,
  // preventing accidental double-join from the destructor or a future Start().
  std::thread audio_t  = std::move(audio_thread_);
  std::thread writer_t = std::move(writer_thread_);

  std::wstring path = output_path_;

  {
    std::ostringstream ss;
    ss << "RecordHandler::Stop spawning stop_thread"
       << " video_q=" << video_queue_size_.load()
       << " audio_q=" << audio_queue_size_.load();
    DebugLog(ss.str());
  }

  // ── Spawn background finalizer ───────────────────────────────────────────
  // The stop_thread_ owns the joins and calls on_done() when the file is
  // ready.  Stop() returns immediately so the Flutter platform thread is
  // never blocked.
  //
  // Lifetime guarantee: ~RecordHandler() joins stop_thread_ before destroying
  // any members (writer_, queue_mutex_, etc.) that this closure touches.
  //
  // NOTE: audio_reader_ is NOT moved out here.  AudioLoop accesses it
  // directly via this->audio_reader_, so it must remain valid until audio_t
  // has been joined.  The stop thread resets it through 'this' after the join.
  stop_thread_ = std::thread(
      [this, path, on_done,
       audio_t  = std::move(audio_t),
       writer_t = std::move(writer_t)]() mutable {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DebugLog("RecordHandler::stop_thread begin");

        if (audio_t.joinable()) {
          DebugLog("RecordHandler::stop_thread joining audio thread");
          audio_t.join();
          DebugLog("RecordHandler::stop_thread audio thread joined");
        }
        // Audio thread has fully exited — safe to release the source reader.
        audio_reader_.Reset();

        if (writer_t.joinable()) {
          DebugLog("RecordHandler::stop_thread joining writer thread"
                   " (drains queue + Finalize inside WriterLoop)");
          writer_t.join();
          DebugLog("RecordHandler::stop_thread writer thread joined");
        }

        // writer_ is now exclusively accessed by this thread — safe to reset.
        writer_.Reset();

        {
          std::lock_guard<std::mutex> lk(queue_mutex_);
          while (!queue_.empty()) queue_.pop();
        }
        video_queue_size_ = 0;
        audio_queue_size_ = 0;

        DebugLog("RecordHandler::stop_thread calling on_done");
        on_done(path);
        DebugLog("RecordHandler::stop_thread end");

        CoUninitialize();
      });

  DebugLog("RecordHandler::Stop returned (finalization running in background)");
}

void RecordHandler::AppendFrame(const uint8_t* bgra, int width, int height) {
  if (!is_recording_.load(std::memory_order_relaxed) ||
      !accepting_queue_items_.load(std::memory_order_relaxed)) {
    return;
  }

  const size_t len = static_cast<size_t>(width) * height * 4;

  RecordItem item;
  item.kind         = RecordItem::Kind::Video;
  item.video_width  = width;
  item.video_height = height;
  item.video_data.assign(bgra, bgra + len);
  // Tentative CFR timestamp — committed (counter++) only if we enqueue.
  item.presentation_time_100ns =
      video_frame_counter_ * frame_duration_100ns_;

  {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    if (!accepting_queue_items_.load(std::memory_order_relaxed) ||
        !writer_running_.load(std::memory_order_relaxed) ||
        !is_recording_.load(std::memory_order_relaxed)) {
      return;
    }
    if (video_queue_size_.load(std::memory_order_relaxed) >=
        kMaxVideoQueueItems) {
      // Queue full: drop this frame. Do NOT increment counter so the next
      // delivered frame is timestamp-contiguous.
      return;
    }
    queue_.push(std::move(item));
    video_queue_size_.fetch_add(1, std::memory_order_relaxed);
  }
  ++video_frame_counter_;
  queue_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

void RecordHandler::WriterLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DebugLog("RecordHandler::WriterLoop started");
  uint64_t processed_items = 0;

  while (true) {
    RecordItem item;
    {
      std::unique_lock<std::mutex> lk(queue_mutex_);
      queue_cv_.wait(lk, [this] {
        return !queue_.empty() || !writer_running_.load();
      });

      // Stop signaled: drop any remaining tail frames and finalize immediately.
      // Encoding the last ~N frames after stop is signaled only adds latency;
      // Finalize() produces a valid MP4 from whatever was already encoded.
      if (!writer_running_.load()) {
        const size_t dropped = queue_.size();
        while (!queue_.empty()) queue_.pop();
        video_queue_size_ = 0;
        audio_queue_size_ = 0;
        {
          std::ostringstream ss;
          ss << "RecordHandler::WriterLoop stop signaled, dropped="
             << dropped << " tail items, processed=" << processed_items;
          DebugLog(ss.str());
        }
        break;
      }

      if (queue_.empty()) break;  // normal drain completion (shouldn't reach)

      item = std::move(queue_.front());
      queue_.pop();
    }

    {
      std::ostringstream ss;
      ss << "RecordHandler::WriterLoop dequeued kind="
         << (item.kind == RecordItem::Kind::Video ? "video" : "audio")
         << " processed=" << processed_items
         << " remaining video_q=" << video_queue_size_.load()
         << " audio_q=" << audio_queue_size_.load();
      DebugLog(ss.str());
    }

    if (item.kind == RecordItem::Kind::Video) {
      video_queue_size_.fetch_sub(1, std::memory_order_relaxed);
      WriteVideoFrame(item);
    } else {
      audio_queue_size_.fetch_sub(1, std::memory_order_relaxed);
      HRESULT hr = writer_->WriteSample(audio_stream_index_,
                                        item.audio_sample.Get());
      if (FAILED(hr)) {
        DebugLog("WriteSample audio failed: " + HrToString(hr));
      }
    }
    ++processed_items;
  }

  DebugLog("RecordHandler::WriterLoop calling Finalize");
  HRESULT hr = writer_->Finalize();
  if (FAILED(hr)) {
    DebugLog("Finalize failed: " + HrToString(hr));
  } else {
    DebugLog("RecordHandler::WriterLoop Finalize succeeded");
  }

  DebugLog("RecordHandler::WriterLoop exiting");
  CoUninitialize();
}

void RecordHandler::WriteVideoFrame(const RecordItem& item) {
  const DWORD data_size =
      static_cast<DWORD>(item.video_width) *
      static_cast<DWORD>(item.video_height) * 4;

  ComPtr<IMFMediaBuffer> mf_buffer;
  if (FAILED(MFCreateMemoryBuffer(data_size, &mf_buffer))) return;

  BYTE* buf = nullptr;
  if (FAILED(mf_buffer->Lock(&buf, nullptr, nullptr))) return;
  std::memcpy(buf, item.video_data.data(), data_size);
  mf_buffer->Unlock();
  mf_buffer->SetCurrentLength(data_size);

  ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample))) return;
  sample->AddBuffer(mf_buffer.Get());
  sample->SetSampleTime(item.presentation_time_100ns);
  sample->SetSampleDuration(frame_duration_100ns_);

  {
    std::ostringstream ss;
    ss << "RecordHandler::WriteVideoFrame WriteSample begin"
       << " ts=" << item.presentation_time_100ns
       << " dur=" << frame_duration_100ns_
       << " size=" << item.video_width << "x" << item.video_height;
    DebugLog(ss.str());
  }
  HRESULT hr = writer_->WriteSample(video_stream_index_, sample.Get());
  if (FAILED(hr)) {
    DebugLog("WriteSample video failed: " + HrToString(hr));
  } else {
    DebugLog("RecordHandler::WriteVideoFrame WriteSample end");
  }
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void RecordHandler::AudioLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DebugLog("RecordHandler::AudioLoop started");

  while (audio_running_) {
    DWORD stream_index, flags;
    LONGLONG device_ts;
    ComPtr<IMFSample> sample;

    HRESULT hr = audio_reader_->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        0, &stream_index, &flags, &device_ts, &sample);

    if (FAILED(hr)) {
      DebugLog("RecordHandler::AudioLoop ReadSample failed: " + HrToString(hr));
      break;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      std::ostringstream ss;
      ss << "RecordHandler::AudioLoop terminating on flags=0x" << std::hex
         << flags;
      DebugLog(ss.str());
      break;
    }
    if (flags & MF_SOURCE_READERF_STREAMTICK) continue;
    if (!sample || !is_recording_.load(std::memory_order_relaxed)) continue;

    RestampAudioSample(sample.Get(), device_ts);

    RecordItem item;
    item.kind         = RecordItem::Kind::Audio;
    item.audio_sample = sample;
    {
      std::lock_guard<std::mutex> lk(queue_mutex_);
      if (!accepting_queue_items_.load(std::memory_order_relaxed) ||
          !writer_running_.load(std::memory_order_relaxed) ||
          !is_recording_.load(std::memory_order_relaxed)) {
        DebugLog("RecordHandler::AudioLoop drop audio sample during shutdown");
        continue;
      }
      if (audio_queue_size_.load(std::memory_order_relaxed) >=
          kMaxAudioQueueItems) {
        DebugLog("RecordHandler::AudioLoop drop audio sample (queue full)");
        continue;
      }
      queue_.push(std::move(item));
      audio_queue_size_.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv_.notify_one();
  }

  DebugLog("RecordHandler::AudioLoop exiting");
  CoUninitialize();
}

// ---------------------------------------------------------------------------
// P3 — Audio timestamp anchoring (QPC-based fixed mapping)
// ---------------------------------------------------------------------------

void RecordHandler::RestampAudioSample(IMFSample* sample, LONGLONG device_ts) {
  if (!audio_ts_anchored_) {
    first_audio_dev_ts_ = device_ts;
    first_audio_rec_ts_ = recording_start_100ns_;
    audio_ts_anchored_  = true;
  }

  LONGLONG mapped_ts =
      first_audio_rec_ts_ + (device_ts - first_audio_dev_ts_);
  if (mapped_ts < 0) mapped_ts = 0;
  sample->SetSampleTime(mapped_ts);
}
