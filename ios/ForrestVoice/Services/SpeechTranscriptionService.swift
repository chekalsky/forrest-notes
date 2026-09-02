import Foundation
import Speech

enum SpeechTranscriptionError: LocalizedError {
    case notAuthorized
    case recognizerUnavailable
    case emptyResult

    var errorDescription: String? {
        switch self {
        case .notAuthorized: "Speech recognition not authorized"
        case .recognizerUnavailable: "Speech recognizer unavailable"
        case .emptyResult: "No speech detected"
        }
    }
}

@MainActor
final class SpeechTranscriptionService {
    static let shared = SpeechTranscriptionService()

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

    /// Transcribe a local WAV/audio file using Apple Speech (on-device when available).
    func transcribe(url: URL, locale: Locale = .current) async throws -> String {
        let status = authorizationStatus()
        guard status == .authorized else {
            AppLog.shared.log("speech", "authorization status=\(status.rawValue)")
            throw SpeechTranscriptionError.notAuthorized
        }

        guard let recognizer = SFSpeechRecognizer(locale: locale), recognizer.isAvailable else {
            AppLog.shared.log("speech", "recognizer unavailable for \(locale.identifier)")
            throw SpeechTranscriptionError.recognizerUnavailable
        }

        AppLog.shared.log("speech", "recognizer locale=\(locale.identifier) onDevice=\(recognizer.supportsOnDeviceRecognition)")

        let request = SFSpeechURLRecognitionRequest(url: url)
        request.shouldReportPartialResults = false
        if recognizer.supportsOnDeviceRecognition {
            request.requiresOnDeviceRecognition = true
        }

        return try await withCheckedThrowingContinuation { continuation in
            var finished = false
            recognizer.recognitionTask(with: request) { result, error in
                if finished { return }

                if let error {
                    finished = true
                    continuation.resume(throwing: error)
                    return
                }

                guard let result else { return }

                if result.isFinal {
                    finished = true
                    let text = result.bestTranscription.formattedString
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    if text.isEmpty {
                        continuation.resume(throwing: SpeechTranscriptionError.emptyResult)
                    } else {
                        continuation.resume(returning: text)
                    }
                }
            }
        }
    }
}
