import Foundation

enum DemoModel: String, CaseIterable, Identifiable {
    case text, audio, vl, omni
    var id: String { rawValue }
    var title: String { switch self { case .text: "LFM3-1.7B"; case .audio: "LFM3-Audio-2B"; case .vl: "LFM3-VL-2B"; case .omni: "LFM3-Omni-2B" } }
}
struct ModelRelease {
    let directories: [String]
    let packageID: String
    let provenance: String
    static func read(_ config: [String: Any]) -> [DemoModel: ModelRelease] {
        guard let models = config["models"] as? [String: [String: Any]] else { return [:] }
        var result: [DemoModel: ModelRelease] = [:]
        for kind in DemoModel.allCases {
            guard let item = models[kind.rawValue], let id = item["expected_package_id"] as? String,
                  id.count == 64, id.allSatisfy({ $0.isHexDigit && !$0.isUppercase }),
                  let directory = item["model_directory"] as? String, !directory.isEmpty else { continue }
            result[kind] = ModelRelease(directories: [directory] + (item["fallback_directories"] as? [String] ?? []),
                                        packageID: id, provenance: item["provenance_sha256"] as? String ?? "")
        }
        return result
    }
}
extension DemoPreset {
    var model: DemoModel { switch self { case .summary: .text; case .asr: .audio; case .image: .vl; case .tts, .vlAudio, .voice: .omni } }
}
