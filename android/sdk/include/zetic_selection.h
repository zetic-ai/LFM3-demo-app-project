// ZeticMLange Hexagon SDK — fetch options of LLM::fromModelKey / Decision::fromModelKey, the same fields as the
// ZeticMLange 0.1.10 SDK (ixian sdk/cpp/inc/zetic_selection.h), so the published guide's code compiles unchanged.
// v0.1 resolves only from FetchOptions::cache_dir (<cache_dir>/<model_name>/<target>/manifest.json); the download comes
// later behind the same calls (DESIGN.md §6.3).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Everything declared below is the public API of libzetic_mlange_hexagon.so (the library is built with -fvisibility=hidden).
#pragma GCC visibility push(default)
namespace zetic::mlange {
inline namespace hx1 {

// How the backend should pick this project's build (mobile SDKs' ModelMode). The Hexagon packages have one build per
// target, so v0.1 accepts every value and loads that build.
enum class ModelMode { Auto, Fp32, Quantized, Speed, Accuracy };

struct FetchOptions {
  // Local model cache root. Required on Android; on Linux "" resolves to $HOME/.cache/zetic_mlange.
  std::string cache_dir;
  // Download progress observer (bytes_received, bytes_total); not called on a cache hit (v0.1: never called).
  std::function<void(std::uint64_t, std::uint64_t)> on_progress;
  ModelMode mode = ModelMode::Auto;
  int version = 0;
  // Optional PEM trust bundle for HTTPS (unused in v0.1).
  std::string ca_bundle_path;
};

}  // namespace hx1
}  // namespace zetic::mlange
#pragma GCC visibility pop
