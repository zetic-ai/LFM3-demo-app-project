import Foundation

enum DemoOutput: String, CaseIterable, Identifiable {
    case text = "Text", audio = "Audio", both = "Both"
    var id: String { rawValue }
}
enum DemoPreset: String, CaseIterable, Identifiable {
    case summary, tts, image, vlAudio = "vl_audio", asr, voice
    var id: String { rawValue }
    var title: String {
        switch self { case .summary: "Summary"; case .tts: "Spoken summary"; case .image: "Visual-length summary"; case .vlAudio: "Visual spoken summary"; case .asr: "Spoken question"; case .voice: "Spoken answer" }
    }
    var input: DemoInput { switch self { case .summary, .tts: .text; case .image, .vlAudio: .image; case .asr, .voice: .audio } }
    var output: DemoOutput { switch self { case .summary, .image, .asr: .text; case .tts, .vlAudio, .voice: .both } }
    var prompt: String {
        switch self {
        case .summary: "Summarize the following text in three sentences."
        case .tts: "Give a spoken summary of the reference extract in one sentence, at most twelve words. Say only that sentence."
        case .image: "1. Count the black circles in the image.\n2. Summarize ONLY the reference extract above in that many short sentences. Use one fact from the extract per sentence. Do not describe the picture."
        case .vlAudio: "1. Count the black circles in the image.\n2. Give a spoken summary of ONLY the reference extract above in that many short sentences. Use one fact from the extract per sentence. Do not describe the picture."
        case .asr: "Answer the spoken question using the reference extract below. Use only facts from the extract. Give two short sentences in plain language. Answer the question rather than transcribing the audio."
        case .voice: "Answer the spoken question using the reference extract below. Use only facts from the extract. Give a spoken answer in two short sentences, at most thirty words in total. Answer the question rather than transcribing the audio."
        }
    }
    var imageName: String? { input == .image ? "count" : nil }
    var audioName: String? { input.usesAudio ? "DarwinQuestion" : nil }
}
