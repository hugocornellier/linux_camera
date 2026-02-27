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
