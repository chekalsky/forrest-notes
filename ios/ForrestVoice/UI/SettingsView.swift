import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var bluetooth: BluetoothManager
    @ObservedObject private var settings = OpenClawSettings.shared
    @State private var hooksToken = ""
    @State private var tokenLoaded = false
    @State private var testResult: String?
    @State private var isTesting = false
    @State private var showEraseConfirm = false
    @State private var eraseResult: String?

    private var transferInProgress: Bool {
        bluetooth.transferInfo.phase == .receiving
    }

    var body: some View {
        Form {
            Section {
                TextField("Gateway URL", text: $settings.gatewayBaseURL, prompt: Text("https://host:18789"))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)

                TextField("Agent ID", text: $settings.agentId, prompt: Text("main"))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                SecureField("Hooks token", text: $hooksToken)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                Toggle("Allow self-signed HTTPS", isOn: $settings.allowSelfSignedCertificate)

                TextField("Delivery channel", text: $settings.deliveryChannel, prompt: Text("telegram"))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                TextField("Delivery to", text: $settings.deliveryTo, prompt: Text("chat id or number"))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } header: {
                Text("OpenClaw")
            } footer: {
                Text("Leave channel and to empty to use OpenClaw defaults (last channel). If you set one, you must set both — e.g. channel=telegram and your chat id. Use hooks.token, not the gateway auth token.")
            }

            Section {
                LabeledContent("Ready") {
                    Text(settings.isConfigured && !hooksToken.isEmpty ? "Yes" : "No")
                        .foregroundStyle(settings.isConfigured ? .green : .secondary)
                }
                if let endpoint = try? settings.hooksAgentURL().absoluteString {
                    Text(endpoint)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
                Button(isTesting ? "Testing…" : "Send test message") {
                    Task { await runTest() }
                }
                .disabled(isTesting || settings.isConfigured == false || hooksToken.isEmpty)
                if let testResult {
                    Text(testResult)
                        .font(.caption)
                        .foregroundStyle(testResult.hasPrefix("OK") ? .green : .red)
                }
            }

            Section("Help") {
                Text("Include http:// or https:// in the gateway URL. HTTPS + self-signed: enable the toggle above. Plain HTTP is allowed for homelab gateways.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Button("Erase logs and recordings", role: .destructive) {
                    showEraseConfirm = true
                }
                .disabled(transferInProgress)
                if let eraseResult {
                    Text(eraseResult)
                        .font(.caption)
                        .foregroundStyle(eraseResult.hasPrefix("Erased") ? .green : .red)
                }
            } header: {
                Text("Data")
            } footer: {
                if transferInProgress {
                    Text("Wait for the current transfer to finish before erasing.")
                } else {
                    Text("Permanently deletes all saved WAV files, transcripts, and the app log. This cannot be undone.")
                }
            }
        }
        .navigationTitle("Settings")
        .alert("Erase all data?", isPresented: $showEraseConfirm) {
            Button("Erase", role: .destructive) {
                eraseResult = nil
                do {
                    try bluetooth.eraseAllLogsAndRecordings()
                    eraseResult = "Erased logs and recordings"
                } catch {
                    eraseResult = error.localizedDescription
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("All recordings and log entries will be permanently deleted from this iPhone.")
        }
        .onAppear {
            if !tokenLoaded {
                hooksToken = settings.hooksToken
                tokenLoaded = true
            }
        }
        .onChange(of: hooksToken) { _, newValue in
            guard tokenLoaded else { return }
            settings.hooksToken = newValue
        }
    }

    private func runTest() async {
        isTesting = true
        testResult = nil
        defer { isTesting = false }
        do {
            try await OpenClawService.shared.postAgentHook(
                message: """
                Forrest Voice connectivity test from iPhone.

                Reply with a short confirmation for the user on the configured channel.
                """,
                name: "Forrest Voice test"
            )
            testResult = "OK — check your OpenClaw channel"
            AppLog.shared.log("openclaw", "Settings test succeeded")
        } catch {
            testResult = error.localizedDescription
            AppLog.shared.log("openclaw", "Settings test failed: \(error.localizedDescription)")
        }
    }
}

#if DEBUG
#Preview {
    NavigationStack {
        SettingsView()
            .environmentObject(BluetoothManager())
    }
}
#endif
