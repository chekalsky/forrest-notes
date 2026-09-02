import Foundation
import UIKit

/// Post-transfer pipeline: notify → Apple Speech → save transcript → second notification.
@MainActor
final class RecordingProcessor {
    static let shared = RecordingProcessor()

    private let store = RecordingStore()

    var onFinished: (() -> Void)?

    private init() {}

    func process(recordingId: UInt16, url: URL, filename: String, byteCount: Int) async {
        AppLog.shared.log("transfer", "File saved id=\(recordingId) path=\(filename) bytes=\(byteCount)")

        await NotificationService.shared.notifyRecordingReceived(
            filename: filename,
            byteCount: byteCount,
            recordingId: recordingId
        )
        AppLog.shared.log("notify", "Sent recording notification id=\(recordingId)")

        AppLog.shared.log("speech", "Starting Apple Speech for \(filename)")
        let started = Date()

        do {
            let transcript = try await SpeechTranscriptionService.shared.transcribe(url: url)
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Done in \(String(format: "%.1f", elapsed))s chars=\(transcript.count)")
            AppLog.shared.log("speech", "Transcript: \(transcript)")

            try store.saveTranscript(recordingId: recordingId, text: transcript)
            AppLog.shared.log("store", "Saved transcript for id=\(recordingId)")

            await forwardToOpenClaw(transcript: transcript, recordingId: recordingId)

            await NotificationService.shared.notifyTranscription(
                text: transcript,
                recordingId: recordingId
            )
            AppLog.shared.log("notify", "Sent transcription notification id=\(recordingId)")
        } catch {
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Failed after \(String(format: "%.1f", elapsed))s: \(error.localizedDescription)")

            try? store.saveTranscript(recordingId: recordingId, text: nil, error: error.localizedDescription)

            await NotificationService.shared.notifyTranscriptionFailed(
                message: error.localizedDescription,
                recordingId: recordingId
            )
            AppLog.shared.log("notify", "Sent transcription error notification id=\(recordingId)")
        }

        onFinished?()
    }

    private func forwardToOpenClaw(transcript: String, recordingId: UInt16) async {
        guard OpenClawSettings.shared.isConfigured else {
            AppLog.shared.log("openclaw", "Skipped id=\(recordingId) — configure gateway in Settings")
            return
        }

        do {
            try await OpenClawService.shared.postAgentHook(
                message: transcript,
                name: "Forrest Voice #\(recordingId)"
            )
            try store.saveOpenClawStatus(recordingId: recordingId, status: "Sent to OpenClaw")
            AppLog.shared.log("openclaw", "Dispatched id=\(recordingId)")
        } catch {
            try? store.saveOpenClawStatus(recordingId: recordingId, status: "Error: \(error.localizedDescription)")
            AppLog.shared.log("openclaw", "Failed id=\(recordingId): \(error.localizedDescription)")
        }
    }
}
