import Foundation

enum TranscriptionLanguage: String, CaseIterable, Identifiable {
    case auto
    case system
    case russian
    case english

    var id: String { rawValue }

    var label: String {
        switch self {
        case .auto: "Auto (detect)"
        case .system: "System language"
        case .russian: "Russian"
        case .english: "English"
        }
    }

    var locale: Locale? {
        switch self {
        case .auto, .system: nil
        case .russian: Locale(identifier: "ru-RU")
        case .english: Locale(identifier: "en-US")
        }
    }
}

@MainActor
final class TranscriptionSettings: ObservableObject {
    static let shared = TranscriptionSettings()

    private enum Keys {
        static let language = "transcription.language"
    }

    @Published var language: TranscriptionLanguage {
        didSet { UserDefaults.standard.set(language.rawValue, forKey: Keys.language) }
    }

    private init() {
        let raw = UserDefaults.standard.string(forKey: Keys.language) ?? TranscriptionLanguage.auto.rawValue
        language = TranscriptionLanguage(rawValue: raw) ?? .auto
    }

    /// Locales tried in Auto mode — Russian included for mixed-language use.
    static func candidateLocales() -> [Locale] {
        var ids: [String] = [Locale.current.identifier, "ru-RU", "en-US"]
        for pref in Locale.preferredLanguages.prefix(4) {
            ids.append(pref)
        }

        var seen = Set<String>()
        var locales: [Locale] = []
        for id in ids {
            let locale = Locale(identifier: id)
            let key = locale.identifier
            guard seen.insert(key).inserted else { continue }
            locales.append(locale)
        }
        return locales
    }
}
