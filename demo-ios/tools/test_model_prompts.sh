#!/bin/sh
set -eu
[ "$#" -eq 6 ] || { echo "usage: $0 SDK_INCLUDE HOST_BUILD TEXT_DIR AUDIO_DIR VL_DIR OMNI_DIR" >&2; exit 2; }
sdk_include=$(cd "$1" && pwd -P)
host_build=$2
text_directory=$3
audio_directory=$4
vl_directory=$5
omni_directory=$6
demo_root=$(cd "$(dirname "$0")/.." && pwd -P)
mkdir -p "$demo_root/build/host"
xcrun swiftc "$demo_root/Sources/DemoPreset.swift" "$demo_root/Sources/ModelCatalog.swift" "$demo_root/Tests/preset_contract_test.swift" -o "$demo_root/build/host/preset-contract"
"$demo_root/build/host/preset-contract" "$demo_root/Resources/Samples" > "$demo_root/build/host/presets.json"
set -- xcrun clang++ -std=c++17 -O2 -fobjc-arc "-I$sdk_include"
set -- "$@" "$demo_root/Tests/model_prompt_test.mm"
for component in audio vision media tokenizer metal; do
  set -- "$@" "$host_build/runtime/liblfm3_ios_$component.a"
done
set -- "$@" "$host_build/tokenizers/libtokenizers_cpp.a" "$host_build/tokenizers/libtokenizers_c.a"
for framework in Foundation Security Metal CoreML AVFoundation CoreGraphics ImageIO CoreMedia CoreVideo; do
  set -- "$@" -framework "$framework"
done
"$@" -o "$demo_root/build/host/model-prompt"
"$demo_root/build/host/model-prompt" "$demo_root/build/host/presets.json" "$demo_root/Resources/Samples/DarwinIntroduction.txt" \
  "$text_directory" "$audio_directory" "$vl_directory" "$omni_directory"
