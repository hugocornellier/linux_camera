import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:camera_platform_interface/camera_platform_interface.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:stream_transform/stream_transform.dart';

import 'image_stream_ffi.dart';

/// Desktop implementation of [CameraPlatform].
///
/// On Linux, uses GStreamer + V4L2. On macOS, uses AVFoundation.
/// This plugin registers itself as the camera platform implementation for
/// desktop. When an app depends on both `camera` and `camera_desktop`, Flutter
/// automatically calls [registerWith], making [CameraController] work out of
/// the box.
class CameraDesktopPlugin extends CameraPlatform {
  /// Creates a new [CameraDesktopPlugin].
  ///
  /// The [channel] parameter is exposed for testing only.
  CameraDesktopPlugin({
    @visibleForTesting MethodChannel? channel,
    this.mirrorPreview = true,
  }) : _channel =
           channel ?? const MethodChannel('plugins.flutter.io/camera_desktop');

  /// Registers this class as the default [CameraPlatform] implementation.
  static void registerWith() {
    CameraPlatform.instance = CameraDesktopPlugin();
  }

  final MethodChannel _channel;

  /// Whether to mirror the preview horizontally (like a mirror).
  /// Defaults to `true`. Set to `false` to show the unmirrored camera image.
  @Deprecated('Mirroring is now handled at the native capture level.')
  final bool mirrorPreview;

  bool _nativeCallHandlerSet = false;

  /// Lazily installs the native → Dart method-call handler.
  ///
  /// Called before the first camera is created. This cannot run in the
  /// constructor because [registerWith] executes during plugin registration,
  /// before [WidgetsFlutterBinding.ensureInitialized].
  void _ensureNativeCallHandler() {
    if (!_nativeCallHandlerSet) {
      _channel.setMethodCallHandler(_handleNativeCall);
      _nativeCallHandlerSet = true;
    }
  }

  /// Mapping from cameraId to textureId (separate to decouple lifecycles).
  final Map<int, int> _textureIds = {};

  /// Broadcast stream for all camera events, filtered by cameraId downstream.
  final StreamController<CameraEvent> _eventStreamController =
      StreamController<CameraEvent>.broadcast();

  /// Per-camera image stream controllers for onStreamedFrameAvailable.
  final Map<int, StreamController<CameraImageData>> _imageStreamControllers =
      {};

  /// Handles method calls from the native side (events pushed to Dart).
  Future<dynamic> _handleNativeCall(MethodCall call) async {
    final args = call.arguments as Map<Object?, Object?>?;
    switch (call.method) {
      case 'cameraError':
        final cameraId = args!['cameraId']! as int;
        final description = args['description']! as String;
        _eventStreamController.add(CameraErrorEvent(cameraId, description));
      case 'cameraClosing':
        final cameraId = args!['cameraId']! as int;
        _eventStreamController.add(CameraClosingEvent(cameraId));
      case 'imageStreamFrame':
        final cameraId = args!['cameraId']! as int;
        final controller = _imageStreamControllers[cameraId];
        if (controller != null && !controller.isClosed) {
          final width = args['width']! as int;
          final height = args['height']! as int;
          final bytes = args['bytes']! as Uint8List;
          controller.add(
            CameraImageData(
              format: CameraImageFormat(
                ImageFormatGroup.bgra8888,
                raw: Platform.isMacOS ? 'BGRA' : 'RGBA',
              ),
              width: width,
              height: height,
              planes: [
                CameraImagePlane(
                  bytes: bytes,
                  bytesPerRow: width * 4,
                  bytesPerPixel: 4,
                  width: width,
                  height: height,
                ),
              ],
            ),
          );
        }
    }
  }

  // -- Helper --

  Stream<CameraEvent> _cameraEvents(int cameraId) => _eventStreamController
      .stream
      .where((CameraEvent e) => e.cameraId == cameraId);

  // ---------------------------------------------------------------------------
  // Camera discovery
  // ---------------------------------------------------------------------------

  @override
  Future<List<CameraDescription>> availableCameras() async {
    final result = await _channel.invokeListMethod<Map<dynamic, dynamic>>(
      'availableCameras',
    );
    if (result == null) return <CameraDescription>[];
    return result.map((Map<dynamic, dynamic> m) {
      return CameraDescription(
        name: m['name'] as String,
        lensDirection: CameraLensDirection.values[m['lensDirection'] as int],
        sensorOrientation: m['sensorOrientation'] as int,
      );
    }).toList();
  }

  // ---------------------------------------------------------------------------
  // Camera lifecycle
  // ---------------------------------------------------------------------------

  @override
  Future<int> createCamera(
    CameraDescription cameraDescription,
    ResolutionPreset? resolutionPreset, {
    bool enableAudio = false,
  }) async {
    return createCameraWithSettings(
      cameraDescription,
      MediaSettings(
        resolutionPreset: resolutionPreset,
        enableAudio: enableAudio,
      ),
    );
  }

  @override
  Future<int> createCameraWithSettings(
    CameraDescription cameraDescription,
    MediaSettings mediaSettings,
  ) async {
    _ensureNativeCallHandler();
    try {
      final result = await _channel.invokeMapMethod<String, dynamic>('create', {
        'cameraName': cameraDescription.name,
        'resolutionPreset':
            mediaSettings.resolutionPreset?.index ?? ResolutionPreset.max.index,
        'enableAudio': mediaSettings.enableAudio,
        'fps': mediaSettings.fps,
      });
      final cameraId = result!['cameraId'] as int;
      final textureId = result['textureId'] as int;
      _textureIds[cameraId] = textureId;
      return cameraId;
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<void> initializeCamera(
    int cameraId, {
    ImageFormatGroup imageFormatGroup = ImageFormatGroup.unknown,
  }) async {
    try {
      final result = await _channel.invokeMapMethod<String, dynamic>(
        'initialize',
        {'cameraId': cameraId},
      );
      _eventStreamController.add(
        CameraInitializedEvent(
          cameraId,
          (result!['previewWidth'] as num).toDouble(),
          (result['previewHeight'] as num).toDouble(),
          ExposureMode.auto,
          false,
          FocusMode.auto,
          false,
        ),
      );
    } on PlatformException catch (e) {
      _eventStreamController.add(
        CameraErrorEvent(cameraId, e.message ?? 'Initialization failed'),
      );
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<void> dispose(int cameraId) async {
    try {
      await _channel.invokeMethod<void>('dispose', {'cameraId': cameraId});
    } on PlatformException {
      // Ignore errors during disposal.
    } finally {
      _textureIds.remove(cameraId);
      final imageController = _imageStreamControllers.remove(cameraId);
      if (imageController != null && !imageController.isClosed) {
        imageController.close();
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Event streams
  // ---------------------------------------------------------------------------

  @override
  Stream<CameraInitializedEvent> onCameraInitialized(int cameraId) =>
      _cameraEvents(cameraId).whereType<CameraInitializedEvent>();

  @override
  Stream<CameraResolutionChangedEvent> onCameraResolutionChanged(
    int cameraId,
  ) => _cameraEvents(cameraId).whereType<CameraResolutionChangedEvent>();

  @override
  Stream<CameraClosingEvent> onCameraClosing(int cameraId) =>
      _cameraEvents(cameraId).whereType<CameraClosingEvent>();

  @override
  Stream<CameraErrorEvent> onCameraError(int cameraId) =>
      _cameraEvents(cameraId).whereType<CameraErrorEvent>();

  @override
  Stream<VideoRecordedEvent> onVideoRecordedEvent(int cameraId) =>
      _cameraEvents(cameraId).whereType<VideoRecordedEvent>();

  @override
  Stream<DeviceOrientationChangedEvent> onDeviceOrientationChanged() =>
      Stream<DeviceOrientationChangedEvent>.value(
        const DeviceOrientationChangedEvent(DeviceOrientation.landscapeLeft),
      );

  // ---------------------------------------------------------------------------
  // Preview
  // ---------------------------------------------------------------------------

  @override
  Widget buildPreview(int cameraId) {
    final textureId = _textureIds[cameraId];
    if (textureId == null) {
      throw CameraException(
        'buildPreview',
        'Camera $cameraId has no registered texture. '
            'Was createCamera called?',
      );
    }
    return Texture(textureId: textureId);
  }

  @override
  Future<void> pausePreview(int cameraId) async {
    try {
      await _channel.invokeMethod<void>('pausePreview', {'cameraId': cameraId});
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<void> resumePreview(int cameraId) async {
    try {
      await _channel.invokeMethod<void>('resumePreview', {
        'cameraId': cameraId,
      });
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  // ---------------------------------------------------------------------------
  // Mirror control
  // ---------------------------------------------------------------------------

  /// Toggles horizontal mirroring on the live camera feed.
  ///
  /// On macOS, this sets `isVideoMirrored` on the AVCaptureConnection.
  /// On Linux, this toggles the `videoflip` GStreamer element's method.
  /// On Windows, this is a no-op (mirror is handled at the Dart/app level).
  ///
  /// Can be called while the camera is running — no restart needed.
  Future<void> setMirror(int cameraId, bool mirrored) async {
    try {
      await _channel.invokeMethod<void>('setMirror', {
        'cameraId': cameraId,
        'mirrored': mirrored,
      });
    } on MissingPluginException {
      // No native handler on this platform (e.g., Windows) — silently ignore.
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  // ---------------------------------------------------------------------------
  // Image streaming
  // ---------------------------------------------------------------------------

  @override
  bool supportsImageStreaming() => true;

  @override
  Stream<CameraImageData> onStreamedFrameAvailable(
    int cameraId, {
    CameraImageStreamOptions? options,
  }) {
    final ffi = ImageStreamFfi.tryCreate(cameraId);
    late final StreamController<CameraImageData> controller;

    controller = StreamController<CameraImageData>(
      onListen: () {
        _channel.invokeMethod<void>('startImageStream', {'cameraId': cameraId});
        ffi?.start(controller);
      },
      onCancel: () {
        ffi?.stop();
        ffi?.dispose();
        _imageStreamControllers.remove(cameraId);
        _channel.invokeMethod<void>('stopImageStream', {'cameraId': cameraId});
      },
    );

    if (ffi == null) {
      // Fallback: use MethodChannel path for frame delivery.
      _imageStreamControllers[cameraId] = controller;
    }
    // When FFI is active, don't register in _imageStreamControllers so that
    // _handleNativeCall("imageStreamFrame") is a no-op for this camera.

    return controller.stream;
  }

  // ---------------------------------------------------------------------------
  // Image capture
  // ---------------------------------------------------------------------------

  @override
  Future<XFile> takePicture(int cameraId) async {
    try {
      final path = await _channel.invokeMethod<String>('takePicture', {
        'cameraId': cameraId,
      });
      return XFile(path!);
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  // ---------------------------------------------------------------------------
  // Video recording
  // ---------------------------------------------------------------------------

  @override
  Future<void> prepareForVideoRecording() async {
    // No-op on desktop.
  }

  @override
  Future<void> startVideoRecording(
    int cameraId, {
    Duration? maxVideoDuration,
  }) async {
    try {
      await _channel.invokeMethod<void>('startVideoRecording', {
        'cameraId': cameraId,
        if (maxVideoDuration != null)
          'maxVideoDuration': maxVideoDuration.inMilliseconds,
      });
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<XFile> stopVideoRecording(int cameraId) async {
    try {
      final path = await _channel.invokeMethod<String>('stopVideoRecording', {
        'cameraId': cameraId,
      });
      return XFile(path!);
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<void> pauseVideoRecording(int cameraId) async {
    throw CameraException(
      'pauseVideoRecording',
      'Pausing video recording is not supported on desktop.',
    );
  }

  @override
  Future<void> resumeVideoRecording(int cameraId) async {
    throw CameraException(
      'resumeVideoRecording',
      'Resuming video recording is not supported on desktop.',
    );
  }

  // ---------------------------------------------------------------------------
  // Camera controls (Phase 3 — sensible defaults / unsupported)
  // ---------------------------------------------------------------------------

  @override
  Future<void> setFlashMode(int cameraId, FlashMode mode) async {
    if (mode == FlashMode.off) {
      return; // No-op; desktop cameras typically lack flash.
    }
    throw CameraException(
      'setFlashMode',
      'Flash mode is not supported on desktop.',
    );
  }

  @override
  Future<void> setExposureMode(int cameraId, ExposureMode mode) async {
    if (mode == ExposureMode.auto) return; // No-op; auto is the default.
    throw CameraException(
      'setExposureMode',
      'Exposure mode control is not supported on desktop.',
    );
  }

  @override
  Future<void> setExposurePoint(int cameraId, Point<double>? point) async {
    throw CameraException(
      'setExposurePoint',
      'Exposure point is not supported on desktop.',
    );
  }

  @override
  Future<double> getMinExposureOffset(int cameraId) async => 0.0;

  @override
  Future<double> getMaxExposureOffset(int cameraId) async => 0.0;

  @override
  Future<double> getExposureOffsetStepSize(int cameraId) async => 0.0;

  @override
  Future<double> setExposureOffset(int cameraId, double offset) async => 0.0;

  @override
  Future<void> setFocusMode(int cameraId, FocusMode mode) async {
    if (mode == FocusMode.auto) return; // No-op; auto is the default.
    throw CameraException(
      'setFocusMode',
      'Focus mode control is not supported on desktop.',
    );
  }

  @override
  Future<void> setFocusPoint(int cameraId, Point<double>? point) async {
    throw CameraException(
      'setFocusPoint',
      'Focus point is not supported on desktop.',
    );
  }

  @override
  Future<double> getMinZoomLevel(int cameraId) async => 1.0;

  @override
  Future<double> getMaxZoomLevel(int cameraId) async => 1.0;

  @override
  Future<void> setZoomLevel(int cameraId, double zoom) async {
    if (zoom != 1.0) {
      throw CameraException(
        'setZoomLevel',
        'Zoom is not supported on desktop. Only 1.0 is accepted.',
      );
    }
  }

  // ---------------------------------------------------------------------------
  // Orientation (no-op on desktop)
  // ---------------------------------------------------------------------------

  @override
  Future<void> lockCaptureOrientation(
    int cameraId,
    DeviceOrientation orientation,
  ) async {
    // No-op on desktop.
  }

  @override
  Future<void> unlockCaptureOrientation(int cameraId) async {
    // No-op on desktop.
  }

  // ---------------------------------------------------------------------------
  // Miscellaneous
  // ---------------------------------------------------------------------------

  @override
  Future<void> setDescriptionWhileRecording(
    CameraDescription description,
  ) async {
    throw CameraException(
      'setDescriptionWhileRecording',
      'Switching camera during recording is not supported on desktop.',
    );
  }
}
