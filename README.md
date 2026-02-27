# camera_desktop

A Flutter camera plugin for desktop platforms. Implements
[`camera_platform_interface`](https://pub.dev/packages/camera_platform_interface)
so it works seamlessly with the standard
[`camera`](https://pub.dev/packages/camera) package and `CameraController`.

## Platform Support

| Platform | Backend | Status |
|----------|---------|--------|
| **Linux** | GStreamer + V4L2 | Included |
| **macOS** | AVFoundation | Included |
| **Windows** | Media Foundation | Included |

## Installation

Add `camera_desktop` alongside `camera` in your `pubspec.yaml`:

```yaml
dependencies:
  camera: ^0.11.0
  camera_desktop: ^0.0.1
```

That's it. All three desktop platforms are covered — no additional packages needed.

## Usage

Use the standard `camera` package API:

```dart
import 'package:camera/camera.dart';

final cameras = await availableCameras();
final controller = CameraController(cameras.first, ResolutionPreset.high);
await controller.initialize();

// Preview
CameraPreview(controller);

// Capture
final file = await controller.takePicture();

// Record
await controller.startVideoRecording();
final video = await controller.stopVideoRecording();
```

## Platform-Specific Setup

### Linux

Install GStreamer development libraries:

```bash
# Ubuntu/Debian
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good

# Fedora
sudo dnf install gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good

# Arch
sudo pacman -S gstreamer gst-plugins-base gst-plugins-good
```

### macOS

Add camera and microphone usage descriptions to your `Info.plist`:

```xml
<key>NSCameraUsageDescription</key>
<string>This app needs camera access.</string>
<key>NSMicrophoneUsageDescription</key>
<string>This app needs microphone access for video recording.</string>
```

For sandboxed apps, add to your entitlements:

```xml
<key>com.apple.security.device.camera</key>
<true/>
<key>com.apple.security.device.audio-input</key>
<true/>
```

### Windows

No additional setup required.

## Features

| Feature | Linux | macOS | Windows |
|---------|-------|-------|---------|
| Camera enumeration | Yes | Yes | Yes |
| Live preview | Yes | Yes | Yes |
| Photo capture | Yes | Yes | Yes |
| Video recording | Yes | Yes | Yes |
| Image streaming | Yes | Yes | No |
| Audio recording | Yes | Yes | Yes |
| Resolution presets | Yes | Yes | Yes |

## Limitations

Desktop cameras generally do not support mobile-oriented features:

- Flash/torch control
- Exposure/focus point selection
- Zoom (beyond 1.0x)
- Device orientation changes
- Pause/resume video recording

These methods either no-op or throw `CameraException` as appropriate.
