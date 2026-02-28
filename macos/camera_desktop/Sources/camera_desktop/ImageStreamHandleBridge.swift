import Foundation

private final class WeakCameraSession {
    weak var value: CameraSession?

    init(_ value: CameraSession) {
        self.value = value
    }
}

@objcMembers
public final class ImageStreamHandleBridge: NSObject {
    private static var nextHandle: Int64 = 1
    private static var sessionsByHandle: [Int64: WeakCameraSession] = [:]
    private static var cameraIdByHandle: [Int64: Int] = [:]
    private static let lock = UnfairLock()

    static func registerSession(_ session: CameraSession) -> Int64 {
        lock.lock()
        defer { lock.unlock() }
        let handle = nextHandle
        nextHandle += 1
        sessionsByHandle[handle] = WeakCameraSession(session)
        cameraIdByHandle[handle] = session.cameraId
        return handle
    }

    public static func releaseHandle(_ handle: Int64) {
        if handle == 0 { return }
        lock.lock()
        sessionsByHandle.removeValue(forKey: handle)
        cameraIdByHandle.removeValue(forKey: handle)
        lock.unlock()
    }

    static func releaseHandles(forCameraId cameraId: Int) {
        lock.lock()
        defer { lock.unlock() }
        let handlesToRemove = cameraIdByHandle.compactMap { entry in
            entry.value == cameraId ? entry.key : nil
        }
        for handle in handlesToRemove {
            sessionsByHandle.removeValue(forKey: handle)
            cameraIdByHandle.removeValue(forKey: handle)
        }
    }

    public static func getImageStreamBuffer(forHandle handle: Int64) -> UnsafeMutableRawPointer? {
        lock.lock()
        let session = sessionsByHandle[handle]?.value
        if session == nil {
            sessionsByHandle.removeValue(forKey: handle)
            cameraIdByHandle.removeValue(forKey: handle)
        }
        lock.unlock()
        return session?.getImageStreamBufferPointer()
    }

    public static func registerImageStreamCallback(
        _ callback: @convention(c) (Int32) -> Void,
        forHandle handle: Int64
    ) {
        lock.lock()
        let session = sessionsByHandle[handle]?.value
        if session == nil {
            sessionsByHandle.removeValue(forKey: handle)
            cameraIdByHandle.removeValue(forKey: handle)
        }
        lock.unlock()
        session?.registerImageStreamCallback(callback)
    }

    public static func unregisterImageStreamCallback(forHandle handle: Int64) {
        lock.lock()
        let session = sessionsByHandle[handle]?.value
        if session == nil {
            sessionsByHandle.removeValue(forKey: handle)
            cameraIdByHandle.removeValue(forKey: handle)
        }
        lock.unlock()
        session?.unregisterImageStreamCallback()
    }
}
