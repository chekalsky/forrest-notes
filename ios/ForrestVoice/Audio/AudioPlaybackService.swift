import AVFoundation
import Combine

@MainActor
final class AudioPlaybackService: ObservableObject {
    @Published private(set) var playingId: UUID?
    @Published private(set) var isPlaying = false
    @Published private(set) var currentTime: TimeInterval = 0
    @Published private(set) var duration: TimeInterval = 0

    private var player: AVAudioPlayer?
    private var progressTimer: Timer?
    private let delegate = PlayerDelegate()

    init() {
        delegate.owner = self
    }

    var progress: Double {
        guard duration > 0 else { return 0 }
        return min(1, max(0, currentTime / duration))
    }

    func isActiveRecording(_ rec: SavedRecording) -> Bool {
        playingId == rec.id && isPlaying
    }

    func toggle(_ rec: SavedRecording) {
        if playingId == rec.id {
            if isPlaying { pause() } else { resume() }
        } else {
            play(rec)
        }
    }

    func play(_ rec: SavedRecording) {
        stopInternal(resetPublished: false)
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default)
            try session.setActive(true)

            let p = try AVAudioPlayer(contentsOf: rec.url)
            p.delegate = delegate
            p.prepareToPlay()
            guard p.play() else {
                throw PlaybackError.startFailed
            }
            player = p
            playingId = rec.id
            isPlaying = true
            duration = p.duration
            currentTime = p.currentTime
            startProgressTimer()
        } catch {
            stopInternal(resetPublished: true)
            AppLog.shared.log("playback", "Failed \(rec.filename): \(error.localizedDescription)")
        }
    }

    func pause() {
        player?.pause()
        isPlaying = false
        stopProgressTimer()
        currentTime = player?.currentTime ?? currentTime
    }

    func resume() {
        guard let player, playingId != nil else { return }
        guard player.play() else { return }
        isPlaying = true
        startProgressTimer()
    }

    func stop() {
        stopInternal(resetPublished: true)
    }

    fileprivate func handleFinished() {
        stopInternal(resetPublished: true)
    }

    private func stopInternal(resetPublished: Bool) {
        player?.stop()
        player = nil
        stopProgressTimer()
        if resetPublished {
            playingId = nil
            isPlaying = false
            currentTime = 0
            duration = 0
        }
    }

    private func startProgressTimer() {
        stopProgressTimer()
        progressTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, let player = self.player else { return }
                self.currentTime = player.currentTime
                self.duration = player.duration
            }
        }
    }

    private func stopProgressTimer() {
        progressTimer?.invalidate()
        progressTimer = nil
    }

    static func formatTime(_ t: TimeInterval) -> String {
        let s = max(0, Int(t.rounded()))
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    enum PlaybackError: LocalizedError {
        case startFailed
        var errorDescription: String? { "Could not start playback" }
    }
}

private final class PlayerDelegate: NSObject, AVAudioPlayerDelegate {
    weak var owner: AudioPlaybackService?

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor in
            owner?.handleFinished()
        }
    }

    func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) {
        Task { @MainActor in
            if let error {
                AppLog.shared.log("playback", "Decode error: \(error.localizedDescription)")
            }
            owner?.handleFinished()
        }
    }
}
