import Foundation
enum DemoInput { case text, image, audio, mixed
    var usesAudio: Bool { self == .audio || self == .mixed }
}
@main struct CatalogTest {
    static func main() throws {
        let url = URL(fileURLWithPath: CommandLine.arguments[1])
        let config = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
        let releases = ModelRelease.read(config); precondition(releases.count == 4)
        precondition(releases[.text]!.directories == ["apple/lfm3-1.7b/ios"])
        precondition(releases[.omni]!.directories.contains("OmniModel"))
        for preset in DemoPreset.allCases {
            let expected: DemoModel = preset == .summary ? .text : preset == .asr ? .audio : preset == .image ? .vl : .omni
            precondition(preset.model == expected)
        }
        var invalid = config; var models = config["models"] as! [String: [String: Any]]
        models["text"]!["expected_package_id"] = "not-a-fingerprint"; invalid["models"] = models
        precondition(ModelRelease.read(invalid)[.text] == nil && ModelRelease.read(invalid).count == 3)
        precondition(ModelRelease.read([:]).isEmpty)
        print("four trusted model releases, six routes, explicit Omni-only fallback and invalid/missing ID rejection PASS")
    }
}
