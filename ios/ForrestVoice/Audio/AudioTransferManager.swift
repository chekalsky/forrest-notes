import Foundation

/// Reassembles AudioData notifications into a single file on disk.
final class AudioTransferManager: ObservableObject {
    struct ActiveTransfer {
        let recordingId: UInt16
        let destinationURL: URL
        let expectedBytes: UInt32
        var receivedBytes: UInt32 = 0
        var highestSeq: Int = -1
        var fileHandle: FileHandle?
    }

    @Published private(set) var activeRecordingId: UInt16?
    @Published private(set) var progress: Double = 0
    @Published private(set) var lastCompletedURL: URL?

    private var active: ActiveTransfer?
    private let store: RecordingStore
    private let queue = DispatchQueue(label: "com.forrest.voice.transfer", qos: .userInitiated)

    var onTransferComplete: ((URL, UInt16) -> Void)?
    var onProgress: ((TransferProgressInfo) -> Void)?

    init(store: RecordingStore) {
        self.store = store
    }

    func begin(meta: RecordingMeta) throws {
        try queue.sync {
            try beginLocked(meta: meta)
        }
        let info = TransferProgressInfo(
            phase: .receiving,
            recordingId: meta.recordingId,
            receivedBytes: 0,
            expectedBytes: meta.totalBytes,
            chunkSeq: -1
        )
        DispatchQueue.main.async { [weak self] in
            self?.progress = 0
            self?.activeRecordingId = meta.recordingId
            self?.onProgress?(info)
        }
        Task { @MainActor in
            AppLog.shared.log("transfer", "Begin id=\(meta.recordingId) expectedBytes=\(meta.totalBytes)")
        }
    }

    /// Called directly from CoreBluetooth — keep off the main thread.
    @discardableResult
    func ingest(chunk: AudioChunk) throws -> Bool {
        var completed: (URL, UInt16)?
        var progressInfo: TransferProgressInfo?
        var shouldAck = false

        try queue.sync {
            guard var transfer = active else { throw TransferError.noActiveTransfer }
            guard chunk.recordingId == transfer.recordingId else { throw TransferError.recordingIdMismatch }

            if Int(chunk.seq) <= transfer.highestSeq {
                return
            }
            if transfer.highestSeq >= 0 && Int(chunk.seq) != transfer.highestSeq + 1 {
                throw TransferError.seqGap(expected: transfer.highestSeq + 1, got: Int(chunk.seq))
            }

            try transfer.fileHandle?.write(contentsOf: chunk.payload)
            transfer.receivedBytes += UInt32(chunk.payload.count)
            transfer.highestSeq = Int(chunk.seq)
            active = transfer

            if transfer.expectedBytes > 0 {
                progressInfo = TransferProgressInfo(
                    phase: .receiving,
                    recordingId: transfer.recordingId,
                    receivedBytes: transfer.receivedBytes,
                    expectedBytes: transfer.expectedBytes,
                    chunkSeq: Int(chunk.seq)
                )
            }

            if chunk.isLast {
                if transfer.receivedBytes != transfer.expectedBytes {
                    throw TransferError.incomplete(received: transfer.receivedBytes, expected: transfer.expectedBytes)
                }
                completed = try finishLocked(transfer: transfer)
            }

            shouldAck = (Int(chunk.seq) + 1) % ForrestVoiceProtocol.ackEveryChunks == 0 || chunk.isLast
        }

        if let progressInfo {
            DispatchQueue.main.async { [weak self] in
                self?.progress = progressInfo.fraction
                self?.onProgress?(progressInfo)
            }
        }

        if let (url, id) = completed {
            let done = TransferProgressInfo(
                phase: .complete,
                recordingId: id,
                receivedBytes: progressInfo?.expectedBytes ?? 0,
                expectedBytes: progressInfo?.expectedBytes ?? 0,
                chunkSeq: progressInfo?.chunkSeq ?? 0
            )
            DispatchQueue.main.async { [weak self] in
                self?.activeRecordingId = nil
                self?.lastCompletedURL = url
                self?.progress = 1
                self?.onProgress?(done)
                AppLog.shared.log("transfer", "Complete id=\(id) path=\(url.lastPathComponent)")
                self?.onTransferComplete?(url, id)
            }
        }

        return shouldAck
    }

    func cancelActive() {
        queue.sync {
            if let transfer = active {
                try? transfer.fileHandle?.close()
                try? FileManager.default.removeItem(at: transfer.destinationURL)
            }
            active = nil
            DispatchQueue.main.async { [weak self] in
                self?.activeRecordingId = nil
                self?.progress = 0
                self?.onProgress?(.idle)
            }
        }
    }

    func reportFailure(recordingId: UInt16, message: String) {
        let info = TransferProgressInfo(
            phase: .failed,
            recordingId: recordingId,
            errorMessage: message
        )
        DispatchQueue.main.async { [weak self] in
            self?.activeRecordingId = nil
            self?.progress = 0
            self?.onProgress?(info)
        }
    }

    private func beginLocked(meta: RecordingMeta) throws {
        cancelActiveLocked()
        let url = store.urlForIncoming(recordingId: meta.recordingId)
        FileManager.default.createFile(atPath: url.path, contents: nil)
        let handle = try FileHandle(forWritingTo: url)
        active = ActiveTransfer(
            recordingId: meta.recordingId,
            destinationURL: url,
            expectedBytes: meta.totalBytes,
            fileHandle: handle
        )
    }

    private func finishLocked(transfer: ActiveTransfer) throws -> (URL, UInt16) {
        try transfer.fileHandle?.close()
        let finalURL = try store.finalizeIncoming(
            tempURL: transfer.destinationURL,
            recordingId: transfer.recordingId
        )
        active = nil
        DispatchQueue.main.async { [weak self] in
            self?.progress = 1
        }
        return (finalURL, transfer.recordingId)
    }

    private func cancelActiveLocked() {
        if let transfer = active {
            try? transfer.fileHandle?.close()
            try? FileManager.default.removeItem(at: transfer.destinationURL)
        }
        active = nil
    }

    enum TransferError: Error {
        case noActiveTransfer
        case recordingIdMismatch
        case seqGap(expected: Int, got: Int)
        case incomplete(received: UInt32, expected: UInt32)
    }
}
