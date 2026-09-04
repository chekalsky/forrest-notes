import CoreBluetooth
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var bluetooth: BluetoothManager
    @EnvironmentObject private var playback: AudioPlaybackService
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
                            RecordingRow(rec: rec)
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

private struct RecordingRow: View {
    let rec: SavedRecording
    @EnvironmentObject private var playback: AudioPlaybackService
    @ObservedObject private var processor = RecordingProcessor.shared

    private var isActive: Bool { playback.isActiveRecording(rec) }
    private var showProgress: Bool { playback.playingId == rec.id && playback.duration > 0 }
    private var isRetranscribing: Bool { processor.activeRecordingId == rec.recordingId }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Button {
                playback.toggle(rec)
            } label: {
                Image(systemName: isActive ? "pause.circle.fill" : "play.circle.fill")
                    .font(.title2)
                    .foregroundStyle(playback.playingId == rec.id ? Color.accentColor : .secondary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(isActive ? "Pause" : "Play")

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text(rec.filename)
                        .font(.body.monospaced())
                    Spacer(minLength: 8)
                    Button {
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        processor.retranscribe(rec)
                    } label: {
                        Group {
                            if isRetranscribing {
                                ProgressView()
                                    .controlSize(.small)
                            } else {
                                Image(systemName: "arrow.clockwise")
                                    .font(.caption)
                            }
                        }
                        .frame(width: 22, height: 22)
                    }
                    .buttonStyle(.borderless)
                    .disabled(isRetranscribing)
                    .accessibilityLabel(isRetranscribing ? "Retranscribing" : "Retranscribe")
                }
                Text("\(rec.byteCount / 1024) KB · \(rec.receivedAt.formatted())")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if showProgress {
                    ProgressView(value: playback.progress)
                    Text("\(AudioPlaybackService.formatTime(playback.currentTime)) / \(AudioPlaybackService.formatTime(playback.duration))")
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                }
                if isRetranscribing {
                    HStack(spacing: 6) {
                        ProgressView()
                            .controlSize(.small)
                        Text("Retranscribing…")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } else if let transcript = rec.transcript, !transcript.isEmpty {
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

#if DEBUG
#Preview {
    ContentView()
        .environmentObject(BluetoothManager())
        .environmentObject(AudioPlaybackService())
}
#endif
