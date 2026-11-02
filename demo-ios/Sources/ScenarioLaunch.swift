import Foundation

enum ScenarioLaunch {
    static let names = ["summary", "tts", "image", "vl_audio", "asr", "voice"]
    static func parse(_ arguments: [String]) throws -> String? {
        let positions = arguments.indices.filter { arguments[$0] == "--scenario" }
        guard let index = positions.first else { return nil }
        guard positions.count == 1, arguments.indices.contains(index + 1), names.contains(arguments[index + 1]) else {
            throw NSError(domain: "LFM3Demo", code: 1, userInfo: [NSLocalizedDescriptionKey: "Use --scenario summary|tts|image|vl_audio|asr|voice."])
        }
        return arguments[index + 1]
    }
}
