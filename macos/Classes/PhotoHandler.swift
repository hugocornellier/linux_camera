import AVFoundation
import CoreImage
import AppKit

/// Captures a still image from a CVPixelBuffer and writes it to a JPEG file.
class PhotoHandler {
    private static let ciContext = CIContext()

    /// Takes a picture from the given pixel buffer and writes a JPEG to the output path.
    /// Returns true on success, false on failure.
    static func takePicture(from buffer: CVPixelBuffer, outputPath: String) -> Bool {
        let ciImage = CIImage(cvPixelBuffer: buffer)

        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)

        guard let cgImage = ciContext.createCGImage(ciImage,
                                                     from: CGRect(x: 0, y: 0,
                                                                  width: width,
                                                                  height: height)) else {
            return false
        }

        let bitmapRep = NSBitmapImageRep(cgImage: cgImage)
        guard let jpegData = bitmapRep.representation(using: .jpeg,
                                                       properties: [.compressionFactor: 0.9]) else {
            return false
        }

        let url = URL(fileURLWithPath: outputPath)
        do {
            try jpegData.write(to: url)
            return true
        } catch {
            return false
        }
    }

    /// Generates a unique temporary file path for a captured image.
    static func generatePath(cameraId: Int) -> String {
        let timestamp = Int(Date().timeIntervalSince1970 * 1000)
        return NSTemporaryDirectory() + "camera_desktop_\(cameraId)_\(timestamp).jpg"
    }
}
