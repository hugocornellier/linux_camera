import AVFoundation

/// Manages video recording via AVAssetWriter.
class RecordHandler: NSObject {
    private var assetWriter: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?
    private var outputPath: String?
    private var sessionStarted = false

    private(set) var isRecording = false

    /// Starts recording to a temporary file.
    /// - Parameters:
    ///   - width: Video frame width.
    ///   - height: Video frame height.
    ///   - enableAudio: Whether to record audio.
    /// - Returns: The output file path on success.
    /// - Throws: If the asset writer cannot be created.
    func startRecording(width: Int, height: Int, enableAudio: Bool) throws -> String {
        guard !isRecording else {
            throw NSError(domain: "camera_desktop", code: -1,
                          userInfo: [NSLocalizedDescriptionKey: "Already recording"])
        }

        let path = RecordHandler.generatePath()
        let url = URL(fileURLWithPath: path)

        // Remove any stale file at this path.
        try? FileManager.default.removeItem(at: url)

        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)

        // Video input — H.264 encoding.
        let videoSettings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: width,
            AVVideoHeightKey: height,
        ]
        let vInput = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        vInput.expectsMediaDataInRealTime = true
        if writer.canAdd(vInput) {
            writer.add(vInput)
        }
        videoInput = vInput

        // Audio input — AAC encoding.
        if enableAudio {
            let audioSettings: [String: Any] = [
                AVFormatIDKey: kAudioFormatMPEG4AAC,
                AVSampleRateKey: 44100,
                AVNumberOfChannelsKey: 2,
                AVEncoderBitRateKey: 128000,
            ]
            let aInput = AVAssetWriterInput(mediaType: .audio, outputSettings: audioSettings)
            aInput.expectsMediaDataInRealTime = true
            if writer.canAdd(aInput) {
                writer.add(aInput)
            }
            audioInput = aInput
        } else {
            audioInput = nil
        }

        writer.startWriting()
        assetWriter = writer
        outputPath = path
        sessionStarted = false
        isRecording = true

        return path
    }

    /// Appends a video sample buffer to the recording.
    func appendVideoBuffer(_ sampleBuffer: CMSampleBuffer) {
        guard isRecording,
              let writer = assetWriter,
              writer.status == .writing,
              let input = videoInput,
              input.isReadyForMoreMediaData else { return }

        if !sessionStarted {
            let timestamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            writer.startSession(atSourceTime: timestamp)
            sessionStarted = true
        }

        input.append(sampleBuffer)
    }

    /// Appends an audio sample buffer to the recording.
    func appendAudioBuffer(_ sampleBuffer: CMSampleBuffer) {
        guard isRecording,
              let writer = assetWriter,
              writer.status == .writing,
              let input = audioInput,
              input.isReadyForMoreMediaData,
              sessionStarted else { return }

        input.append(sampleBuffer)
    }

    /// Stops recording and finalizes the file.
    /// - Parameter completion: Called with the output file path on success, or nil on failure.
    func stopRecording(completion: @escaping (String?) -> Void) {
        guard isRecording, let writer = assetWriter else {
            completion(nil)
            return
        }

        isRecording = false
        videoInput?.markAsFinished()
        audioInput?.markAsFinished()

        let path = outputPath
        writer.finishWriting {
            if writer.status == .completed {
                completion(path)
            } else {
                completion(nil)
            }
        }

        assetWriter = nil
        videoInput = nil
        audioInput = nil
        outputPath = nil
        sessionStarted = false
    }

    /// Generates a unique temporary file path for a video recording.
    static func generatePath() -> String {
        let timestamp = Int(Date().timeIntervalSince1970 * 1000)
        return NSTemporaryDirectory() + "camera_desktop_video_\(timestamp).mp4"
    }
}
