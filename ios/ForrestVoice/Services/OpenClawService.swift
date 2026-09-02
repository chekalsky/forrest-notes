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
    }

    /// POST /hooks/agent — deliver:true sends agent reply to configured channel (e.g. Telegram).
    func sendTranscript(_ text: String, recordingId: UInt16) async throws {
        let settings = OpenClawSettings.shared
        guard settings.isConfigured else {
            throw OpenClawError.notConfigured
        }

        let url = try settings.hooksAgentURL()
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(settings.hooksToken)", forHTTPHeaderField: "Authorization")
        request.timeoutInterval = 30

        let body = AgentRequest(
            message: text,
            name: "Forrest Voice #\(recordingId)",
            agentId: settings.agentId,
            deliver: true,
            wakeMode: "now"
        )
        request.httpBody = try JSONEncoder().encode(body)

        AppLog.shared.log("openclaw", "POST \(url.absoluteString)")
        AppLog.shared.log("openclaw", "agentId=\(settings.agentId) deliver=true selfSigned=\(settings.allowSelfSignedCertificate)")
        AppLog.shared.log("openclaw", "message=\(text.prefix(120))")

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
        AppLog.shared.log("openclaw", "HTTP \(http.statusCode) \(responseBody.prefix(200))")

        // 202 = async agent run started (expected)
        guard (200 ... 299).contains(http.statusCode) else {
            throw OpenClawError.badResponse(http.statusCode, responseBody.prefix(300).description)
        }
    }

    private static func describeNetworkError(_ error: URLError, host: String) -> String {
        switch error.code {
        case .cannotConnectToHost, .networkConnectionLost:
            return "Cannot connect to \(host) — nothing listening? Check OpenClaw gateway is running and port is open."
        case .cannotFindHost, .dnsLookupFailed:
            return "Cannot resolve \(host) — iPhone may not know this hostname (use IP or fix DNS)."
        case .timedOut:
            return "Timed out connecting to \(host) — phone may be off Wi‑Fi or firewall blocking."
        case .secureConnectionFailed, .serverCertificateUntrusted:
            return "TLS failed for \(host) — enable “Allow self-signed HTTPS” or fix the certificate."
        case .appTransportSecurityRequiresSecureConnection:
            return "ATS blocked HTTP — use https:// or update the app."
        default:
            return error.localizedDescription
        }
    }
}
