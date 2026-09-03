import Foundation

@MainActor
final class OpenClawSettings: ObservableObject {
    static let shared = OpenClawSettings()

    private enum Keys {
        static let gatewayBaseURL = "openclaw.gatewayBaseURL"
        static let agentId = "openclaw.agentId"
        static let allowSelfSigned = "openclaw.allowSelfSignedCertificate"
        static let hooksTokenKeychain = "openclaw.hooksToken"
        static let deliveryChannel = "openclaw.deliveryChannel"
        static let deliveryTo = "openclaw.deliveryTo"
    }

    @Published var gatewayBaseURL: String {
        didSet { UserDefaults.standard.set(gatewayBaseURL, forKey: Keys.gatewayBaseURL) }
    }

    @Published var agentId: String {
        didSet {
            let trimmed = agentId.trimmingCharacters(in: .whitespacesAndNewlines)
            let stored = trimmed.isEmpty ? "main" : trimmed
            UserDefaults.standard.set(stored, forKey: Keys.agentId)
            if stored != agentId {
                agentId = stored
            }
        }
    }

    @Published var allowSelfSignedCertificate: Bool {
        didSet { UserDefaults.standard.set(allowSelfSignedCertificate, forKey: Keys.allowSelfSigned) }
    }

    @Published var deliveryChannel: String {
        didSet { UserDefaults.standard.set(deliveryChannel, forKey: Keys.deliveryChannel) }
    }

    @Published var deliveryTo: String {
        didSet { UserDefaults.standard.set(deliveryTo, forKey: Keys.deliveryTo) }
    }

    private init() {
        gatewayBaseURL = UserDefaults.standard.string(forKey: Keys.gatewayBaseURL) ?? ""
        let storedAgentId = UserDefaults.standard.string(forKey: Keys.agentId) ?? "main"
        let trimmedStoredAgentId = storedAgentId.trimmingCharacters(in: .whitespacesAndNewlines)
        agentId = trimmedStoredAgentId.isEmpty ? "main" : trimmedStoredAgentId
        allowSelfSignedCertificate = UserDefaults.standard.object(forKey: Keys.allowSelfSigned) as? Bool ?? true
        deliveryChannel = UserDefaults.standard.string(forKey: Keys.deliveryChannel) ?? ""
        deliveryTo = UserDefaults.standard.string(forKey: Keys.deliveryTo) ?? ""
        migrateHooksTokenKeychainAccessibility()
    }

    /// Re-save token with AfterFirstUnlock so background BLE processing can read it.
    private func migrateHooksTokenKeychainAccessibility() {
        let token = trimmedHooksToken
        guard !token.isEmpty else { return }
        KeychainStore.save(token, account: Keys.hooksTokenKeychain)
    }

    var hooksToken: String {
        get { KeychainStore.load(account: Keys.hooksTokenKeychain) ?? "" }
        set {
            if newValue.isEmpty {
                KeychainStore.delete(account: Keys.hooksTokenKeychain)
            } else {
                KeychainStore.save(newValue, account: Keys.hooksTokenKeychain)
            }
            objectWillChange.send()
        }
    }

    var isConfigured: Bool {
        configurationGaps.isEmpty
    }

    /// Empty when ready to send; otherwise names the missing field(s).
    var configurationGaps: [String] {
        var gaps: [String] = []
        if normalizedBaseURL().isEmpty { gaps.append("gateway URL") }
        if trimmedHooksToken.isEmpty { gaps.append("hooks token") }
        return gaps
    }

    var trimmedHooksToken: String {
        hooksToken.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Non-empty agent id for hook payloads — OpenClaw rejects `"agentId": ""`.
    var resolvedAgentId: String {
        let trimmed = agentId.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "main" : trimmed
    }

    var trimmedDeliveryChannel: String {
        deliveryChannel.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var trimmedDeliveryTo: String {
        deliveryTo.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// OpenClaw requires channel and to together, or neither (gateway defaults).
    func resolvedDeliveryTarget() throws -> (channel: String, to: String)? {
        var channel = trimmedDeliveryChannel
        let to = trimmedDeliveryTo
        // Legacy default — "last" without `to` is invalid; omit both.
        if channel == "last" && to.isEmpty {
            channel = ""
        }
        if channel.isEmpty && to.isEmpty { return nil }
        if channel.isEmpty || to.isEmpty {
            throw OpenClawError.deliveryTargetIncomplete
        }
        return (channel, to)
    }

    func normalizedBaseURL() -> String {
        gatewayBaseURL.trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    }

    func hooksAgentURL() throws -> URL {
        let base = normalizedBaseURL()
        guard !base.isEmpty else { throw OpenClawError.notConfigured }
        let withScheme = base.hasPrefix("http://") || base.hasPrefix("https://") ? base : "http://\(base)"
        guard var components = URLComponents(string: withScheme) else {
            throw OpenClawError.invalidURL
        }
        var path = components.path
        if path.hasSuffix("/hooks/agent") {
            // already full
        } else if path.hasSuffix("/hooks") {
            path += "/agent"
        } else {
            path = (path.isEmpty ? "" : path) + "/hooks/agent"
        }
        components.path = path
        guard let url = components.url else { throw OpenClawError.invalidURL }
        return url
    }
}

enum OpenClawError: LocalizedError {
    case notConfigured
    case invalidURL
    case emptyMessage
    case deliveryTargetIncomplete
    case network(String)
    case badResponse(Int, String)

    var errorDescription: String? {
        switch self {
        case .notConfigured: "OpenClaw gateway not configured"
        case .invalidURL: "Invalid gateway URL"
        case .emptyMessage: "Empty message"
        case .deliveryTargetIncomplete: "Set both delivery channel and delivery to, or leave both empty"
        case .network(let detail): detail
        case .badResponse(let code, let body): "OpenClaw HTTP \(code): \(body)"
        }
    }
}
