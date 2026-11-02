# Host validation

Host utilities do not execute model inference or a phone app. From the repository root:

The Python tests also check that the device helper refuses existing app/model destinations, verifies model files and SDK checksums, and rejects archive traversal and links. A passing mock test does not establish device installation or execution.

```sh
mkdir -p demo-ios/build/host
python3 -m unittest discover -s demo-ios/Tests -p 'test_*.py' -v
xcrun clang++ -std=c++17 -O2 demo-ios/Tests/metrics_test.cpp -o demo-ios/build/host/metrics
demo-ios/build/host/metrics
xcrun clang++ -std=c++17 -O2 demo-ios/Tests/completion_test.cpp -o demo-ios/build/host/completion
demo-ios/build/host/completion
xcrun clang++ -std=c++17 -O2 demo-ios/Tests/input_hash_test.cpp -o demo-ios/build/host/input-hash
demo-ios/build/host/input-hash
xcrun clang++ -std=c++17 -O2 -fobjc-arc demo-ios/Tests/model_store_test.mm \
  demo-ios/Sources/ModelStore.mm -framework Foundation -o demo-ios/build/host/store
demo-ios/build/host/store
xcrun clang++ -std=c++17 -O2 -fobjc-arc demo-ios/Tests/audio_output_test.mm \
  -framework Foundation -o demo-ios/build/host/audio-output
demo-ios/build/host/audio-output
xcrun clang++ -std=c++17 -O2 -fobjc-arc demo-ios/Tests/audio_input_test.mm \
  demo-ios/Sources/AudioInput.mm -framework Foundation -framework AVFoundation \
  -o demo-ios/build/host/audio
demo-ios/build/host/audio
xcrun swiftc demo-ios/Sources/BoundedMediaCopy.swift demo-ios/Tests/bounded_copy_test.swift \
  -o demo-ios/build/host/copy
demo-ios/build/host/copy
xcrun swiftc demo-ios/Sources/ScenarioLaunch.swift demo-ios/Tests/scenario_test.swift \
  -o demo-ios/build/host/scenario
demo-ios/build/host/scenario
xcrun swiftc demo-ios/Sources/DemoPreset.swift demo-ios/Sources/ModelCatalog.swift \
  demo-ios/Tests/catalog_test.swift -o demo-ios/build/host/catalog
demo-ios/build/host/catalog demo-ios/Resources/ModelConfiguration.json
```

## Optional model-specific prompt contracts

The bundled SDK targets iPhone and cannot be linked into macOS host tests. If a compatible macOS arm64 SDK and tokenizer libraries are available, use their public header directory and host-built libraries to run the model-specific tests:

```sh
sh demo-ios/tools/test_model_prompts.sh /absolute/sdk/include/lfm3-ios /absolute/host-build \
  /absolute/models/apple/lfm3-1.7b/ios \
  /absolute/models/apple/lfm3-audio-2b/ios \
  /absolute/models/apple/lfm3-vl-2b/ios \
  /absolute/models/apple/lfm3-omni-2b/ios
```

The host build must provide runtime and tokenizer archives in the locations used by the script; these host libraries are not included in this repository. The test reads each model's tokenizer and exported prefill shape, checks all six routes and actual tasks, and enforces both text and full-prefill limits. These checks do not establish model output correctness.

Downloader tests mock fetching without network use; they check all four family inventories, integrity, unsafe paths, missing components, provenance and partial outputs. Audio tests synthesize waveforms and verify complete-file mono/stereo resampling, bounded reads and unsupported format/duration rejection. Copy/hash tests verify known/unknown-size limits, partial cleanup and preserved originals. Completion tests stress1,000 concurrent terminal-callback→immediate-admission handoffs while duplicate requests remain rejected. Audio-output tests simulate Caches purge/recreation and invalid directory paths.
