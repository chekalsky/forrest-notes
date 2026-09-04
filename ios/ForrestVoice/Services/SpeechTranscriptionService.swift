import AVFoundation
import Foundation
import NaturalLanguage
import Speech

enum SpeechTranscriptionError: LocalizedError {
    case notAuthorized
    case recognizerUnavailable
    case emptyResult
    case timeout
    case chunkFailed

    var errorDescription: String? {
        switch self {
        case .notAuthorized: "Speech recognition not authorized"
        case .recognizerUnavailable: "Speech recognizer unavailable"
        case .emptyResult: "No speech detected"
        case .timeout: "Speech recognition timed out"
        case .chunkFailed: "Could not split audio for transcription"
        }
    }
}

final class SpeechTranscriptionService {
    static let shared = SpeechTranscriptionService()

    /// Apple Speech tasks effectively cap around one minute; stay under that.
    private let chunkMaxSeconds: TimeInterval = 50
    private let recognitionTimeout: TimeInterval = 45
    private let totalTimeout: TimeInterval = 180

    private init() {}

    func authorizationStatus() -> SFSpeechRecognizerAuthorizationStatus {
        SFSpeechRecognizer.authorizationStatus()
    }

    func requestAuthorization() async -> SFSpeechRecognizerAuthorizationStatus {
        await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { status in
                continuation.resume(returning: status)
            }
        }
    }

    /// Transcribe a local WAV/audio file using Apple Speech.
    func transcribe(url: URL) async throws -> String {
        try await withTimeout(seconds: totalTimeout) {
            try await self.transcribeUnbounded(url: url)
        }
    }

    private func transcribeUnbounded(url: URL) async throws -> String {
        let status = authorizationStatus()
        guard status == .authorized else {
            await AppLog.shared.log("speech", "authorization status=\(status.rawValue)")
            throw SpeechTranscriptionError.notAuthorized
        }

        let language = await MainActor.run { TranscriptionSettings.shared.language }
        let duration = try audioDurationSeconds(url: url)
        await AppLog.shared.log(
            "speech",
            "File \(url.lastPathComponent) duration=\(String(format: "%.1f", duration))s mode=\(language.label)"
        )

        let chunkURLs = try makeChunkURLs(source: url, maxSeconds: chunkMaxSeconds)
        defer { cleanupTempChunks(chunkURLs, source: url) }
        if chunkURLs.count > 1 {
            await AppLog.shared.log("speech", "Split into \(chunkURLs.count) chunks (max \(Int(chunkMaxSeconds))s each)")
        }

        var parts: [String] = []
        parts.reserveCapacity(chunkURLs.count)

        let locale: Locale
        var startIndex = 0

        switch language {
        case .auto:
            let (picked, firstText) = try await detectLocaleAndFirstChunk(url: chunkURLs[0])
            locale = picked
            if let firstText {
                parts.append(firstText)
                startIndex = 1
            }
        case .system:
            locale = Locale.current
        case .russian:
            locale = Locale(identifier: "ru-RU")
        case .english:
            locale = Locale(identifier: "en-US")
        }

        guard let recognizer = SFSpeechRecognizer(locale: locale), recognizer.isAvailable else {
            await AppLog.shared.log("speech", "recognizer unavailable for \(locale.identifier)")
            throw SpeechTranscriptionError.recognizerUnavailable
        }

        await AppLog.shared.log(
            "speech",
            "Using locale=\(locale.identifier) onDevice=\(recognizer.supportsOnDeviceRecognition)"
        )

        for index in startIndex..<chunkURLs.count {
            let chunkURL = chunkURLs[index]
            await AppLog.shared.log("speech", "Chunk \(index + 1)/\(chunkURLs.count) \(chunkURL.lastPathComponent)")
            let text = try await recognizeOnce(url: chunkURL, locale: locale, forceOnDevice: false)
            if !text.isEmpty {
                parts.append(text)
            }
        }

        let joined = parts.joined(separator: " ").trimmingCharacters(in: .whitespacesAndNewlines)
        if joined.isEmpty {
            throw SpeechTranscriptionError.emptyResult
        }
        return joined
    }

    /// Auto: try Russian + system language only (2 attempts max on sample chunk).
    private func detectLocaleAndFirstChunk(url: URL) async throws -> (Locale, String?) {
        let candidates = autoCandidateLocales()
        var best: (locale: Locale, text: String, score: Double)?

        for locale in candidates {
            guard let recognizer = SFSpeechRecognizer(locale: locale), recognizer.isAvailable else {
                continue
            }
            do {
                let text = try await recognizeOnce(url: url, locale: locale, forceOnDevice: false)
                let score = scoreTranscript(text, for: locale)
                await AppLog.shared.log(
                    "speech",
                    "Auto try \(locale.identifier) chars=\(text.count) score=\(String(format: "%.0f", score))"
                )
                if best == nil || score > best!.score {
                    best = (locale, text, score)
                }
            } catch {
                await AppLog.shared.log("speech", "Auto try \(locale.identifier) failed: \(error.localizedDescription)")
            }
        }

        guard let best else {
            throw SpeechTranscriptionError.recognizerUnavailable
        }

        await AppLog.shared.log("speech", "Auto picked locale=\(best.locale.identifier)")
        return (best.locale, best.text)
    }

    private func autoCandidateLocales() -> [Locale] {
        var locales: [Locale] = [Locale(identifier: "ru-RU"), Locale.current]
        var seen = Set<String>()
        return locales.filter { locale in
            seen.insert(locale.identifier).inserted
        }
    }

    private func scoreTranscript(_ text: String, for locale: Locale) -> Double {
        let lengthScore = Double(text.count)
        let expected = locale.language.languageCode?.identifier ?? String(locale.identifier.prefix(2)).lowercased()

        let languageRecognizer = NLLanguageRecognizer()
        languageRecognizer.processString(text)
        let dominant = languageRecognizer.dominantLanguage?.rawValue.lowercased() ?? ""

        let langBonus: Double
        if dominant.hasPrefix(expected) || expected.hasPrefix(String(dominant.prefix(2))) {
            langBonus = 1000
        } else if !dominant.isEmpty {
            langBonus = 0
        } else {
            langBonus = 100
        }

        return langBonus + lengthScore
    }

    private func recognizeOnce(url: URL, locale: Locale, forceOnDevice: Bool) async throws -> String {
        guard let recognizer = SFSpeechRecognizer(locale: locale), recognizer.isAvailable else {
            throw SpeechTranscriptionError.recognizerUnavailable
        }

        let request = SFSpeechURLRecognitionRequest(url: url)
        request.shouldReportPartialResults = false
        request.requiresOnDeviceRecognition = forceOnDevice && recognizer.supportsOnDeviceRecognition
        if #available(iOS 16.0, *) {
            request.addsPunctuation = true
        }

        await AppLog.shared.log(
            "speech",
            "recognize locale=\(locale.identifier) onDevice=\(request.requiresOnDeviceRecognition)"
        )

        return try await withCheckedThrowingContinuation { continuation in
            let lock = NSLock()
            var finished = false

            func finish(_ result: Result<String, Error>) {
                lock.lock()
                defer { lock.unlock() }
                guard !finished else { return }
                finished = true
                continuation.resume(with: result)
            }

            var recognitionTask: SFSpeechRecognitionTask?
            recognitionTask = recognizer.recognitionTask(with: request) { result, error in
                if Task.isCancelled {
                    finish(.failure(SpeechTranscriptionError.timeout))
                    return
                }
                if let error {
                    finish(.failure(error))
                    return
                }
                guard let result else { return }
                if result.isFinal {
                    let text = result.bestTranscription.formattedString
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    if text.isEmpty {
                        finish(.failure(SpeechTranscriptionError.emptyResult))
                    } else {
                        finish(.success(text))
                    }
                }
            }

            DispatchQueue.global().asyncAfter(deadline: .now() + recognitionTimeout) { [recognitionTask] in
                lock.lock()
                let done = finished
                lock.unlock()
                if !done {
                    recognitionTask?.cancel()
                    finish(.failure(SpeechTranscriptionError.timeout))
                }
            }
        }
    }

    private func withTimeout<T>(
        seconds: TimeInterval,
        operation: @escaping () async throws -> T
    ) async throws -> T {
        try await withThrowingTaskGroup(of: T.self) { group in
            group.addTask {
                try await operation()
            }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                throw SpeechTranscriptionError.timeout
            }
            guard let result = try await group.next() else {
                throw SpeechTranscriptionError.timeout
            }
            group.cancelAll()
            return result
        }
    }

    private func audioDurationSeconds(url: URL) throws -> TimeInterval {
        let file = try AVAudioFile(forReading: url)
        return Double(file.length) / file.processingFormat.sampleRate
    }

    private func makeChunkURLs(source: URL, maxSeconds: TimeInterval) throws -> [URL] {
        let file = try AVAudioFile(forReading: source)
        let format = file.processingFormat
        let framesPerChunk = AVAudioFrameCount(maxSeconds * format.sampleRate)
        let totalFrames = file.length

        if totalFrames <= AVAudioFramePosition(framesPerChunk) {
            return [source]
        }

        let tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("forrest-speech", isDirectory: true)
        try FileManager.default.createDirectory(at: tempDir, withIntermediateDirectories: true)

        var urls: [URL] = []
        var offset: AVAudioFramePosition = 0
        var chunkIndex = 0

        while offset < totalFrames {
            let remaining = totalFrames - offset
            let framesToRead = min(AVAudioFrameCount(remaining), framesPerChunk)
            file.framePosition = offset

            guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: framesToRead) else {
                throw SpeechTranscriptionError.chunkFailed
            }
            try file.read(into: buffer, frameCount: framesToRead)

            let chunkURL = tempDir.appendingPathComponent(
                "\(source.deletingPathExtension().lastPathComponent)_chunk\(chunkIndex).wav"
            )
            let chunkFile = try AVAudioFile(forWriting: chunkURL, settings: format.settings)
            try chunkFile.write(from: buffer)

            urls.append(chunkURL)
            offset += AVAudioFramePosition(framesToRead)
            chunkIndex += 1
        }

        return urls
    }

    private func cleanupTempChunks(_ chunkURLs: [URL], source: URL) {
        for url in chunkURLs where url != source {
            try? FileManager.default.removeItem(at: url)
        }
    }
}
