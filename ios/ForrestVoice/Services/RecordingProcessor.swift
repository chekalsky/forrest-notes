import Foundation
import UIKit

/// Post-transfer pipeline: notify → Apple Speech → save transcript → second notification.
@MainActor
final class RecordingProcessor: ObservableObject {
    static let shared = RecordingProcessor()

    @Published private(set) var activeRecordingId: UInt16?

    private let store = RecordingStore()
    private var pipeline: Task<Void, Never>?
    private var retranscribeTask: Task<Void, Never>?

    var onRecordingsChanged: (() -> Void)?

    private init() {}

    func process(recordingId: UInt16, url: URL, filename: String, byteCount: Int) async {
        await pipeline?.value
        let task = Task {
            await self.processWork(recordingId: recordingId, url: url, filename: filename, byteCount: byteCount)
        }
        pipeline = task
        await task.value
    }

    func retranscribe(_ recording: SavedRecording) {
        retranscribeTask?.cancel()
        try? store.saveTranscript(recordingId: recording.recordingId, text: nil, error: nil)
        onRecordingsChanged?()

        retranscribeTask = Task {
            await self.runRetranscribe(recording)
        }
    }

    private func runRetranscribe(_ recording: SavedRecording) async {
        activeRecordingId = recording.recordingId
        defer {
            activeRecordingId = nil
            onRecordingsChanged?()
        }

        AppLog.shared.log("speech", "Retranscribe start id=\(recording.recordingId) \(recording.filename)")
        let started = Date()

        do {
            let transcript = try await SpeechTranscriptionService.shared.transcribe(url: recording.url)
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Retranscribe done in \(String(format: "%.1f", elapsed))s chars=\(transcript.count)")

            try store.saveTranscript(recordingId: recording.recordingId, text: transcript)
            onRecordingsChanged?()
        } catch is CancellationError {
            AppLog.shared.log("speech", "Retranscribe cancelled id=\(recording.recordingId)")
        } catch {
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Retranscribe failed after \(String(format: "%.1f", elapsed))s: \(error.localizedDescription)")
            try? store.saveTranscript(
                recordingId: recording.recordingId,
                text: nil,
                error: error.localizedDescription
            )
            onRecordingsChanged?()
        }
    }

    private func processWork(recordingId: UInt16, url: URL, filename: String, byteCount: Int) async {
        AppLog.shared.log("transfer", "File saved id=\(recordingId) path=\(filename) bytes=\(byteCount)")

        await NotificationService.shared.notifyRecordingReceived(
            filename: filename,
            byteCount: byteCount,
            recordingId: recordingId
        )
        AppLog.shared.log("notify", "Sent recording notification id=\(recordingId)")

        await transcribeRecording(
            SavedRecording(
                id: UUID(),
                recordingId: recordingId,
                filename: filename,
                byteCount: byteCount,
                receivedAt: Date(),
                transcript: nil,
                transcribedAt: nil,
                transcriptionError: nil,
                openClawStatus: nil
            ),
            notifyOnStart: true
        )
    }

    private func transcribeRecording(_ recording: SavedRecording, notifyOnStart: Bool) async {
        activeRecordingId = recording.recordingId
        defer {
            activeRecordingId = nil
            onRecordingsChanged?()
        }

        let url = recording.url
        AppLog.shared.log("speech", "Starting Apple Speech for \(recording.filename)")
        let started = Date()

        do {
            let transcript = try await SpeechTranscriptionService.shared.transcribe(url: url)
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Done in \(String(format: "%.1f", elapsed))s chars=\(transcript.count)")
            AppLog.shared.log("speech", "Transcript: \(transcript)")

            try store.saveTranscript(recordingId: recording.recordingId, text: transcript)
            AppLog.shared.log("store", "Saved transcript for id=\(recording.recordingId)")
            onRecordingsChanged?()

            await forwardToOpenClaw(transcript: transcript, recordingId: recording.recordingId)
            onRecordingsChanged?()

            await NotificationService.shared.notifyTranscription(
                text: transcript,
                recordingId: recording.recordingId
            )
            AppLog.shared.log("notify", "Sent transcription notification id=\(recording.recordingId)")
        } catch {
            let elapsed = Date().timeIntervalSince(started)
            AppLog.shared.log("speech", "Failed after \(String(format: "%.1f", elapsed))s: \(error.localizedDescription)")

            try? store.saveTranscript(
                recordingId: recording.recordingId,
                text: nil,
                error: error.localizedDescription
            )
            onRecordingsChanged?()

            if notifyOnStart {
                await NotificationService.shared.notifyTranscriptionFailed(
                    message: error.localizedDescription,
                    recordingId: recording.recordingId
                )
                AppLog.shared.log("notify", "Sent transcription error notification id=\(recording.recordingId)")
            }
        }
    }

    private func forwardToOpenClaw(transcript: String, recordingId: UInt16) async {
        let settings = OpenClawSettings.shared
        guard settings.isConfigured else {
            let missing = settings.configurationGaps.joined(separator: ", ")
            let bg = UIApplication.shared.applicationState != .active
            let status = "Skipped: missing \(missing)"
            AppLog.shared.log(
                "openclaw",
                "Skipped id=\(recordingId) — \(status) background=\(bg)"
            )
            try? store.saveOpenClawStatus(recordingId: recordingId, status: status)
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
