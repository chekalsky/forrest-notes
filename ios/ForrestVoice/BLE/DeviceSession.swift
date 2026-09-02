import CoreBluetooth
import Foundation
import UIKit

@MainActor
final class DeviceSession: NSObject {
    let peripheral: CBPeripheral

    private var deviceStatusChar: CBCharacteristic?
    private var recordingMetaChar: CBCharacteristic?
    private var audioDataChar: CBCharacteristic?
    private var controlChar: CBCharacteristic?

    private let transferManager: AudioTransferManager
    private let ackSender = TransferAckSender()

    var onStatusUpdate: ((DeviceStatusPayload) -> Void)?
    var onLog: ((String) -> Void)?
    var onRecordingReceived: ((URL, UInt16) -> Void)?

    init(peripheral: CBPeripheral, transferManager: AudioTransferManager) {
        self.peripheral = peripheral
        self.transferManager = transferManager
        super.init()
        peripheral.delegate = self
        transferManager.onTransferComplete = { [weak self] url, id in
            self?.handleTransferComplete(url: url, recordingId: id)
        }
    }

    func discoverServices() {
        peripheral.discoverServices([ForrestVoiceProtocol.serviceUUID])
    }

    private func log(_ message: String) {
        AppLog.shared.log("ble", message)
        onLog?(message)
    }

    private func subscribeIfNeeded(_ characteristic: CBCharacteristic) {
        guard !characteristic.isNotifying else { return }
        peripheral.setNotifyValue(true, for: characteristic)
    }

    private func sendAck(recordingId: UInt16, seq: UInt16) {
        ackSender.send(recordingId: recordingId, seq: seq)
    }

    private func handleTransferComplete(url: URL, recordingId: UInt16) {
        let filename = url.lastPathComponent
        let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0
        onRecordingReceived?(url, recordingId)

        Task { @MainActor in
            var bgTask: UIBackgroundTaskIdentifier = .invalid
            bgTask = UIApplication.shared.beginBackgroundTask {
                UIApplication.shared.endBackgroundTask(bgTask)
            }

            await RecordingProcessor.shared.process(
                recordingId: recordingId,
                url: url,
                filename: filename,
                byteCount: size
            )

            if bgTask != .invalid {
                UIApplication.shared.endBackgroundTask(bgTask)
            }
        }
    }
}

extension DeviceSession: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            if let error {
                log("discoverServices error: \(error.localizedDescription)")
                return
            }
            guard let services = peripheral.services else { return }
            for service in services where service.uuid == ForrestVoiceProtocol.serviceUUID {
                peripheral.discoverCharacteristics(nil, for: service)
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        Task { @MainActor in
            if let error {
                log("discoverCharacteristics error: \(error.localizedDescription)")
                return
            }
            guard let chars = service.characteristics else { return }
            for char in chars {
                switch char.uuid {
                case ForrestVoiceProtocol.deviceStatusUUID:
                    deviceStatusChar = char
                    subscribeIfNeeded(char)
                case ForrestVoiceProtocol.recordingMetaUUID:
                    recordingMetaChar = char
                    subscribeIfNeeded(char)
                case ForrestVoiceProtocol.audioDataUUID:
                    audioDataChar = char
                    subscribeIfNeeded(char)
                case ForrestVoiceProtocol.controlUUID:
                    controlChar = char
                    ackSender.configure(peripheral: peripheral, control: char)
                default:
                    break
                }
            }
            log("Subscribed to Forrest Voice characteristics")
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            Task { @MainActor in log("notify error: \(error.localizedDescription)") }
            return
        }
        guard let data = characteristic.value else { return }

        if characteristic.uuid == ForrestVoiceProtocol.deviceStatusUUID {
            guard let payload = DeviceStatusPayload.parse(data) else { return }
            if payload.pendingCount > 0, payload.state != .transferring {
                ackSender.sendRetryPending()
            }
            Task { @MainActor in
                onStatusUpdate?(payload)
            }
            return
        }

        if characteristic.uuid == ForrestVoiceProtocol.recordingMetaUUID {
            guard let meta = RecordingMeta.parse(data) else {
                Task { @MainActor in log("bad RecordingMeta") }
                return
            }
            do {
                try transferManager.begin(meta: meta)
                Task { @MainActor in
                    log("RecordingMeta id=\(meta.recordingId) bytes=\(meta.totalBytes)")
                }
            } catch {
                Task { @MainActor in log("begin transfer failed: \(error)") }
            }
            return
        }

        if characteristic.uuid == ForrestVoiceProtocol.audioDataUUID {
            guard let chunk = AudioChunk.parse(data) else { return }
            do {
                let shouldAck = try transferManager.ingest(chunk: chunk)
                if shouldAck {
                    ackSender.send(recordingId: chunk.recordingId, seq: chunk.seq)
                }
                if chunk.isLast {
                    Task { @MainActor in log("Transfer complete seq=\(chunk.seq)") }
                }
            } catch {
                transferManager.reportFailure(
                    recordingId: chunk.recordingId,
                    message: String(describing: error)
                )
                Task { @MainActor in
                    log("chunk ingest failed seq=\(chunk.seq) id=\(chunk.recordingId): \(error)")
                }
            }
            return
        }
    }
}
