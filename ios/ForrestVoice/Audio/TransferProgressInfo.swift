import Foundation

struct TransferProgressInfo: Equatable {
    enum Phase: String, Equatable {
        case idle
        case receiving
        case complete
        case failed
    }

    var phase: Phase = .idle
    var recordingId: UInt16 = 0
    var receivedBytes: UInt32 = 0
    var expectedBytes: UInt32 = 0
    var chunkSeq: Int = -1
    var errorMessage: String?

    static let idle = TransferProgressInfo()

    var fraction: Double {
        guard expectedBytes > 0 else { return 0 }
        return min(1, Double(receivedBytes) / Double(expectedBytes))
    }

    var percentText: String {
        String(format: "%.0f%%", fraction * 100)
    }

    var bytesText: String {
        let kb = 1024.0
        let recv = Double(receivedBytes) / kb
        let total = Double(expectedBytes) / kb
        if expectedBytes == 0 {
            return "—"
        }
        if total >= 1024 {
            return String(format: "%.1f / %.1f MB", recv / kb, total / kb)
        }
        return String(format: "%.0f / %.0f KB", recv, total)
    }

    var statusLine: String {
        switch phase {
        case .idle:
            "Ready"
        case .receiving:
            if receivedBytes == 0 {
                "Starting transfer #\(recordingId)…"
            } else {
                "Receiving #\(recordingId) · chunk \(chunkSeq + 1)"
            }
        case .complete:
            "Received #\(recordingId)"
        case .failed:
            errorMessage ?? "Transfer failed"
        }
    }
}
