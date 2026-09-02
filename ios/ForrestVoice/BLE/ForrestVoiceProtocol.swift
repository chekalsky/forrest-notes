import CoreBluetooth
import Foundation

enum ForrestVoiceProtocol {
    static let serviceUUID = CBUUID(string: "6E4000F0-B5A3-F393-E0A9-E50E24DCCA9E")
    static let deviceInfoUUID = CBUUID(string: "6E4000F1-B5A3-F393-E0A9-E50E24DCCA9E")
    static let deviceStatusUUID = CBUUID(string: "6E4000F2-B5A3-F393-E0A9-E50E24DCCA9E")
    static let controlUUID = CBUUID(string: "6E4000F3-B5A3-F393-E0A9-E50E24DCCA9E")
    static let recordingMetaUUID = CBUUID(string: "6E4000F4-B5A3-F393-E0A9-E50E24DCCA9E")
    static let audioDataUUID = CBUUID(string: "6E4000F5-B5A3-F393-E0A9-E50E24DCCA9E")
    static let resultTextUUID = CBUUID(string: "6E4000F6-B5A3-F393-E0A9-E50E24DCCA9E")
    static let protocolVerUUID = CBUUID(string: "6E4000F7-B5A3-F393-E0A9-E50E24DCCA9E")

    static let restoreIdentifier = "com.forrest.voice.central"
    static let protocolVersion: UInt16 = 0x0001

    static let msgAudioChunk: UInt8 = 0x01
    static let msgRecordingMeta: UInt8 = 0x10
    static let flagLastChunk: UInt8 = 0x01

    static let cmdAckChunk: UInt8 = 0x01
    static let cmdXferComplete: UInt8 = 0x02
    static let cmdRetryPending: UInt8 = 0x04

    static let ackEveryChunks = 16
}

struct DeviceStatusPayload {
    let state: DeviceState
    let batteryPercent: Int?
    let pendingCount: Int
    let statusSeq: UInt8

    static func parse(_ data: Data) -> DeviceStatusPayload? {
        guard data.count >= 4, let state = DeviceState(rawValue: data[0]) else { return nil }
        let battRaw = data[1]
        let batteryPercent = battRaw == 255 ? nil : Int(battRaw)
        return DeviceStatusPayload(
            state: state,
            batteryPercent: batteryPercent,
            pendingCount: Int(data[2]),
            statusSeq: data[3]
        )
    }
}

enum DeviceState: UInt8, CustomStringConvertible {
    case idle = 0
    case recording = 1
    case finalizing = 2
    case transferring = 3
    case waiting = 4
    case success = 5
    case error = 6

    var description: String {
        switch self {
        case .idle: "Idle"
        case .recording: "Recording"
        case .finalizing: "Finalizing"
        case .transferring: "Transferring"
        case .waiting: "Waiting"
        case .success: "Success"
        case .error: "Error"
        }
    }
}

struct RecordingMeta {
    let recordingId: UInt16
    let totalBytes: UInt32
    let sampleRate: UInt16
    let channels: UInt8
    let bitsPerSample: UInt8

    static func parse(_ data: Data) -> RecordingMeta? {
        guard data.count >= 16, data[0] == ForrestVoiceProtocol.msgRecordingMeta else { return nil }
        let recordingId = data.u16LE(at: 2)
        let totalBytes = data.u32LE(at: 4)
        let sampleRate = data.u16LE(at: 8)
        let channels = data[10]
        let bits = data[11]
        return RecordingMeta(
            recordingId: recordingId,
            totalBytes: totalBytes,
            sampleRate: sampleRate,
            channels: channels,
            bitsPerSample: bits
        )
    }
}

struct AudioChunk {
    let recordingId: UInt16
    let seq: UInt16
    let isLast: Bool
    let payload: Data

    static func parse(_ data: Data) -> AudioChunk? {
        guard data.count >= 8, data[0] == ForrestVoiceProtocol.msgAudioChunk else { return nil }
        let flags = data[1]
        let recordingId = data.u16LE(at: 2)
        let seq = data.u16LE(at: 4)
        let payloadLen = Int(data.u16LE(at: 6))
        guard data.count >= 8 + payloadLen else { return nil }
        let payload = data.subdata(in: 8 ..< 8 + payloadLen)
        return AudioChunk(
            recordingId: recordingId,
            seq: seq,
            isLast: (flags & ForrestVoiceProtocol.flagLastChunk) != 0,
            payload: payload
        )
    }
}

extension Data {
    func u16LE(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func u32LE(at offset: Int) -> UInt32 {
        UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
