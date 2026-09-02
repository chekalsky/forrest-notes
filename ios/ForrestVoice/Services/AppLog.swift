import Foundation
import os

/// Central log sink: in-app list, os.Logger, and append-only file.
@MainActor
final class AppLog: ObservableObject {
    static let shared = AppLog()

    @Published private(set) var lines: [String] = []

    private let logger = Logger(subsystem: "com.forrest.voice", category: "app")
    private let fileURL: URL
    private let maxLines = 500

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let dir = docs.appendingPathComponent("Logs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        fileURL = dir.appendingPathComponent("forrest-voice.log")
    }

    func log(_ category: String, _ message: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
        let line = "[\(stamp)] [\(category)] \(message)"
        lines.insert(line, at: 0)
        if lines.count > maxLines { lines.removeLast() }

        logger.info("[\(category, privacy: .public)] \(message, privacy: .public)")
        appendToFile(line + "\n")
    }

    private func appendToFile(_ text: String) {
        guard let data = text.data(using: .utf8) else { return }
        if FileManager.default.fileExists(atPath: fileURL.path) {
            if let handle = try? FileHandle(forWritingTo: fileURL) {
                try? handle.seekToEnd()
                try? handle.write(contentsOf: data)
                try? handle.close()
            }
        } else {
            try? data.write(to: fileURL)
        }
    }

    /// Snapshot for Share/AirDrop — prefers on-disk log, falls back to in-memory lines.
    func urlForSharing() -> URL {
        let stamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
        let exportURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("forrest-voice-\(stamp).log")

        if FileManager.default.fileExists(atPath: fileURL.path),
           let disk = try? String(contentsOf: fileURL, encoding: .utf8),
           !disk.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            try? disk.write(to: exportURL, atomically: true, encoding: .utf8)
            return exportURL
        }

        let body = lines.reversed().joined(separator: "\n")
        let text = body.isEmpty ? "(no log entries yet)\n" : body + "\n"
        try? text.write(to: exportURL, atomically: true, encoding: .utf8)
        return exportURL
    }

    var logFilePath: String { fileURL.path }

    var logFileURL: URL { fileURL }

    func clear() {
        lines.removeAll()
        try? FileManager.default.removeItem(at: fileURL)
    }
}
