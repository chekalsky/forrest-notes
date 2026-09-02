import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = OpenClawSettings.shared
    @State private var hooksToken = ""
    @State private var testResult: String?
    @State private var isTesting = false

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
            } header: {
                Text("OpenClaw")
            } footer: {
                Text("Transcripts are sent to POST /hooks/agent with deliver:true. Enable self-signed HTTPS for homelab gateways with private CAs. Use hooks.token — not the gateway auth token.")
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
        }
        .navigationTitle("Settings")
        .onAppear {
            hooksToken = settings.hooksToken
        }
        .onChange(of: hooksToken) { _, newValue in
            settings.hooksToken = newValue
        }
    }

    private func runTest() async {
        isTesting = true
        testResult = nil
        defer { isTesting = false }
        do {
            try await OpenClawService.shared.sendTranscript(
                "Forrest Voice test ping from iPhone.",
                recordingId: 0
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
    }
}
#endif
