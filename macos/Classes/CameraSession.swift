import AVFoundation
import FlutterMacOS

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

    private var recordHandler = RecordHandler()
    private var previewPaused = false
    private var imageStreaming = false
    private var latestBuffer: CVPixelBuffer?

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
            DispatchQueue.main.async {
                guard let self = self else { return }
                if !granted {
                    result(FlutterError(code: "permission_denied",
                                        message: "Camera permission was denied",
                                        details: nil))
                    return
                }
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
            result(FlutterError(code: "no_camera",
                                message: "No camera device found for ID: \(config.deviceId)",
                                details: nil))
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
            result(FlutterError(code: "input_failed",
                                message: "Failed to create video input: \(error.localizedDescription)",
                                details: nil))
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
        guard let buffer = latestBuffer else {
            result(FlutterError(code: "no_frame",
                                message: "No frame available for capture",
                                details: nil))
            return
        }

        let path = PhotoHandler.generatePath(cameraId: cameraId)
        if PhotoHandler.takePicture(from: buffer, outputPath: path) {
            result(path)
        } else {
            result(FlutterError(code: "capture_failed",
                                message: "Failed to write JPEG to disk",
                                details: nil))
        }
    }

    // MARK: - Video Recording

    func startVideoRecording(result: @escaping FlutterResult) {
        do {
            let path = try recordHandler.startRecording(
                width: actualWidth,
                height: actualHeight,
                enableAudio: config.enableAudio
            )
            _ = path // Recording started, path stored internally.
            result(nil)
        } catch {
            result(FlutterError(code: "recording_failed",
                                message: "Failed to start recording: \(error.localizedDescription)",
                                details: nil))
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

    // MARK: - Preview Control

    func pausePreview() {
        previewPaused = true
    }

    func resumePreview() {
        previewPaused = false
    }

    // MARK: - Disposal

    func dispose() {
        if recordHandler.isRecording {
            recordHandler.stopRecording { _ in }
        }

        captureSession?.stopRunning()
        captureSession = nil
        videoDevice = nil
        videoOutput = nil
        audioOutput = nil

        if texture != nil, let registry = textureRegistry {
            registry.unregisterTexture(textureId)
        }
        texture = nil

        // Send closing event to Dart.
        methodChannel?.invokeMethod("cameraClosing", arguments: ["cameraId": cameraId])
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
        latestBuffer = pixelBuffer

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
            DispatchQueue.main.async { [weak self] in
                guard let self = self, let registry = self.textureRegistry else { return }
                registry.textureFrameAvailable(self.textureId)
            }
        }

        // Append to recording if active.
        recordHandler.appendVideoBuffer(sampleBuffer)

        // Send frame to Dart image stream if active.
        if imageStreaming {
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
