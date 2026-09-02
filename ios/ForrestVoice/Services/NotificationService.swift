import Foundation
import UserNotifications

@MainActor
final class NotificationService: NSObject, UNUserNotificationCenterDelegate {
    static let shared = NotificationService()

    private override init() {
        super.init()
    }

    func configure() {
        UNUserNotificationCenter.current().delegate = self
    }

    func requestAuthorization() async -> Bool {
        do {
            return try await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge])
        } catch {
            return false
        }
    }

    /// Local notification when a recording file lands (works in background after BLE wake).
    func notifyRecordingReceived(filename: String, byteCount: Int, recordingId: UInt16) async {
        let content = UNMutableNotificationContent()
        content.title = "New voice recording"
        content.body = "\(filename) (\(byteCount / 1024) KB) from device #\(recordingId)"
        content.sound = .default
        content.userInfo = [
            "type": "recording_received",
            "filename": filename,
            "recordingId": recordingId,
        ]

        let request = UNNotificationRequest(
            identifier: "rec-\(recordingId)-\(UUID().uuidString)",
            content: content,
            trigger: nil // deliver immediately
        )

        do {
            try await UNUserNotificationCenter.current().add(request)
            AppLog.shared.log("notify", "Scheduled recording notification id=\(recordingId)")
        } catch {
            AppLog.shared.log("notify", "Recording notification failed: \(error.localizedDescription)")
        }
    }

    /// Separate notification after Apple Speech finishes.
    func notifyTranscription(text: String, recordingId: UInt16) async {
        let content = UNMutableNotificationContent()
        content.title = "Transcription"
        let preview = text.count > 220 ? String(text.prefix(217)) + "…" : text
        content.body = preview
        content.sound = .default
        content.userInfo = [
            "type": "transcription",
            "recordingId": recordingId,
            "fullText": text,
        ]

        let request = UNNotificationRequest(
            identifier: "tx-\(recordingId)-\(UUID().uuidString)",
            content: content,
            trigger: nil
        )

        do {
            try await UNUserNotificationCenter.current().add(request)
        } catch {
            AppLog.shared.log("notify", "Transcription notification failed: \(error.localizedDescription)")
        }
    }

    func notifyTranscriptionFailed(message: String, recordingId: UInt16) async {
        let content = UNMutableNotificationContent()
        content.title = "Transcription failed"
        content.body = message
        content.sound = .default
        content.userInfo = [
            "type": "transcription_error",
            "recordingId": recordingId,
        ]

        let request = UNNotificationRequest(
            identifier: "tx-err-\(recordingId)-\(UUID().uuidString)",
            content: content,
            trigger: nil
        )

        do {
            try await UNUserNotificationCenter.current().add(request)
        } catch {
            AppLog.shared.log("notify", "Error notification failed: \(error.localizedDescription)")
        }
    }

    // Show notifications even when app is foreground
    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification
    ) async -> UNNotificationPresentationOptions {
        [.banner, .sound, .badge]
    }
}
