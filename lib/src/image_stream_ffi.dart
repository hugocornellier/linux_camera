import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:camera_platform_interface/camera_platform_interface.dart';

/// FFI struct matching the native ImageStreamBuffer layout (32-byte header).
///
/// Layout:
///   int64_t sequence      (offset 0)
///   int32_t width         (offset 8)
///   int32_t height        (offset 12)
///   int32_t bytes_per_row (offset 16)
///   int32_t format        (offset 20)  -- 0=BGRA, 1=RGBA
///   int32_t ready         (offset 24)  -- 1=Dart may read, 0=native writing
///   int32_t _pad          (offset 28)
///   uint8_t pixels[]      (offset 32)
final class ImageStreamBuffer extends Struct {
  @Int64()
  external int sequence;

  @Int32()
  external int width;

  @Int32()
  external int height;

  @Int32()
  external int bytesPerRow;

  @Int32()
  external int format;

  @Int32()
  external int ready;

  @Int32()
  // ignore: unused_field
  external int _pad;
}

/// Native function signatures.
typedef _GetBufferNative = Pointer<Void> Function(Int32 cameraId);
typedef _GetBufferDart = Pointer<Void> Function(int cameraId);

typedef _RegisterCallbackNative =
    Void Function(
      Int32 cameraId,
      Pointer<NativeFunction<Void Function(Int32)>> callback,
    );
typedef _RegisterCallbackDart =
    void Function(
      int cameraId,
      Pointer<NativeFunction<Void Function(Int32)>> callback,
    );

typedef _UnregisterCallbackNative = Void Function(Int32 cameraId);
typedef _UnregisterCallbackDart = void Function(int cameraId);

/// Manages FFI-based image stream for a single camera.
///
/// Instead of receiving frame data through MethodChannel serialization
/// (3 copies per frame), this reads directly from a native shared buffer
/// via dart:ffi (1 copy per frame — into a Dart-owned Uint8List).
///
/// If FFI setup fails (symbols not found, library not loadable), returns
/// null from [tryCreate] and the caller falls back to MethodChannel.
class ImageStreamFfi {
  ImageStreamFfi._(
    this._cameraId,
    this._getBuffer,
    this._registerCallback,
    this._unregisterCallback,
  );

  final int _cameraId;
  final _GetBufferDart _getBuffer;
  final _RegisterCallbackDart _registerCallback;
  final _UnregisterCallbackDart _unregisterCallback;

  NativeCallable<Void Function(Int32)>? _nativeCallable;
  StreamController<CameraImageData>? _controller;
  int _lastSequence = 0;

  /// Attempts to set up the FFI image stream.
  ///
  /// Returns null if the native library or symbols cannot be found.
  static ImageStreamFfi? tryCreate(int cameraId) {
    try {
      final lib = _loadNativeLibrary();

      final getBuffer = lib.lookupFunction<_GetBufferNative, _GetBufferDart>(
        'camera_desktop_get_image_stream_buffer',
      );
      final registerCallback = lib
          .lookupFunction<_RegisterCallbackNative, _RegisterCallbackDart>(
            'camera_desktop_register_image_stream_callback',
          );
      final unregisterCallback = lib
          .lookupFunction<_UnregisterCallbackNative, _UnregisterCallbackDart>(
            'camera_desktop_unregister_image_stream_callback',
          );

      return ImageStreamFfi._(
        cameraId,
        getBuffer,
        registerCallback,
        unregisterCallback,
      );
    } catch (_) {
      // Symbol lookup or library loading failed — fall back to MethodChannel.
      return null;
    }
  }

  static DynamicLibrary _loadNativeLibrary() {
    // On all desktop platforms, the plugin's native code is compiled into
    // a shared library loaded by the Flutter engine. DynamicLibrary.process()
    // searches the current process's symbol table.
    if (Platform.isMacOS || Platform.isLinux) {
      return DynamicLibrary.process();
    }
    if (Platform.isWindows) {
      // On Windows, try process first, then explicit DLL name.
      try {
        return DynamicLibrary.process();
      } catch (_) {
        return DynamicLibrary.open('camera_desktop_plugin.dll');
      }
    }
    throw UnsupportedError('Unsupported platform for FFI image stream');
  }

  /// Registers the FFI callback and starts delivering frames to [controller].
  void start(StreamController<CameraImageData> controller) {
    _controller = controller;
    _lastSequence = 0;

    _nativeCallable = NativeCallable<Void Function(Int32)>.listener(
      _onFrameReady,
    );

    _registerCallback(_cameraId, _nativeCallable!.nativeFunction);
  }

  /// Called from any native thread when a new frame is ready.
  /// Delivered to the Dart isolate's event loop by NativeCallable.listener.
  void _onFrameReady(int cameraId) {
    final controller = _controller;
    if (controller == null || controller.isClosed) return;

    final bufPtr = _getBuffer(_cameraId);
    if (bufPtr == nullptr) return;

    final buf = bufPtr.cast<ImageStreamBuffer>().ref;
    if (buf.ready != 1) return;

    // Skip duplicate frames.
    if (buf.sequence <= _lastSequence) return;
    _lastSequence = buf.sequence;

    final width = buf.width;
    final height = buf.height;
    final bytesPerRow = buf.bytesPerRow;
    final format = buf.format;
    final dataSize = bytesPerRow * height;

    // Create a zero-copy view over the native pixel buffer.
    final pixelsPtr = bufPtr.cast<Uint8>() + sizeOf<ImageStreamBuffer>();
    final nativeView = pixelsPtr.asTypedList(dataSize);

    // Copy into a Dart-owned Uint8List (1 copy — required by platform interface).
    final bytes = Uint8List.fromList(nativeView);

    final rawFormat = format == 0 ? 'BGRA' : 'RGBA';

    controller.add(
      CameraImageData(
        format: CameraImageFormat(ImageFormatGroup.bgra8888, raw: rawFormat),
        width: width,
        height: height,
        planes: [
          CameraImagePlane(
            bytes: bytes,
            bytesPerRow: bytesPerRow,
            bytesPerPixel: 4,
            width: width,
            height: height,
          ),
        ],
      ),
    );
  }

  /// Unregisters the native callback.
  void stop() {
    _unregisterCallback(_cameraId);
  }

  /// Releases all resources.
  void dispose() {
    _nativeCallable?.close();
    _nativeCallable = null;
    _controller = null;
  }
}
