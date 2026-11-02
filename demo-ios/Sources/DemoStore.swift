import AVFoundation
import Foundation
import SwiftUI
import ImageIO
import UIKit

enum DemoInput: String, CaseIterable, Identifiable {
    case text = "Text", image = "Vision", audio = "Audio", mixed = "Mixed"
    var id: String { rawValue }
    var icon: String { switch self { case .text: "text.alignleft"; case .image: "photo"; case .audio: "waveform"; case .mixed: "square.stack.3d.up" } }
    var usesImage: Bool { self == .image || self == .mixed }
    var usesAudio: Bool { self == .audio || self == .mixed }
}

@MainActor
final class DemoStore: ObservableObject {
    let isPreview = Bundle.main.object(forInfoDictionaryKey: "DemoUIPreview") as? String == "true"
    @Published var input: DemoInput = .text { didSet { if !applyingPreset { selectedPreset = nil; if prompt == Self.defaultTask(oldValue) { prompt = Self.defaultTask(input) } }; invalidate() } }
    @Published var outputMode: DemoOutput = .text { didSet { if !applyingPreset { selectedPreset = nil }; invalidate() } }
    var speech: Bool { outputMode != .text }
    @Published var selectedPreset: DemoPreset? = .summary
    @Published var prompt = DemoStore.defaultTask(.text) { didSet { if !applyingPreset { selectedPreset = nil }; invalidate() } }
    @Published var modelReady = false
    @Published var activeModel: DemoModel = .text
    @Published var prefillCapacity = 1536
    @Published var textBudget = 1024
    @Published var openMs: Double?
    @Published var timingBoundary = "TTFT excludes explicit staged model loads."
    private(set) var models: [DemoModel: ModelRelease] = [:]
    @Published var busy = false
    @Published var running = false
    @Published var status = "Set up your model"
    @Published var error: String?
    @Published var expectedPackageID = ""
    @Published var provenanceSHA = ""
    @Published var packageID = ""
    @Published var modelRepo = ""
    @Published var modelRevision = ""
    @Published var modelDirectory = "OmniModel"
    @Published var effectivePrompt = ""
    @Published var rows = 0
    @Published var promptRows = 0
    @Published var textRows = 0
    @Published var observedPromptRows: Int?
    @Published var remaining = 2048
    @Published var imageURL: URL?
    @Published var previewImage: UIImage?
    @Published var audioURL: URL?
    @Published var audioSeconds = 0.0
    @Published var imageRows = 0
    @Published var audioRows = 0
    @Published var output = ""
    @Published var ttft: Double?
    @Published var visibleTime: Double?
    @Published var tps: Double?
    @Published var firstPCM: Double?
    @Published var framesPerSecond: Double?
    @Published var audioRealtime: Double?
    @Published var selectedTokens = 0
    @Published var producedSeconds = 0.0
    @Published var levels: [Double] = []
    @Published var generatedAudioURL: URL?
    @Published var wallMs: Double?
    @Published var loadMs: Double?
    @Published var setupMs: Double?
    @Published var verificationMs: Double?
    @Published var responseWallMs: Double?
    @Published var recording = false
    @Published var playing = false
    @Published var inputPlaying = false
    private var inputPlayer: AVAudioPlayer?
    private let engine = OmniBridge()
    private var player: AVAudioPlayer?
    private var recorder: AVAudioRecorder?
    private var recordingTimer: Timer?
    private var playbackTimer: Timer?
    private var prepared = false
    private var applyingPreset = false
    private var launchScenario: DemoPreset?
    private enum LaunchStage { case waitingForModel, loading, preparing, running, finished }
    private var launchStage = LaunchStage.waitingForModel
    private var inputSHA256 = ""
    private let inputDirectory = FileManager.default.temporaryDirectory.appendingPathComponent("OmniDemoInputs", isDirectory: true).appendingPathComponent(UUID().uuidString, isDirectory: true)
    private static func defaultTask(_ input: DemoInput) -> String {
        switch input {
        case .text: "Summarize the following text in three sentences."
        case .image: "Describe the image."
        case .audio: "Transcribe the speech exactly."
        case .mixed: "Describe the image, then transcribe the speech."
        }
    }

    init() {
        do { launchScenario = try ScenarioLaunch.parse(ProcessInfo.processInfo.arguments).flatMap(DemoPreset.init(rawValue:)) }
        catch { self.error = error.localizedDescription }
        let current = inputDirectory
        DispatchQueue.global(qos: .utility).async {
            try? FileManager.default.createDirectory(at: current, withIntermediateDirectories: true)
            let root = current.deletingLastPathComponent()
            for previous in (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? [] where previous != current && UUID(uuidString: previous.lastPathComponent) != nil {
                try? FileManager.default.removeItem(at: previous)
            }
        }
        if let url = Bundle.main.url(forResource: "ModelConfiguration", withExtension: "json"),
           let data = try? Data(contentsOf: url), let config = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            models = ModelRelease.read(config)
            expectedPackageID = config["expected_package_id"] as? String ?? ""
            provenanceSHA = config["provenance_sha256"] as? String ?? ""
            modelRepo = config["repo_id"] as? String ?? ""
            modelRevision = config["revision"] as? String ?? ""
            modelDirectory = config["model_directory"] as? String ?? "OmniModel"
        }
        modelReady = !models.isEmpty
        status = isPreview ? "UI preview · inference disabled" : modelReady ? "Choose an example · its model is verified before running" : "Model release configuration is missing"
        DispatchQueue.main.async { [weak self] in self?.beginLaunchScenarioIfReady() }
    }

    private func beginLaunchScenarioIfReady() {
        guard let scenario = launchScenario, launchStage == .waitingForModel, modelReady, !busy, !isPreview else { return }
        runPreset(scenario, recordScenario: true)
    }

    var canPrepare: Bool {
        modelReady && !busy && !running && (!input.usesImage || imageURL != nil) && (!input.usesAudio || audioURL != nil)
    }
    var canRun: Bool { prepared && !busy && !running }
    var rowFraction: Double { min(1, Double(rows) / Double(max(1, prefillCapacity))) }
    func invalidate() { prepared = false; rows = 0; promptRows = 0; textRows = 0; observedPromptRows = nil; effectivePrompt = "" }
    func runPreset(_ preset: DemoPreset, recordScenario: Bool = false) {
        guard !busy && !running && !recording && modelReady else { return }
        if !recordScenario { launchScenario = nil }
        guard let release = models[preset.model] else { error = "No trusted release is configured for \(preset.model.title)."; status = "Model release missing"; launchStage = .finished; saveScenarioResult(); return }
        activeModel = preset.model; packageID = ""; expectedPackageID = release.packageID; provenanceSHA = release.provenance
        busy = true; error = nil; status = "Verifying \(activeModel.title)…"
        engine.useModel(inDocuments: activeModel.rawValue, directories: release.directories, expectedPackageID: release.packageID, provenanceSHA256: release.provenance) { [weak self] result in
            Task { @MainActor in
                guard let self else { return }
                self.busy = false
                guard result["ok"] as? Bool == true else { self.error = result["error"] as? String ?? "Model verification failed"; self.status = "\(self.activeModel.title) unavailable"; self.launchStage = .finished; self.saveScenarioResult(); return }
                self.packageID = result["packageID"] as? String ?? ""
                self.prefillCapacity = result["prefillCapacity"] as? Int ?? 0
                self.launchStage = .loading
                self.applyPreset(preset)
            }
        }
    }
    func applyPreset(_ preset: DemoPreset) {
        guard !busy && !running && !recording else { return }
        applyingPreset = true
        Self.removeOwned(imageURL); Self.removeOwned(audioURL)
        input = preset.input; outputMode = preset.output; prompt = preset.prompt
        selectedPreset = preset; imageURL = nil; audioURL = nil; previewImage = nil; invalidate()
        applyingPreset = false; busy = true; status = "Loading example…"; error = nil
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let imageURL = preset.imageName.flatMap { Bundle.main.url(forResource: $0, withExtension: "png", subdirectory: "Samples") }
            let audioURL = preset.audioName.flatMap { Bundle.main.url(forResource: $0, withExtension: "wav", subdirectory: "Samples") }
            let image = imageURL.flatMap { UIImage(contentsOfFile: $0.path) }
            DispatchQueue.main.async {
                guard let self else { return }
                self.imageURL = imageURL; self.audioURL = audioURL; self.previewImage = image; self.busy = false
                if (preset.imageName != nil && imageURL == nil) || (preset.audioName != nil && audioURL == nil) { self.error = "The selected example resource is missing."; self.status = "Example unavailable"; if self.launchStage == .loading { self.launchStage = .finished; self.saveScenarioResult() } }
                else { self.status = "Example ready · prepare input"; if self.launchStage == .loading { self.launchStage = .preparing; self.prepare() } }
            }
        }
    }

    func importModel(_ url: URL) {
        guard !busy && !running else { return }
        busy = true; error = nil; status = "Copying and verifying model…"
        engine.importModel(at: url, expectedPackageID: expectedPackageID.lowercased().trimmingCharacters(in: .whitespacesAndNewlines), provenanceSHA256: provenanceSHA.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)) { [weak self] result in
            Task { @MainActor in
            guard let self else { return }
            self.busy = false
            if result["ok"] as? Bool == true {
                self.modelReady = true; self.packageID = result["packageID"] as? String ?? ""; self.status = "Model ready"; self.invalidate()
                self.beginLaunchScenarioIfReady()
            } else { self.error = result["error"] as? String ?? "Model import failed"; self.status = "Model import failed" }
            }
        }
    }

    func importMedia(_ url: URL, image: Bool) {
        guard !busy && !running else { return }
        busy = true; status = "Importing input…"; error = nil
        let folder = inputDirectory
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            var copiedURL: URL?
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let destination = try BoundedMediaCopy.copy(source: url, directory: folder, limit: image ? 128 * 1024 * 1024 : 256 * 1024 * 1024)
                copiedURL = destination
                var thumbnail: UIImage?
                if image, let source = CGImageSourceCreateWithURL(destination as CFURL, nil), let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 512, kCGImageSourceCreateThumbnailWithTransform: true] as CFDictionary) { thumbnail = UIImage(cgImage: cgImage) }
                let preview = thumbnail
                DispatchQueue.main.async {
                    guard let self else { return }
                    if image { Self.removeOwned(self.imageURL); self.imageURL = destination; self.previewImage = preview } else { Self.removeOwned(self.audioURL); self.audioURL = destination }
                    self.selectedPreset = nil; self.invalidate(); self.busy = false; self.status = "Input imported"
                }
            } catch {
                Self.removeOwned(copiedURL)
                let message = error.localizedDescription
                DispatchQueue.main.async { self?.busy = false; self?.error = message; self?.status = "Import failed" }
            }
        }
    }

    func prepare() {
        guard canPrepare else { return }
        busy = true; error = nil; status = "Preparing input…"
        engine.preparePrompt(prompt, imageURL: input.usesImage ? imageURL : nil, audioURL: input.usesAudio ? audioURL : nil, speech: speech, interleaved: outputMode == .both, documentContext: selectedPreset != nil) { [weak self] result in
            Task { @MainActor in
            guard let self else { return }
            self.busy = false
            if result["ok"] as? Bool == true {
                self.prepared = true; self.rows = result["rows"] as? Int ?? 0; self.remaining = result["remaining"] as? Int ?? 0
                self.promptRows = result["promptRows"] as? Int ?? 0; self.observedPromptRows = nil
                self.textRows = result["textRows"] as? Int ?? 0
                self.textBudget = result["textBudget"] as? Int ?? 1024
                self.prefillCapacity = result["capacity"] as? Int ?? self.prefillCapacity
                self.effectivePrompt = result["effectivePrompt"] as? String ?? ""; self.audioSeconds = result["audioSeconds"] as? Double ?? 0
                self.imageRows = result["imageRows"] as? Int ?? 0; self.audioRows = result["audioRows"] as? Int ?? 0; self.status = "Ready to run"
                self.inputSHA256 = result["inputSHA256"] as? String ?? ""
                if self.launchStage == .preparing { self.launchStage = .running; self.run() }
            } else { self.prepared = false; self.status = "Check your input"; self.error = result["error"] as? String ?? "Input preparation failed"; if self.launchStage == .preparing { self.launchStage = .finished; self.saveScenarioResult() } }
            }
        }
    }

    func run() {
        guard canRun else { return }
        player?.stop(); playing = false
        inputPlayer?.stop(); inputPlaying = false
        playbackTimer?.invalidate(); playbackTimer = nil
        Self.removeOwned(generatedAudioURL)
        output = ""; ttft = nil; visibleTime = nil; tps = nil; firstPCM = nil; framesPerSecond = nil; audioRealtime = nil
        selectedTokens = 0; producedSeconds = 0; levels = []; generatedAudioURL = nil; wallMs = nil; loadMs = nil; setupMs = nil; verificationMs = nil; responseWallMs = nil; openMs = nil
        error = nil; running = true; status = "Starting…"
        engine.run { [weak self] event in Task { @MainActor in self?.receive(event) } }
    }
    func cancel() { engine.cancel(); status = "Stopping…" }
    private func receive(_ event: [AnyHashable: Any]) {
        switch event["type"] as? String {
        case "status": status = event["message"] as? String ?? status
        case "text": output += event["text"] as? String ?? ""
        case "promptValidated": observedPromptRows = event["promptRows"] as? Int
        case "metrics":
            ttft = event["ttftMs"] as? Double; visibleTime = event["visibleMs"] as? Double
            tps = event["decodeTPS"] as? Double; firstPCM = event["firstPCMMs"] as? Double
            framesPerSecond = event["frameRate"] as? Double; selectedTokens = event["tokens"] as? Int ?? 0
            producedSeconds = event["audioSeconds"] as? Double ?? 0
        case "audioLevel":
            levels.append(event["level"] as? Double ?? 0); if levels.count > 60 { levels.removeFirst(levels.count - 60) }
        case "complete":
            openMs = event["openMs"] as? Double
            timingBoundary = event["timingBoundary"] as? String ?? "TTFT excludes explicit staged model loads; request setup remains included."
            running = false; status = event["reason"] as? String ?? "Complete"
            generatedAudioURL = event["audioURL"] as? URL; audioRealtime = event["audioRealtime"] as? Double
            producedSeconds = event["audioSeconds"] as? Double ?? producedSeconds
            wallMs = event["wallMs"] as? Double; loadMs = event["modelLoadMs"] as? Double; setupMs = event["setupMs"] as? Double
            verificationMs = event["verificationMs"] as? Double; responseWallMs = event["responseWallMs"] as? Double
        case "error": running = false; status = "Request failed"; error = event["message"] as? String ?? "Unknown error"
        default: break
        }
        if launchStage == .running, !running {
            launchStage = .finished
            saveScenarioResult()
        }
    }

    private func saveScenarioResult() {
        guard let scenario = launchScenario else { return }
        var record: [String: Any] = ["scenario": scenario.rawValue, "phase": String(describing: launchStage), "status": status, "package_id": packageID, "model_id": activeModel.title, "model_kind": activeModel.rawValue, "prefill_capacity": prefillCapacity,
            "input_sha256": inputSHA256, "input_hash_basis": activeModel == .vl ? "prompt IDs, original image file SHA256, normalized PCM" : "prompt IDs, decoded RGB, normalized PCM", "text_tokens": textRows, "prompt_rows": promptRows,
            "prefill_rows": rows, "output": output, "selected_tokens": selectedTokens,
            "audio_seconds": producedSeconds, "timestamp": ISO8601DateFormatter().string(from: Date())]
        record["ttft_ms_excluding_model_load"] = ttft; record["decode_tokens_per_second"] = tps
        record["first_pcm_ms_excluding_model_load"] = firstPCM; record["audio_realtime"] = audioRealtime
        record["raw_wall_ms"] = wallMs; record["error"] = error
        record["runtime_prompt_rows"] = observedPromptRows
        record["open_ms"] = openMs; record["timing_boundary"] = timingBoundary
        if let audio = generatedAudioURL { record["generated_audio_path"] = audio.path }
        guard let data = try? JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys]),
              let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            do { try data.write(to: folder.appendingPathComponent("LatestScenario.json"), options: .atomic) }
            catch { DispatchQueue.main.async { [weak self] in self?.error = "Could not save scenario result: \(error.localizedDescription)" } }
        }
    }

    func toggleInputPlayback() {
        if inputPlaying { inputPlayer?.stop(); inputPlaying = false; return }
        guard let url = audioURL, !running && !busy else { return }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback); try AVAudioSession.sharedInstance().setActive(true)
            inputPlayer = try AVAudioPlayer(contentsOf: url); inputPlaying = inputPlayer?.play() == true
            let duration = inputPlayer?.duration ?? 0
            DispatchQueue.main.asyncAfter(deadline: .now() + duration + 0.1) { [weak self] in self?.inputPlaying = false }
        } catch { self.error = error.localizedDescription }
    }

    func togglePlayback() {
        if playing { player?.stop(); playing = false; playbackTimer?.invalidate(); playbackTimer = nil; return }
        guard let url = generatedAudioURL else { return }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback); try AVAudioSession.sharedInstance().setActive(true)
            player = try AVAudioPlayer(contentsOf: url); player?.play(); playing = true
            playbackTimer?.invalidate()
            playbackTimer = Timer.scheduledTimer(withTimeInterval: (player?.duration ?? 0) + 0.1, repeats: false) { [weak self] _ in Task { @MainActor in self?.playing = false } }
        }
        catch { self.error = error.localizedDescription }
    }
    func startRecording() {
        guard !busy && !running && !isPreview else { return }
        AVAudioApplication.requestRecordPermission { [weak self] granted in
            Task { @MainActor in
                guard let self else { return }
                guard granted else { self.error = "Microphone permission is needed to record an input."; return }
                guard !self.busy && !self.running && !self.recording else { return }
                var recordingURL: URL?
                do {
                    try AVAudioSession.sharedInstance().setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
                    try AVAudioSession.sharedInstance().setActive(true)
                    let url = self.inputDirectory.appendingPathComponent(UUID().uuidString + ".wav")
                    recordingURL = url
                    self.recorder = try AVAudioRecorder(url: url, settings: [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 16000, AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false])
                    guard self.recorder?.record(forDuration: 30) == true else { throw CocoaError(.fileWriteUnknown) }
                    self.recording = true; self.recordingTimer?.invalidate()
                    self.recordingTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: false) { [weak self] _ in Task { @MainActor in self?.stopRecording() } }
                } catch { self.recorder = nil; Self.removeOwned(recordingURL); self.error = error.localizedDescription }
            }
        }
    }
    func stopRecording() {
        guard recording else { return }
        recorder?.stop(); recordingTimer?.invalidate(); recordingTimer = nil; recording = false
        if let url = recorder?.url { Self.removeOwned(audioURL); audioURL = url; selectedPreset = nil; invalidate() }
        recorder = nil
    }
    nonisolated private static func removeOwned(_ url: URL?) {
        guard let url, url.isFileURL else { return }
        let roots = [FileManager.default.temporaryDirectory.appendingPathComponent("OmniDemoInputs").standardizedFileURL.path,
                     FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?.appendingPathComponent("OmniDemoAudio").standardizedFileURL.path].compactMap { $0 }
        let path = url.standardizedFileURL.path
        guard roots.contains(where: { path.hasPrefix($0 + "/") }) else { return }
        DispatchQueue.global(qos: .utility).async { try? FileManager.default.removeItem(at: url) }
    }
}
