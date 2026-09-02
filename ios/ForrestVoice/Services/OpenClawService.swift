import Foundation

@MainActor
final class OpenClawService {
    static let shared = OpenClawService()

    private init() {}

    private struct AgentRequest: Encodable {
        let message: String
        let name: String
        let agentId: String
        let deliver: Bool
        let wakeMode: String
        let channel: String?
        let to: String?

        private enum CodingKeys: String, CodingKey {
            case message, name, agentId, deliver, wakeMode, channel, to
        }

        func encode(to encoder: Encoder) throws {
            var container = encoder.container(keyedBy: CodingKeys.self)
            try container.encode(message, forKey: .message)
            try container.encode(name, forKey: .name)
            try container.encode(agentId, forKey: .agentId)
            try container.encode(deliver, forKey: .deliver)
            try container.encode(wakeMode, forKey: .wakeMode)
            if let channel, let to {
                try container.encode(channel, forKey: .channel)
                try container.encode(to, forKey: .to)
            }
        }
    }

    /// Single hooks/agent POST — settings test and voice transcripts both use this.
    func postAgentHook(message: String, name: String) async throws {
        let text = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            throw OpenClawError.emptyMessage
        }

        let settings = OpenClawSettings.shared
        guard settings.isConfigured else {
            throw OpenClawError.notConfigured
        }

        let url = try settings.hooksAgentURL()
        let delivery = try settings.resolvedDeliveryTarget()
        let agentId = settings.resolvedAgentId

        let body = AgentRequest(
            message: text,
            name: name,
            agentId: agentId,
            deliver: true,
            wakeMode: "now",
            channel: delivery?.channel,
            to: delivery?.to
        )

        try await postJSON(to: url, body: body, agentId: agentId, delivery: delivery)
    }

    private func postJSON(
        to url: URL,
        body: AgentRequest,
        agentId: String,
        delivery: (channel: String, to: String)?
    ) async throws {
        let settings = OpenClawSettings.shared
        let token = settings.trimmedHooksToken

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue(token, forHTTPHeaderField: "x-openclaw-token")
        request.timeoutInterval = 45
        request.httpBody = try JSONEncoder().encode(body)

        let deliveryLog = delivery.map { "\($0.channel) → \($0.to)" } ?? "default"
        let bodyLog = String(data: request.httpBody ?? Data(), encoding: .utf8) ?? "?"
        AppLog.shared.log("openclaw", "POST agent \(url.absoluteString)")
        AppLog.shared.log("openclaw", "body=\(bodyLog.prefix(400))")
        AppLog.shared.log("openclaw", "agentId=\(agentId) delivery=\(deliveryLog)")

        let session = OpenClawURLSession.session()
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: request)
        } catch let error as URLError {
            AppLog.shared.log("openclaw", "Network error code=\(error.code.rawValue) \(error.localizedDescription)")
            throw OpenClawError.network(Self.describeNetworkError(error, host: url.host ?? "?"))
        }

        guard let http = response as? HTTPURLResponse else {
            throw OpenClawError.badResponse(-1, "No HTTP response")
        }

        let responseBody = String(data: data, encoding: .utf8) ?? ""
        AppLog.shared.log("openclaw", "HTTP \(http.statusCode) \(responseBody.prefix(300))")

        guard (200 ... 299).contains(http.statusCode) else {
            if http.statusCode == 429 {
                let retry = http.value(forHTTPHeaderField: "Retry-After") ?? "?"
                throw OpenClawError.badResponse(http.statusCode, "Rate limited — retry after \(retry)s. \(responseBody.prefix(200))")
            }
            throw OpenClawError.badResponse(http.statusCode, responseBody.prefix(300).description)
        }
    }

    private static func describeNetworkError(_ error: URLError, host: String) -> String {
        switch error.code {
        case .cannotConnectToHost, .networkConnectionLost:
            return "Cannot connect to \(host) — is the gateway running and is the phone on the same network?"
        case .cannotFindHost, .dnsLookupFailed:
            return "Cannot resolve \(host) — try the LAN IP instead of a hostname."
        case .timedOut:
            return "Timed out connecting to \(host) — phone may be off Wi‑Fi or the port blocked."
        case .secureConnectionFailed, .serverCertificateUntrusted:
            return "TLS failed for \(host) — enable “Allow self-signed HTTPS” or fix the certificate."
        case .appTransportSecurityRequiresSecureConnection:
            return "ATS blocked HTTP — use https:// in the gateway URL."
        default:
            return error.localizedDescription
        }
    }
}
