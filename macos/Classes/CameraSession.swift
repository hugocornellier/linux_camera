import AVFoundation
import FlutterMacOS
import QuartzCore

/// Manages a persistent shared buffer for zero-copy FFI image stream delivery.
/// Native writes frame data here; Dart reads it directly via FFI pointer.
/// Uses a double-buffer strategy so writeFrame() never holds the lock during memcpy.
class ImageStreamFFI {
    // Buffer layout matches C struct ImageStreamBuffer:
    //   int64_t sequence (8 bytes, offset 0)
    //   int32_t width (4 bytes, offset 8)
    //   int32_t height (4 bytes, offset 12)
    //   int32_t bytes_per_row (4 bytes, offset 16)
    //   int32_t format (4 bytes, offset 20) -- 0=BGRA, 1=RGBA
    //   int32_t ready (4 bytes, offset 24) -- 1=ready for Dart, 0=being written
    //   int32_t _pad (4 bytes, offset 28)
    //   uint8_t pixels[] (offset 32)
    static let headerSize = 32

    private var buffers: (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?) = (nil, nil)
    private var bufferSizes: (Int, Int) = (0, 0)
    private var frontIndex: Int = 0  // 0 or 1 — which buffer Dart reads from
    private var callback: (@convention(c) (Int32) -> Void)?
    private var sequence: Int64 = 0
    private let lock = UnfairLock()

    func getBufferPointer() -> UnsafeMutableRawPointer? {
        lock.lock()
        let idx = frontIndex
        lock.unlock()
        return idx == 0 ? buffers.0 : buffers.1
    }

    var hasCallback: Bool {
        lock.lock()
        defer { lock.unlock() }
        return callback != nil
    }

    func registerCallback(_ cb: @convention(c) (Int32) -> Void) {
        lock.lock()
        callback = cb
        lock.unlock()
    }

    func unregisterCallback() {
        lock.lock()
        callback = nil
        lock.unlock()
    }

    func writeFrame(pixelBuffer: CVPixelBuffer, cameraId: Int) {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

        guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        let dataSize = bytesPerRow * height
        let totalSize = ImageStreamFFI.headerSize + dataSize

        // Determine back buffer index (brief lock)
        lock.lock()
        let backIdx = 1 - frontIndex
        lock.unlock()

        // Ensure back buffer is large enough (only the back buffer is reallocated)
        let backSize = backIdx == 0 ? bufferSizes.0 : bufferSizes.1
        var backBuf = backIdx == 0 ? buffers.0 : buffers.1

        if backSize < totalSize {
            backBuf?.deallocate()
            backBuf = UnsafeMutableRawPointer.allocate(byteCount: totalSize, alignment: 8)
            if backIdx == 0 {
                buffers.0 = backBuf
                bufferSizes.0 = totalSize
            } else {
                buffers.1 = backBuf
                bufferSizes.1 = totalSize
            }
        }

        guard let buf = backBuf else { return }

        // Write to back buffer — no lock held during memcpy
        buf.storeBytes(of: Int32(0), toByteOffset: 24, as: Int32.self) // ready=0
        memcpy(buf.advanced(by: ImageStreamFFI.headerSize), baseAddress, dataSize)

        sequence += 1
        buf.storeBytes(of: sequence, toByteOffset: 0, as: Int64.self)
        buf.storeBytes(of: Int32(width), toByteOffset: 8, as: Int32.self)
        buf.storeBytes(of: Int32(height), toByteOffset: 12, as: Int32.self)
        buf.storeBytes(of: Int32(bytesPerRow), toByteOffset: 16, as: Int32.self)
        buf.storeBytes(of: Int32(0), toByteOffset: 20, as: Int32.self) // format=BGRA
        buf.storeBytes(of: Int32(1), toByteOffset: 24, as: Int32.self) // ready=1

        // Swap front/back and grab callback — brief lock
        lock.lock()
        frontIndex = backIdx
        let cb = callback
        lock.unlock()

        cb?(Int32(cameraId))
    }

    deinit {
        buffers.0?.deallocate()
        buffers.1?.deallocate()
    }
}

/// Manages a single camera session — AVCaptureSession lifecycle, preview texture,
/// photo capture, video recording, and image streaming.
///
/// One CameraSession instance exists per active camera (identified by cameraId).
class CameraSession: NSObject {
    let cameraId: Int
    private(set) var textureId: Int64 = -1

    private let config: CameraConfig
    private var captureSession: AVCaptureSession?
    private var videoDevice: AVCaptureDevice?
    private var videoOutput: AVCaptureVideoDataOutput?
    private var audioOutput: AVCaptureAudioDataOutput?
    private var texture: CameraTexture?
    private weak var textureRegistry: FlutterTextureRegistry?
    private weak var methodChannel: FlutterMethodChannel?

    private let captureQueue = DispatchQueue(label: "com.hugocornellier.camera_desktop.capture")
    private let audioQueue = DispatchQueue(label: "com.hugocornellier.camera_desktop.audio")
    private let sessionQueue = DispatchQueue(label: "com.hugocornellier.camera_desktop.session")
    private let bufferLock = UnfairLock()
    private let flagsLock = UnfairLock()

    private var lastTextureNotification: CFTimeInterval = 0
    private let textureNotificationInterval: CFTimeInterval = 1.0 / 120.0

    private var recordHandler = RecordHandler()
    private let imageStreamFFI = ImageStreamFFI()
    private var _previewPaused = false
    private var _imageStreaming = false
    private var latestBuffer: CVPixelBuffer?

    private var previewPaused: Bool {
        get { flagsLock.lock(); defer { flagsLock.unlock() }; return _previewPaused }
        set { flagsLock.lock(); _previewPaused = newValue; flagsLock.unlock() }
    }

    private var imageStreaming: Bool {
        get { flagsLock.lock(); defer { flagsLock.unlock() }; return _imageStreaming }
        set { flagsLock.lock(); _imageStreaming = newValue; flagsLock.unlock() }
    }

    private var actualWidth: Int = 0
    private var actualHeight: Int = 0
    private var firstFrameReceived = false

    /// Pending initialization result callback — called when the first frame arrives.
    private var pendingInitResult: FlutterResult?

    struct CameraConfig {
        let deviceId: String
        let resolutionPreset: Int
        let enableAudio: Bool
    }

    init(cameraId: Int, config: CameraConfig,
         textureRegistry: FlutterTextureRegistry,
         methodChannel: FlutterMethodChannel) {
        self.cameraId = cameraId
        self.config = config
        self.textureRegistry = textureRegistry
        self.methodChannel = methodChannel
        super.init()
    }

    // MARK: - Texture Registration

    /// Registers a FlutterTexture and returns the texture ID.
    func registerTexture() -> Int64 {
        let tex = CameraTexture()
        texture = tex
        guard let registry = textureRegistry else { return -1 }
        textureId = registry.register(tex)
        return textureId
    }

    // MARK: - Initialization

    /// Initializes the AVCaptureSession. Responds asynchronously when the first frame arrives.
    func initialize(result: @escaping FlutterResult) {
        AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
            guard let self = self else { return }
            if !granted {
                DispatchQueue.main.async {
                    result(FlutterError(code: "permission_denied",
                                        message: "Camera permission was denied",
                                        details: nil))
                }
                return
            }
            self.sessionQueue.async {
                self.setupSession(result: result)
            }
        }
    }

    private func setupSession(result: @escaping FlutterResult) {
        let session = AVCaptureSession()
        let preset = DeviceEnumerator.sessionPreset(for: config.resolutionPreset)
        if session.canSetSessionPreset(preset) {
            session.sessionPreset = preset
        } else {
            session.sessionPreset = .high
        }

        // Find the video device.
        let devices = AVCaptureDevice.captureDevices(mediaType: .video)
        guard let device = devices.first(where: { $0.uniqueID == config.deviceId })
                ?? devices.first else {
            DispatchQueue.main.async {
                result(FlutterError(code: "no_camera",
                                    message: "No camera device found for ID: \(self.config.deviceId)",
                                    details: nil))
            }
            return
        }
        videoDevice = device

        // Configure device.
        do {
            try device.lockForConfiguration()
            if device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusMode = .continuousAutoFocus
            }
            if device.isExposureModeSupported(.continuousAutoExposure) {
                device.exposureMode = .continuousAutoExposure
            }
            device.unlockForConfiguration()
        } catch {
            // Non-fatal — continue with default settings.
        }

        // Add video input.
        do {
            let videoInput = try AVCaptureDeviceInput(device: device)
            if session.canAddInput(videoInput) {
                session.addInput(videoInput)
            }
        } catch {
            let message = error.localizedDescription
            DispatchQueue.main.async {
                result(FlutterError(code: "input_failed",
                                    message: "Failed to create video input: \(message)",
                                    details: nil))
            }
            return
        }

        // Add audio input if enabled.
        if config.enableAudio {
            let audioDevices = AVCaptureDevice.captureDevices(mediaType: .audio)
            if let audioDevice = audioDevices.first {
                do {
                    let audioInput = try AVCaptureDeviceInput(device: audioDevice)
                    if session.canAddInput(audioInput) {
                        session.addInput(audioInput)
                    }
                } catch {
                    // Non-fatal — continue without audio.
                }
            }
        }

        // Add video output.
        let vOutput = AVCaptureVideoDataOutput()
        vOutput.alwaysDiscardsLateVideoFrames = true
        vOutput.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
        ]
        vOutput.setSampleBufferDelegate(self, queue: captureQueue)
        if session.canAddOutput(vOutput) {
            session.addOutput(vOutput)
        }
        videoOutput = vOutput

        // Mirror at the capture source so all consumers get mirrored frames.
        if let connection = vOutput.connection(with: .video),
           connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = true
        }

        // Add audio output if enabled.
        if config.enableAudio {
            let aOutput = AVCaptureAudioDataOutput()
            aOutput.setSampleBufferDelegate(self, queue: audioQueue)
            if session.canAddOutput(aOutput) {
                session.addOutput(aOutput)
            }
            audioOutput = aOutput
        }

        captureSession = session
        pendingInitResult = result
        firstFrameReceived = false

        // Start running — the first frame callback will respond to the pending result.
        session.startRunning()

        // Timeout: if no frame arrives in 8 seconds, fail.
        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) { [weak self] in
            guard let self = self, let pending = self.pendingInitResult else { return }
            self.pendingInitResult = nil
            pending(FlutterError(code: "initialization_timeout",
                                 message: "Camera initialization timed out — no frames received",
                                 details: nil))
        }
    }

    // MARK: - Photo Capture

    func takePicture(result: @escaping FlutterResult) {
        bufferLock.lock()
        let buffer = latestBuffer
        bufferLock.unlock()

        guard let buffer = buffer else {
            result(FlutterError(code: "no_frame",
                                message: "No frame available for capture",
                                details: nil))
            return
        }

        let path = PhotoHandler.generatePath(cameraId: cameraId)
        sessionQueue.async {
            let success = PhotoHandler.takePicture(from: buffer, outputPath: path)
            DispatchQueue.main.async {
                if success {
                    result(path)
                } else {
                    result(FlutterError(code: "capture_failed",
                                        message: "Failed to write JPEG to disk",
                                        details: nil))
                }
            }
        }
    }

    // MARK: - Video Recording

    func startVideoRecording(result: @escaping FlutterResult) {
        let enableAudio = config.enableAudio

        sessionQueue.async { [self] in
            do {
                let path = try self.recordHandler.startRecording(
                    width: self.actualWidth,
                    height: self.actualHeight,
                    enableAudio: enableAudio
                )
                _ = path
                DispatchQueue.main.async { result(nil) }
            } catch {
                let message = error.localizedDescription
                DispatchQueue.main.async {
                    result(FlutterError(code: "recording_failed",
                                        message: "Failed to start recording: \(message)",
                                        details: nil))
                }
            }
        }
    }

    func stopVideoRecording(result: @escaping FlutterResult) {
        guard recordHandler.isRecording else {
            result(FlutterError(code: "not_recording",
                                message: "No recording in progress",
                                details: nil))
            return
        }

        recordHandler.stopRecording { path in
            DispatchQueue.main.async {
                if let path = path {
                    result(path)
                } else {
                    result(FlutterError(code: "recording_failed",
                                        message: "Failed to finalize recording",
                                        details: nil))
                }
            }
        }
    }

    // MARK: - Image Streaming

    func startImageStream() {
        imageStreaming = true
    }

    func stopImageStream() {
        imageStreaming = false
    }

    // MARK: - FFI Image Stream Access

    func getImageStreamBufferPointer() -> UnsafeMutableRawPointer? {
        return imageStreamFFI.getBufferPointer()
    }

    func registerImageStreamCallback(_ callback: @convention(c) (Int32) -> Void) {
        imageStreamFFI.registerCallback(callback)
    }

    func unregisterImageStreamCallback() {
        imageStreamFFI.unregisterCallback()
    }

    // MARK: - Preview Control

    func pausePreview() {
        previewPaused = true
    }

    func resumePreview() {
        previewPaused = false
    }

    // MARK: - Mirror Control

    /// Toggles horizontal mirroring on the live video output connection.
    /// Can be called while the session is running — no restart needed.
    func setMirror(mirrored: Bool) {
        sessionQueue.async { [self] in
            guard let connection = self.videoOutput?.connection(with: .video),
                  connection.isVideoMirroringSupported else {
                return
            }
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = mirrored
        }
    }

    // MARK: - Disposal

    func dispose() {
        sessionQueue.async { [self] in
            self.recordHandler.stopRecording { _ in }
            self.captureSession?.stopRunning()
            self.captureSession = nil
            self.videoDevice = nil
            self.videoOutput = nil
            self.audioOutput = nil

            DispatchQueue.main.async {
                if self.texture != nil, let registry = self.textureRegistry {
                    registry.unregisterTexture(self.textureId)
                }
                self.texture = nil
                self.methodChannel?.invokeMethod("cameraClosing", arguments: ["cameraId": self.cameraId])
            }
        }
    }
}

// MARK: - AVCaptureVideoDataOutputSampleBufferDelegate & AVCaptureAudioDataOutputSampleBufferDelegate

extension CameraSession: AVCaptureVideoDataOutputSampleBufferDelegate,
                          AVCaptureAudioDataOutputSampleBufferDelegate {

    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {

        // Route audio buffers to the record handler.
        if output == audioOutput {
            recordHandler.appendAudioBuffer(sampleBuffer)
            return
        }

        // Video frame handling.
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)

        // Store the latest buffer for photo capture.
        bufferLock.lock()
        latestBuffer = pixelBuffer
        bufferLock.unlock()

        // Handle first-frame initialization response.
        let isFirstFrame = !firstFrameReceived
        if isFirstFrame {
            firstFrameReceived = true
            actualWidth = width
            actualHeight = height

            DispatchQueue.main.async { [weak self] in
                guard let self = self, let pending = self.pendingInitResult else { return }
                self.pendingInitResult = nil
                pending([
                    "previewWidth": Double(width),
                    "previewHeight": Double(height),
                ])
            }
        }

        // Update the texture for Flutter preview.
        if !previewPaused || isFirstFrame {
            texture?.update(buffer: pixelBuffer)
            let now = CACurrentMediaTime()
            if isFirstFrame || (now - lastTextureNotification) >= textureNotificationInterval {
                lastTextureNotification = now
                DispatchQueue.main.async { [weak self] in
                    guard let self = self, let registry = self.textureRegistry else { return }
                    registry.textureFrameAvailable(self.textureId)
                }
            }
        }

        // Append to recording if active.
        recordHandler.appendVideoBuffer(sampleBuffer)

        // Send frame to Dart image stream if active.
        if imageStreaming {
            if imageStreamFFI.hasCallback {
                imageStreamFFI.writeFrame(pixelBuffer: pixelBuffer, cameraId: cameraId)
            } else {
                CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
                defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
                guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
                let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
                let dataSize = bytesPerRow * height
                let data = Data(bytes: baseAddress, count: dataSize)
                DispatchQueue.main.async { [weak self] in
                    guard let self = self else { return }
                    self.methodChannel?.invokeMethod("imageStreamFrame", arguments: [
                        "cameraId": self.cameraId,
                        "width": width,
                        "height": height,
                        "bytes": FlutterStandardTypedData(bytes: data),
                    ])
                }
            }
        }
    }
}
