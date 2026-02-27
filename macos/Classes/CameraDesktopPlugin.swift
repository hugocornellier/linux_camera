import FlutterMacOS
import AVFoundation

/// Flutter plugin entry point for camera_desktop on macOS.
///
/// Routes MethodChannel calls to the appropriate CameraSession instance.
/// Speaks the exact same protocol as the Linux native side so the shared
/// Dart CameraDesktopPlugin class works on both platforms.
public class CameraDesktopPlugin: NSObject, FlutterPlugin {
    private static var _sharedInstance: CameraDesktopPlugin?
    @objc public class var sharedInstance: CameraDesktopPlugin? {
        get { _sharedInstance }
        set { _sharedInstance = newValue }
    }

    private var sessions: [Int: CameraSession] = [:]
    private var nextCameraId = 1
    private let textureRegistry: FlutterTextureRegistry
    private let methodChannel: FlutterMethodChannel

    init(textureRegistry: FlutterTextureRegistry, methodChannel: FlutterMethodChannel) {
        self.textureRegistry = textureRegistry
        self.methodChannel = methodChannel
        super.init()
    }

    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(
            name: "plugins.flutter.io/camera_desktop",
            binaryMessenger: registrar.messenger
        )
        let instance = CameraDesktopPlugin(
            textureRegistry: registrar.textures,
            methodChannel: channel
        )
        registrar.addMethodCallDelegate(instance, channel: channel)
        CameraDesktopPlugin.sharedInstance = instance
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "availableCameras":
            handleAvailableCameras(result: result)
        case "create":
            handleCreate(call: call, result: result)
        case "initialize":
            handleInitialize(call: call, result: result)
        case "takePicture":
            handleTakePicture(call: call, result: result)
        case "startVideoRecording":
            handleStartVideoRecording(call: call, result: result)
        case "stopVideoRecording":
            handleStopVideoRecording(call: call, result: result)
        case "startImageStream":
            handleStartImageStream(call: call, result: result)
        case "stopImageStream":
            handleStopImageStream(call: call, result: result)
        case "pausePreview":
            handlePausePreview(call: call, result: result)
        case "resumePreview":
            handleResumePreview(call: call, result: result)
        case "setMirror":
            handleSetMirror(call: call, result: result)
        case "dispose":
            handleDispose(call: call, result: result)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // MARK: - Method Handlers

    private func handleAvailableCameras(result: @escaping FlutterResult) {
        DispatchQueue.global(qos: .userInitiated).async {
            let devices = DeviceEnumerator.enumerateDevices()
            let list = devices.map { device -> [String: Any] in
                return [
                    "name": device.name,
                    "lensDirection": device.lensDirection,
                    "sensorOrientation": device.sensorOrientation,
                ]
            }
            DispatchQueue.main.async {
                result(list)
            }
        }
    }

    private func handleCreate(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any],
              let cameraName = args["cameraName"] as? String,
              let resolutionPreset = args["resolutionPreset"] as? Int else {
            result(FlutterError(code: "invalid_args",
                                message: "Missing required arguments for create",
                                details: nil))
            return
        }

        let enableAudio = args["enableAudio"] as? Bool ?? false

        // Extract device ID from camera name: "Friendly Name (deviceId)"
        guard let deviceId = DeviceEnumerator.extractDeviceId(from: cameraName) else {
            result(FlutterError(code: "invalid_camera_name",
                                message: "Could not extract device ID from camera name",
                                details: nil))
            return
        }

        let cameraId = nextCameraId
        nextCameraId += 1

        let config = CameraSession.CameraConfig(
            deviceId: deviceId,
            resolutionPreset: resolutionPreset,
            enableAudio: enableAudio
        )

        let session = CameraSession(
            cameraId: cameraId,
            config: config,
            textureRegistry: textureRegistry,
            methodChannel: methodChannel
        )

        let textureId = session.registerTexture()
        if textureId < 0 {
            result(FlutterError(code: "texture_registration_failed",
                                message: "Failed to register Flutter texture",
                                details: nil))
            return
        }

        sessions[cameraId] = session

        result([
            "cameraId": cameraId,
            "textureId": textureId,
        ])
    }

    private func handleInitialize(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.initialize(result: result)
    }

    private func handleTakePicture(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.takePicture(result: result)
    }

    private func handleStartVideoRecording(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.startVideoRecording(result: result)
    }

    private func handleStopVideoRecording(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.stopVideoRecording(result: result)
    }

    private func handleStartImageStream(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.startImageStream()
        result(nil)
    }

    private func handleStopImageStream(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.stopImageStream()
        result(nil)
    }

    private func handlePausePreview(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.pausePreview()
        result(nil)
    }

    private func handleResumePreview(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        session.resumePreview()
        result(nil)
    }

    private func handleSetMirror(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let session = findSession(call: call, result: result) else { return }
        guard let args = call.arguments as? [String: Any],
              let mirrored = args["mirrored"] as? Bool else {
            result(FlutterError(code: "invalid_args",
                                message: "Missing 'mirrored' argument",
                                details: nil))
            return
        }
        session.setMirror(mirrored: mirrored)
        result(nil)
    }

    private func handleDispose(call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any],
              let cameraId = args["cameraId"] as? Int else {
            result(nil)
            return
        }

        if let session = sessions.removeValue(forKey: cameraId) {
            session.dispose()
        }
        result(nil)
    }

    // MARK: - FFI Bridge

    @objc public func getImageStreamBuffer(forCamera cameraId: Int) -> UnsafeMutableRawPointer? {
        return sessions[cameraId]?.getImageStreamBufferPointer()
    }

    @objc public func registerImageStreamCallback(
        _ callback: @convention(c) (Int32) -> Void,
        forCamera cameraId: Int
    ) {
        sessions[cameraId]?.registerImageStreamCallback(callback)
    }

    @objc public func unregisterImageStreamCallback(forCamera cameraId: Int) {
        sessions[cameraId]?.unregisterImageStreamCallback()
    }

    // MARK: - Helpers

    private func findSession(call: FlutterMethodCall,
                              result: @escaping FlutterResult) -> CameraSession? {
        guard let args = call.arguments as? [String: Any],
              let cameraId = args["cameraId"] as? Int else {
            result(FlutterError(code: "invalid_args",
                                message: "Missing cameraId argument",
                                details: nil))
            return nil
        }

        guard let session = sessions[cameraId] else {
            result(FlutterError(code: "camera_not_found",
                                message: "No camera found with the given ID",
                                details: nil))
            return nil
        }

        return session
    }
}
