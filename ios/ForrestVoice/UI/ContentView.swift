import CoreBluetooth
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var bluetooth: BluetoothManager
    @ObservedObject private var appLog = AppLog.shared

    var body: some View {
        NavigationStack {
            List {
                Section("Connection") {
                    LabeledContent("Bluetooth") {
                        Text(stateLabel(bluetooth.bluetoothState))
                            .foregroundStyle(bluetooth.bluetoothState == .poweredOn ? .green : .secondary)
                    }
                    LabeledContent("Device") {
                        Text(bluetooth.connectedName ?? "Not connected")
                    }
                    if let deviceState = bluetooth.deviceState {
                        LabeledContent("Device state") {
                            Text(deviceState.description)
                        }
                    }
                    if bluetooth.devicePendingCount > 0 {
                        LabeledContent("Queued on device") {
                            Text("\(bluetooth.devicePendingCount)")
                        }
                    }
                    if let lastStatus = bluetooth.lastDeviceStatus {
                        LabeledContent("Last status") {
                            Text(lastStatus, style: .relative)
                                .foregroundStyle(.secondary)
                        }
                    }
                    if bluetooth.isScanning {
                        Text("Scanning…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                if bluetooth.transferInfo.phase != .idle || bluetooth.deviceState == .transferring {
                    Section("Transfer") {
                        TransferProgressView(
                            info: bluetooth.transferInfo,
                            deviceState: bluetooth.deviceState
                        )
                    }
                }

                Section("Recordings") {
                    let items = bluetooth.recordings
                    if items.isEmpty {
                        Text("No recordings yet")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(items) { rec in
                            VStack(alignment: .leading, spacing: 4) {
                                Text(rec.filename)
                                    .font(.body.monospaced())
                                Text("\(rec.byteCount / 1024) KB · \(rec.receivedAt.formatted())")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                if let transcript = rec.transcript, !transcript.isEmpty {
                                    Text(transcript)
                                        .font(.subheadline)
                                        .foregroundStyle(.primary)
                                } else if let err = rec.transcriptionError {
                                    Text(err)
                                        .font(.caption)
                                        .foregroundStyle(.red)
                                } else {
                                    Text("Transcribing…")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                if let oc = rec.openClawStatus {
                                    Text(oc)
                                        .font(.caption2)
                                        .foregroundStyle(oc.hasPrefix("Error") ? .red : .secondary)
                                }
                            }
                        }
                    }
                }

                Section {
                    ShareLink(
                        item: appLog.urlForSharing(),
                        preview: SharePreview("Forrest Voice log", image: Image(systemName: "doc.text"))
                    ) {
                        Label("Share log file", systemImage: "square.and.arrow.up")
                    }
                    Text("File: \(appLog.logFilePath)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    ForEach(Array(bluetooth.logs.prefix(40).enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.caption2.monospaced())
                            .textSelection(.enabled)
                    }
                } header: {
                    Text("Log")
                }
            }
            .navigationTitle("Forrest Voice")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    NavigationLink {
                        SettingsView()
                    } label: {
                        Image(systemName: "gearshape")
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    if bluetooth.connectedName != nil {
                        Button("Disconnect") { bluetooth.disconnect() }
                    } else {
                        Button("Scan") { bluetooth.startScanning() }
                    }
                }
            }
        }
    }

    private func stateLabel(_ state: CBManagerState) -> String {
        switch state {
        case .unknown: "Unknown"
        case .resetting: "Resetting"
        case .unsupported: "Unsupported"
        case .unauthorized: "Unauthorized"
        case .poweredOff: "Off"
        case .poweredOn: "On"
        @unknown default: "?"
        }
    }
}

#if DEBUG
#Preview {
    ContentView()
        .environmentObject(BluetoothManager())
}
#endif
