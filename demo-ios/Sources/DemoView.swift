import SwiftUI
import UniformTypeIdentifiers

struct DemoView: View {
    @EnvironmentObject private var store: DemoStore
    @Environment(\.scenePhase) private var scenePhase
    private let teal = Color(red: 52 / 255, green: 169 / 255, blue: 163 / 255)
    private let columns = [GridItem(.flexible()), GridItem(.flexible())]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 12) {
                    Image("ZeticSymbol").resizable().scaledToFit().frame(width: 42, height: 42)
                    Text("LFM3 zetic runtime benchmark").font(.headline)
                }
                HStack {
                    if store.busy || store.running { ProgressView().controlSize(.small) }
                    Text(store.isPreview ? "UI PREVIEW · inference disabled" : store.status).font(.subheadline)
                    Spacer()
                }
                if let error = store.error { Text(error).font(.callout).foregroundStyle(.red).textSelection(.enabled) }
                LazyVGrid(columns: columns, spacing: 10) {
                    ForEach(DemoPreset.allCases) { preset in
                        Button { store.runPreset(preset) } label: {
                            VStack(spacing: 5) {
                                Text(presetTitle(preset)).font(.subheadline.weight(.semibold))
                                Text(preset.title).font(.caption)
                            }.frame(maxWidth: .infinity, minHeight: 58)
                        }
                        .buttonStyle(.borderedProminent).tint(teal)
                        .disabled(!store.modelReady || store.busy || store.running)
                    }
                }
                Text("Select an example to run it. Each example uses its matching LFM3 model.").font(.caption).foregroundStyle(.secondary)
                Divider()
                Text("Input · \(store.activeModel.title)").font(.headline)
                if let image = store.previewImage { Image(uiImage: image).resizable().scaledToFit().frame(maxHeight: 240).frame(maxWidth: .infinity) }
                if let audio = store.audioURL {
                    HStack {
                        Text(audio.lastPathComponent).font(.caption)
                        Spacer()
                        Button(store.inputPlaying ? "Stop input" : "Play input", systemImage: store.inputPlaying ? "stop.fill" : "play.fill") { store.toggleInputPlayback() }.disabled(store.running || store.busy)
                    }
                }
                ScrollView { Text(store.effectivePrompt.isEmpty ? store.prompt : store.effectivePrompt).font(.callout).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
                    .frame(maxHeight: 220)
                if store.running { Button("Stop", systemImage: "stop.fill") { store.cancel() }.buttonStyle(.bordered).tint(.red) }
                Divider()
                Text("Output").font(.headline)
                Text(store.output.isEmpty ? "No output yet." : store.output).font(.callout).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                if store.speech {
                    Text("First PCM: \(number(store.firstPCM)) ms · audio: \(number(store.audioRealtime, decimals: 2))× realtime").font(.subheadline.monospacedDigit())
                    Text("Decode tok/s is unavailable for speech output.").font(.caption).foregroundStyle(.secondary)
                } else {
                    Text("TTFT: \(number(store.ttft)) ms · decode: \(number(store.tps, decimals: 2)) tok/s").font(.subheadline.monospacedDigit())
                }
                if let url = store.generatedAudioURL {
                    HStack {
                        Button(store.playing ? "Stop audio" : "Play audio", systemImage: store.playing ? "stop.fill" : "play.fill") { store.togglePlayback() }.buttonStyle(.bordered)
                        Text("\(number(store.producedSeconds, decimals: 2)) s").font(.caption)
                        Spacer()
                        ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
                    }
                }
            }.padding(20).frame(maxWidth: 700).frame(maxWidth: .infinity)
        }
        .background(Color(.systemBackground)).tint(teal)
        .onChange(of: scenePhase) { _, phase in if phase == .background { store.stopRecording(); if store.running { store.cancel() } } }
    }
    private func number(_ value: Double?, decimals: Int = 0) -> String { value.map { String(format: "%.*f", decimals, $0) } ?? "—" }
    private func presetTitle(_ preset: DemoPreset) -> String {
        switch preset { case .summary: "Text → text"; case .tts: "Text → audio"; case .image: "Image → text"; case .vlAudio: "Image → audio"; case .asr: "Audio → text"; case .voice: "Audio → audio" }
    }
}
