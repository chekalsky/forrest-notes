import CoreBluetooth
import Foundation

/// Thread-safe ACK writer (off the CoreBluetooth callback thread).
final class TransferAckSender {
    private let lock = NSLock()
    private weak var peripheral: CBPeripheral?
    private var controlChar: CBCharacteristic?
    private let writeQueue = DispatchQueue(label: "com.forrest.voice.ble-writes")

    func configure(peripheral: CBPeripheral, control: CBCharacteristic) {
        lock.lock()
        self.peripheral = peripheral
        self.controlChar = control
        lock.unlock()
    }

    func send(recordingId: UInt16, seq: UInt16) {
        lock.lock()
        let peripheral = self.peripheral
        let controlChar = self.controlChar
        lock.unlock()
        guard let peripheral, let controlChar else {
            Task { @MainActor in
                AppLog.shared.log("ble", "ACK dropped — control char not ready seq=\(seq)")
            }
            return
        }

        var payload = Data(count: 5)
        payload[0] = ForrestVoiceProtocol.cmdAckChunk
        payload[1] = UInt8(recordingId & 0xFF)
        payload[2] = UInt8(recordingId >> 8)
        payload[3] = UInt8(seq & 0xFF)
        payload[4] = UInt8(seq >> 8)

        writeQueue.async {
            peripheral.writeValue(payload, for: controlChar, type: .withoutResponse)
        }
    }

    func sendRetryPending() {
        lock.lock()
        let peripheral = self.peripheral
        let controlChar = self.controlChar
        lock.unlock()
        guard let peripheral, let controlChar else {
            Task { @MainActor in
                AppLog.shared.log("ble", "RETRY_PENDING dropped — control char not ready")
            }
            return
        }

        var payload = Data(count: 1)
        payload[0] = ForrestVoiceProtocol.cmdRetryPending

        writeQueue.async {
            peripheral.writeValue(payload, for: controlChar, type: .withoutResponse)
        }
    }
}
