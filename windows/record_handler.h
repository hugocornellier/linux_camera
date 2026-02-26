#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// Tagged item placed into the encoder queue.
struct RecordItem {
  enum class Kind { Video, Audio } kind = Kind::Video;

  // Video: raw BGRA pixels (copied at enqueue time)
  std::vector<uint8_t> video_data;
  int                  video_width  = 0;
  int                  video_height = 0;
  LONGLONG             presentation_time_100ns = 0;

  // Audio: MF sample forwarded directly (ref-counted by ComPtr)
  ComPtr<IMFSample> audio_sample;
};

class RecordHandler {
 public:
  RecordHandler();
  ~RecordHandler();

  // Starts recording to |path|. Returns false on error (sets |error|).
  bool Start(const std::wstring& path, int width, int height, int fps,
             bool enable_audio, std::string* error);

  // Stops recording, drains the writer queue, and finalises the file.
  // Calls |on_done(path)| when the file is ready.
  void Stop(std::function<void(const std::wstring&)> on_done);

  // Enqueues a video frame. Non-blocking: drops the frame if the queue is full.
  // |bgra| must already have the horizontal flip applied.
  // Timestamp is computed internally from a CFR frame counter.
  void AppendFrame(const uint8_t* bgra, int width, int height);

  bool is_recording() const { return is_recording_.load(); }

 private:
  void WriterLoop();
  void WriteVideoFrame(const RecordItem& item);
  void AudioLoop();
  // P1 stub — passes device timestamp through unchanged; replaced in P3.
  void RestampAudioSample(IMFSample* sample, LONGLONG device_ts);

  // ── Sink writer (owned exclusively by writer_thread_ after Start) ──────
  ComPtr<IMFSinkWriter> writer_;
  DWORD video_stream_index_ = 0;
  DWORD audio_stream_index_ = 0;

  int width_ = 0;
  int height_ = 0;
  int fps_    = 30;
  bool has_audio_ = false;
  std::wstring output_path_;

  LONGLONG frame_duration_100ns_ = 0;  // 1e7 / fps

  // ── Writer thread ───────────────────────────────────────────────────────
  std::thread        writer_thread_;
  std::atomic<bool>  writer_running_{false};

  // ── Encoder queue (video + audio) ──────────────────────────────────────
  static constexpr size_t kMaxVideoQueueItems = 8;
  static constexpr size_t kMaxAudioQueueItems = 64;
  std::queue<RecordItem>  queue_;
  std::mutex              queue_mutex_;
  std::condition_variable queue_cv_;
  std::atomic<size_t>     video_queue_size_{0};
  std::atomic<size_t>     audio_queue_size_{0};

  // ── Audio capture ───────────────────────────────────────────────────────
  ComPtr<IMFSourceReader> audio_reader_;
  std::thread             audio_thread_;
  std::atomic<bool>       audio_running_{false};

  // ── Timestamp tracking ──────────────────────────────────────────────────
  std::atomic<bool>    is_recording_{false};
  std::atomic<bool>    accepting_queue_items_{false};
  LONGLONG             video_frame_counter_  = 0;
  LONGLONG             recording_start_100ns_ = 0;  // QPC-derived, set in Start()

  bool    audio_ts_anchored_  = false;
  LONGLONG first_audio_dev_ts_ = 0;
  LONGLONG first_audio_rec_ts_ = 0;

  // Serializes Start/Stop transitions.
  std::mutex lifecycle_mutex_;

  // ── Async stop finalization ──────────────────────────────────────────────
  // Stop() moves audio_thread_ and writer_thread_ into this thread's closure
  // and returns immediately so the caller is never blocked.
  // ~RecordHandler() and Start() must join stop_thread_ before proceeding.
  std::thread stop_thread_;
};
