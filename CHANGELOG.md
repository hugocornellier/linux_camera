## 1.0.0

First stable release of `camera_desktop`

### Platform implementations

* **macOS** — AVFoundation (`AVCaptureSession`, `AVAssetWriter`). Preview via `CVPixelBuffer` textures, H.264/AAC recording, native mirror support.
* **Windows** — Media Foundation (`IMFCaptureEngine`) with Direct3D 11 texture rendering. H.264/AAC recording via `IMFSinkWriter`.
* **Linux** — GStreamer + V4L2 (`v4l2src → videoconvert → appsink` pipeline). H.264/AAC recording with automatic encoder selection, native mirror via `videoflip`.

### Features

* Live camera preview with hardware-accelerated texture rendering on all platforms
* Photo capture, video recording, and real-time image streaming
* FFI-based zero-copy frame delivery (MethodChannel fallback for compatibility)
* Configurable resolution presets, FPS (5–60), and video bitrate
* Mirror/flip control (macOS and Linux)
* Pause/resume preview
* Runtime capability querying via `getPlatformCapabilities()`

## 0.0.8

* Migrate Windows implementation to IMFCaptureEngine

## 0.0.7

* Update example app to show settings panel

## 0.0.5

* Fix C linkage on Linux

## 0.0.4

* FFI-based image stream for reduced memory copies (3→2 per frame)
* Fix macOS Swift/ObjC interop for FFI bridge
* Fix image format reporting (Linux/Windows RGBA vs macOS BGRA)

## 0.0.3

* Performance improvements

## 0.0.2

* Add setMirror API and built-in camera sorting for DeviceEnumerator

## 0.0.1

* Linux camera support via GStreamer + V4L2.
* macOS camera support via AVFoundation.
* Windows camera support via Media Foundation.
* Full `camera_platform_interface` compliance.
* Photo capture, video recording, image streaming, and live preview.
