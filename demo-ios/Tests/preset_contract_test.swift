import AVFoundation
import Foundation

// Minimal UI-free input type; production preset declarations are compiled
// directly, so the token test consumes the actual app task strings and modes.
enum DemoInput { case text, image, audio, mixed
    var usesAudio: Bool { self == .audio || self == .mixed }
}
@main struct PresetContractTest {
    static func main() throws {
        guard CommandLine.arguments.count == 2 else { throw NSError(domain: "Samples directory required", code: 1) }
        let samples = URL(fileURLWithPath: CommandLine.arguments[1])
        var values: [[String: Any]] = []
        for preset in DemoPreset.allCases {
            var audioRows = 0
            if let name = preset.audioName {
                let file = try AVAudioFile(forReading: samples.appendingPathComponent(name + ".wav"))
                precondition(file.processingFormat.sampleRate == 16000 && file.processingFormat.channelCount == 1)
                precondition(file.length > 0 && Double(file.length) / 16000 <= 30)
                let mel = 1 + max(0, Int(file.length) - 400 + 159) / 160
                audioRows = (mel + 7) / 8
            }
            let marker = preset.output == .text ? 0 : preset.output == .both ? 125015 : 125012
            if preset == .tts { precondition(preset.output == .both && preset.title == "Spoken summary") }
            values.append(["id": preset.rawValue, "model": preset.model.rawValue, "task": preset.prompt, "marker": marker,
                           "image_rows": preset.imageName == nil ? 0 : 256, "audio_rows": audioRows])
        }
        let data = try JSONSerialization.data(withJSONObject: values, options: [.prettyPrinted, .sortedKeys])
        FileHandle.standardOutput.write(data)
    }
}
