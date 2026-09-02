import CoreBluetooth
import Foundation
import Combine

@MainActor
final class BluetoothManager: NSObject, ObservableObject {
    @Published private(set) var bluetoothState: CBManagerState = .unknown
    @Published private(set) var isScanning = false
    @Published private(set) var connectedName: String?
    @Published private(set) var deviceState: DeviceState?
    @Published private(set) var devicePendingCount: Int = 0
    @Published private(set) var lastDeviceStatus: Date?
    @Published private(set) var transferInfo: TransferProgressInfo = .idle
    @Published private(set) var recordingsRevision: Int = 0

    var logs: [String] { AppLog.shared.lines }

    private var logCancellable: AnyCancellable?
    private var completeResetTask: Task<Void, Never>?
    private var central: CBCentralManager!
    private let recordingStore = RecordingStore()
    private let transferManager: AudioTransferManager
    private var session: DeviceSession?
    private var connectedPeripheral: CBPeripheral?
    private var knownPeripheralID: UUID?
    private var reconnectTask: Task<Void, Never>?

    override init() {
        transferManager = AudioTransferManager(store: recordingStore)
        super.init()
        transferManager.onProgress = { [weak self] info in
            Task { @MainActor in
                self?.completeResetTask?.cancel()
                self?.transferInfo = info
                if info.phase == .complete {
                    self?.completeResetTask = Task {
                        try? await Task.sleep(for: .seconds(3))
                        guard !Task.isCancelled else { return }
                        await MainActor.run {
                            if self?.transferInfo.phase == .complete {
                                self?.transferInfo = .idle
                            }
                        }
                    }
                }
            }
        }
        logCancellable = AppLog.shared.objectWillChange.sink { [weak self] _ in
            self?.objectWillChange.send()
        }
        RecordingProcessor.shared.onFinished = { [weak self] in
            Task { @MainActor in self?.recordingsRevision += 1 }
        }
        central = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey: ForrestVoiceProtocol.restoreIdentifier,
                CBCentralManagerOptionShowPowerAlertKey: true,
            ]
        )
    }

    var recordings: [SavedRecording] {
        _ = recordingsRevision
        return recordingStore.loadIndex()
    }

    private func attachSessionCallbacks(_ deviceSession: DeviceSession) {
        deviceSession.onLog = { [weak self] msg in self?.appendLog(msg) }
        deviceSession.onStatusUpdate = { [weak self] payload in
            self?.deviceState = payload.state
            self?.devicePendingCount = payload.pendingCount
            self?.lastDeviceStatus = Date()
        }
        deviceSession.onRecordingReceived = { [weak self] _, _ in
            Task { @MainActor in
                self?.recordingsRevision += 1
            }
        }
    }

    func startScanning() {
        guard central.state == .poweredOn, connectedPeripheral == nil else { return }
        reconnectTask?.cancel()
        central.scanForPeripherals(
            withServices: [ForrestVoiceProtocol.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        isScanning = true
        appendLog("Scanning for Forrest Voice…")
    }

    private func scheduleReconnect(after seconds: TimeInterval = 2) {
        reconnectTask?.cancel()
        reconnectTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(seconds))
            guard !Task.isCancelled, connectedPeripheral == nil else { return }
            if let id = knownPeripheralID,
               let peripheral = central.retrievePeripherals(withIdentifiers: [id]).first {
                appendLog("Reconnecting to known device…")
                connect(peripheral)
            } else {
                startScanning()
            }
        }
    }

    func stopScanning() {
        central.stopScan()
        isScanning = false
    }

    func disconnect() {
        if let peripheral = connectedPeripheral {
            central.cancelPeripheralConnection(peripheral)
        }
    }

    func eraseAllLogsAndRecordings() throws {
        try recordingStore.deleteAll()
        AppLog.shared.clear()
        recordingsRevision += 1
        AppLog.shared.log("app", "Erased all logs and recordings")
    }

    private func appendLog(_ line: String) {
        AppLog.shared.log("ble", line)
    }

    private func connect(_ peripheral: CBPeripheral) {
        guard connectedPeripheral == nil else { return }
        reconnectTask?.cancel()
        stopScanning()
        connectedPeripheral = peripheral
        knownPeripheralID = peripheral.identifier
        connectedName = peripheral.name ?? peripheral.identifier.uuidString
        appendLog("Connecting to \(connectedName ?? "?")…")

        let deviceSession = DeviceSession(peripheral: peripheral, transferManager: transferManager)
        attachSessionCallbacks(deviceSession)
        session = deviceSession
        central.connect(peripheral, options: nil)
    }
}

extension BluetoothManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            bluetoothState = central.state
            appendLog("Bluetooth state: \(central.state.rawValue)")
            if central.state == .poweredOn, connectedPeripheral == nil {
                startScanning()
            }
        }
    }

    /// Called when iOS relaunches the app in background to restore BLE state.
    nonisolated func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        Task { @MainActor in
            appendLog("Restoring BLE state (background relaunch)")
            if let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] {
                for peripheral in peripherals {
                    knownPeripheralID = peripheral.identifier
                    connectedPeripheral = peripheral
                    connectedName = peripheral.name
                    let deviceSession = DeviceSession(peripheral: peripheral, transferManager: transferManager)
                    attachSessionCallbacks(deviceSession)
                    session = deviceSession
                    peripheral.delegate = deviceSession
                    if peripheral.state == .connected {
                        appendLog("Restored connected peripheral")
                        deviceSession.discoverServices()
                    } else {
                        central.connect(peripheral, options: nil)
                    }
                }
            }
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        Task { @MainActor in
            guard connectedPeripheral == nil else { return }
            let name = peripheral.name
                ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
                ?? "?"
            appendLog("Discovered \(name) rssi=\(RSSI)")
            connect(peripheral)
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            appendLog("Connected")
            connectedName = peripheral.name ?? peripheral.identifier.uuidString
            knownPeripheralID = peripheral.identifier
            session?.discoverServices()
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            appendLog("Disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
            connectedPeripheral = nil
            connectedName = nil
            session = nil
            deviceState = nil
            devicePendingCount = 0
            lastDeviceStatus = nil
            transferInfo = .idle
            completeResetTask?.cancel()
            scheduleReconnect()
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        Task { @MainActor in
            appendLog("Connect failed: \(error?.localizedDescription ?? "?")")
            connectedPeripheral = nil
            scheduleReconnect(after: 3)
        }
    }
}
