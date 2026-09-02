import Foundation

@MainActor
final class OpenClawSettings: ObservableObject {
    static let shared = OpenClawSettings()

    private enum Keys {
        static let gatewayBaseURL = "openclaw.gatewayBaseURL"
        static let agentId = "openclaw.agentId"
        static let allowSelfSigned = "openclaw.allowSelfSignedCertificate"
        static let hooksTokenKeychain = "openclaw.hooksToken"
    }

    @Published var gatewayBaseURL: String {
        didSet { UserDefaults.standard.set(gatewayBaseURL, forKey: Keys.gatewayBaseURL) }
    }

    @Published var agentId: String {
        didSet { UserDefaults.standard.set(agentId, forKey: Keys.agentId) }
    }

    @Published var allowSelfSignedCertificate: Bool {
        didSet { UserDefaults.standard.set(allowSelfSignedCertificate, forKey: Keys.allowSelfSigned) }
    }

    private init() {
        gatewayBaseURL = UserDefaults.standard.string(forKey: Keys.gatewayBaseURL) ?? ""
        agentId = UserDefaults.standard.string(forKey: Keys.agentId) ?? "main"
        allowSelfSignedCertificate = UserDefaults.standard.object(forKey: Keys.allowSelfSigned) as? Bool ?? true
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
        !normalizedBaseURL().isEmpty && !hooksToken.isEmpty
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
    case network(String)
    case badResponse(Int, String)

    var errorDescription: String? {
        switch self {
        case .notConfigured: "OpenClaw gateway not configured"
        case .invalidURL: "Invalid gateway URL"
        case .network(let detail): detail
        case .badResponse(let code, let body): "OpenClaw HTTP \(code): \(body)"
        }
    }
}
