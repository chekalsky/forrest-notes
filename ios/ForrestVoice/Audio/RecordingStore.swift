import Foundation

struct SavedRecording: Identifiable, Codable {
    let id: UUID
    let recordingId: UInt16
    let filename: String
    let byteCount: Int
    let receivedAt: Date
    var transcript: String?
    var transcribedAt: Date?
    var transcriptionError: String?
    var openClawStatus: String?

    var url: URL { RecordingStore.recordingsDirectory.appendingPathComponent(filename) }
}

final class RecordingStore {
    static var recordingsDirectory: URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("Recordings", isDirectory: true)
    }

    private let indexURL: URL

    init() {
        indexURL = Self.recordingsDirectory.appendingPathComponent("index.json")
        try? FileManager.default.createDirectory(at: Self.recordingsDirectory, withIntermediateDirectories: true)
    }

    func urlForIncoming(recordingId: UInt16) -> URL {
        Self.recordingsDirectory.appendingPathComponent("incoming_\(recordingId).part")
    }

    func finalizeIncoming(tempURL: URL, recordingId: UInt16) throws -> URL {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let stamp = formatter.string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let filename = "rec_\(recordingId)_\(stamp).wav"
        let finalURL = Self.recordingsDirectory.appendingPathComponent(filename)
        if FileManager.default.fileExists(atPath: finalURL.path) {
            try FileManager.default.removeItem(at: finalURL)
        }
        try FileManager.default.moveItem(at: tempURL, to: finalURL)
        let attrs = try FileManager.default.attributesOfItem(atPath: finalURL.path)
        let size = (attrs[.size] as? Int) ?? 0
        var all = loadIndex()
        all.insert(
            SavedRecording(
                id: UUID(),
                recordingId: recordingId,
                filename: filename,
                byteCount: size,
                receivedAt: Date(),
                transcript: nil,
                transcribedAt: nil,
                transcriptionError: nil,
                openClawStatus: nil
            ),
            at: 0
        )
        try saveIndex(all)
        return finalURL
    }

    func saveTranscript(recordingId: UInt16, text: String?, error: String? = nil) throws {
        var all = loadIndex()
        guard let idx = all.firstIndex(where: { $0.recordingId == recordingId }) else {
            return
        }
        all[idx].transcript = text
        all[idx].transcribedAt = Date()
        all[idx].transcriptionError = error
        try saveIndex(all)
    }

    func saveOpenClawStatus(recordingId: UInt16, status: String) throws {
        var all = loadIndex()
        guard let idx = all.firstIndex(where: { $0.recordingId == recordingId }) else { return }
        all[idx].openClawStatus = status
        try saveIndex(all)
    }

    func loadIndex() -> [SavedRecording] {
        guard let data = try? Data(contentsOf: indexURL),
              let decoded = try? JSONDecoder().decode([SavedRecording].self, from: data)
        else { return [] }
        return decoded
    }

    private func saveIndex(_ items: [SavedRecording]) throws {
        let data = try JSONEncoder().encode(items)
        try data.write(to: indexURL, options: .atomic)
    }
}
