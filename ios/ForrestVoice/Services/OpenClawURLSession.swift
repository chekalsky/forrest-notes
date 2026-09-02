import Foundation

/// Accepts server trust for self-signed HTTPS (OpenClaw homelab use only).
final class SelfSignedURLSessionDelegate: NSObject, URLSessionDelegate {
    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust
        else {
            completionHandler(.performDefaultHandling, nil)
            return
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}

enum OpenClawURLSession {
    private static let insecureDelegate = SelfSignedURLSessionDelegate()
    private static let insecureSession: URLSession = {
        URLSession(configuration: .default, delegate: insecureDelegate, delegateQueue: nil)
    }()

    @MainActor
    static func session() -> URLSession {
        if OpenClawSettings.shared.allowSelfSignedCertificate {
            return insecureSession
        }
        return URLSession.shared
    }
}
