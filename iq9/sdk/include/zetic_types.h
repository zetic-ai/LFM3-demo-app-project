// ZeticMLange Hexagon SDK (zetic::mlange_hexagon) — shared types: status codes, the exception thrown by factories,
// capabilities, model info, timings, logging.
//
// Namespace: zetic::mlange with the inline ABI namespace hx1. Source code spells zetic::mlange::LLM as with the
// ZeticMLange 0.1.10 SDK (llama.cpp + QNN); the mangled symbols differ, so both libraries can live in one process.
// Design: sdk/DESIGN.md §2.3.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#define ZETIC_MLANGE_HEXAGON_VERSION_MAJOR 0
#define ZETIC_MLANGE_HEXAGON_VERSION_MINOR 1
#define ZETIC_MLANGE_HEXAGON_VERSION_PATCH 0
#define ZETIC_MLANGE_HEXAGON_VERSION "0.1.0"

// Everything declared below is the public API of libzetic_mlange_hexagon.so (the library is built with -fvisibility=hidden).
#pragma GCC visibility push(default)
namespace zetic::mlange {
inline namespace hx1 {

// What a call reports. Requests (LLM::chat, Decision::systemOne, ...) never throw for these; they come back in
// Response::status / DecisionResult::status with a message. Factories throw Error carrying one of them.
enum class Status : int32_t {
  Ok = 0,
  InvalidArgument,    // malformed media, empty request, bad config value
  Unsupported,        // modality / output the package lacks, or a feature not implemented in this version
                      // (ixian ZETIC_MLANGE_NOT_IMPLEMENTED)
  ContextExceeded,    // the request does not fit max_context (prompt rows + max_tokens reserve > positions)
  Busy,               // another call is in flight on this object and Options::wait_if_busy is false
  PackageError,       // manifest / checksum / target mismatch, missing file, unknown pack_abi
  DeviceError,        // FastRPC / DSP failure (code in Error::dspError()); the object must be reload()ed
  Internal,
};

// Short name of a status ("Ok", "InvalidArgument", ...).
const char* statusName(Status s) noexcept;

// Thrown by factories (LLM::fromPath, LLM::fromModelKey, Decision::fromPath, Image::fromFile, Audio::fromWav, ...)
// and by nothing else.
class Error : public std::runtime_error {
 public:
  Error(Status s, std::string what, int32_t dsp_error = 0);
  Status status() const noexcept;
  int32_t dspError() const noexcept;  // hexrt / FastRPC code (e.g. 0x8000040d), 0 if none

 private:
  Status status_;
  int32_t dsp_error_;
};

// What a loaded package can do. Same field names as ixian lm::multimodal::Capabilities (#671), plus video / decision.
struct Capabilities {
  bool text = false, image = false, audio = false, video = false;
  bool speech_out = false;          // modalities {Audio} / {Text, Audio}
  bool decision = false;            // d1 package: use Decision, not LLM
  bool multi_turn = true;           // always true: the prefix cache (DESIGN.md §2.6)
  bool media_in_followup_turns = true;
  int32_t max_media_per_turn = -1;  // bound only by max_context
  int32_t max_context = 0;          // positions the loaded model holds (LLMOptions::max_context; 2048 by default)
  int32_t input_sample_rate = 0;    // 16000 with audio
  int32_t speech_sample_rate = 0;   // 24000 with speech_out
};

// The loaded package and the device it runs on.
struct ModelInfo {
  std::string model_id, revision;   // "LiquidAI/LFM3-VL-2B", HF commit
  std::string target;               // "v81"
  int32_t hexagon_arch = 0;         // 73 / 79 / 81
  int32_t nsp_count = 1;            // 2 on the IQ9
  std::string soc;                  // "SM8850", "QCS9075"
  std::string package_dir, package_version, sdk_version, runtime_commit;
};

// Wall-clock stage times of one request, in ms (host clock); dsp_* are the DSP times reported by the RPCs.
// llama.cpp server "timings" plus a stage breakdown (DESIGN.md §5.1).
struct Timings {
  double tokenize = 0, template_render = 0, preprocess = 0, encode = 0, assemble = 0, prefill = 0;
  double ttft = 0;                  // call entry -> first text token (or first audio sample for {Audio})
  double ttft_module = 0;           // the module tools' TTFT definition: ttft - tokenize - template_render
  double decode = 0;                // first token -> last token
  int32_t decode_steps = 0;         // DSP decode steps in `decode`
  double tokens_per_second = 0;     // decode_steps / (decode - callbacks)
  double first_audio = 0, audio_seconds = 0, realtime_factor = 0;
  double callbacks = 0;             // time spent inside user callbacks (excluded from tokens_per_second)
  double dsp_prefill = 0, dsp_decode = 0, dsp_encode = 0;
};

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };
// Log sink; personal keys and prompt / reply text are never logged.
using LogSink = std::function<void(LogLevel, std::string_view)>;

}  // namespace hx1
}  // namespace zetic::mlange
#pragma GCC visibility pop
