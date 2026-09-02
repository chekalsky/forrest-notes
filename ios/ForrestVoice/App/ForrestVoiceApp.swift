import SwiftUI

@main
struct ForrestVoiceApp: App {
    @StateObject private var bluetooth = BluetoothManager()

    init() {
        NotificationService.shared.configure()
        AppLog.shared.log("app", "Forrest Voice launched")
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(bluetooth)
                .task {
                    _ = await NotificationService.shared.requestAuthorization()
                    AppLog.shared.log("app", "Notification authorization requested")

                    let speechStatus = await SpeechTranscriptionService.shared.requestAuthorization()
                    AppLog.shared.log("app", "Speech authorization status=\(speechStatus.rawValue)")
                }
        }
    }
}
