// ZeticMLange Hexagon SDK — generative models of the LFM3 family on the Hexagon NPU through Ixian Hexagon:
// LFM3-1.7B (text), LFM3-VL-2B, LFM3-Audio-2B, LFM3-Omni-2B. d1 decision models: zetic_decision.h.
//
//   #include <zetic_llm.h>
//   using namespace zetic::mlange;
//   LLM llm = LLM::fromPath("/data/local/tmp/zsdk/models/lfm3-1.7b");
//   Response r = llm.chat({Message::User("What is the capital of France?")}, {}, [](const StreamEvent& ev) {
//     if (ev.type == StreamEvent::Type::Text) fwrite(ev.text.data(), 1, ev.text.size(), stdout);
//     return true;
//   });
//
// Requests are stateless (send the whole history every time, OpenAI style); the SDK prefills only the rows past the
// longest cached prefix (llama.cpp server cache_prompt). Design: sdk/DESIGN.md §2.4 - §2.13.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetic_media.h"
#include "zetic_selection.h"
#include "zetic_types.h"

// Everything declared below is the public API of libzetic_mlange_hexagon.so (the library is built with -fvisibility=hidden).
#pragma GCC visibility push(default)
namespace zetic::mlange {
inline namespace hx1 {

enum class Modality : uint8_t { Text, Audio };
enum class FinishReason : uint8_t { Stop, Length, StopSequence, Cancelled, ContextFull, Error };

// Used when GenerationConfig::modalities contains Audio (LFM3-Audio-2B, LFM3-Omni-2B).
struct AudioOutputConfig {
  int32_t first_chunk_frames = 1;   // code frames (80 ms of audio each) before the first audio event (issue #53)
  int32_t chunk_frames = 12;        // after the first chunk (one codec call = 12 frames = 0.96 s)
  float temperature = -1;           // -1 = the model's generation_config (Audio-2B 1.0 / Omni 0.4)
  int32_t top_k = -1;               // Audio-2B 128 / Omni none
  float repetition_penalty = -1;    // Audio-2B 1.1 / Omni 1.2
  int32_t repetition_window = -1;   // Audio-2B 4096 / Omni 64
  bool collect = true;              // also return the whole clip in Response::audio
};

struct VideoConfig {
  double fps = 2.0;                 // sampling rate (HF Lfm3VlVideoProcessor.fps)
  int32_t min_frames = 4;           // HF min_frames
  int32_t max_groups = 0;           // temporal patches per video; 0 = as many as fit max_context - reserve
};

// Per-request settings. OpenAI / llama.cpp server names. -1 = the package's generation_config.json
// (LFM3 text: temperature 0.1, top_k 50, repetition_penalty 1.1; sampled on the host).
struct GenerationConfig {
  int32_t max_tokens = -1;          // text tokens; -1 = LLM::Options::max_new_tokens (TTS: max_audio_seconds bounds)
  float temperature = -1;           // 0 = greedy (the DSP argmax; the logits are not read)
  float top_p = -1, min_p = -1;
  int32_t top_k = -1;
  float repetition_penalty = -1;    // LFM3 recommends 1.05-1.1; the package default is used when -1
  int32_t repeat_last_n = 64;
  uint64_t seed = 0;                // 0 = random
  std::vector<std::string> stop;    // stop sequences, matched on the detokenized stream; never streamed
  bool logprobs = false;            // per generated token
  int32_t top_logprobs = 0;         // 0..20
  bool enable_thinking = false;     // chat template kwarg (HF); off renders the empty <think></think>
  std::vector<Modality> modalities{Modality::Text};  // {Audio} = TTS, {Text, Audio} = interleaved voice reply
  AudioOutputConfig audio;
  float max_audio_seconds = 30;
  VideoConfig video;
  bool cache_prompt = true;         // llama.cpp server name; false = always prefill from position 0
};

struct TokenLogprob {
  int32_t token = -1;
  std::string piece;
  float logprob = 0;
  std::vector<std::pair<int32_t, float>> top;  // top_logprobs entries
};

struct AudioChunk {
  const float* pcm = nullptr;       // mono float at sample_rate, valid during the callback
  size_t samples = 0;
  int32_t sample_rate = 24000;
  int64_t first_sample = 0;         // position of pcm[0] in the reply's audio
  bool last = false;
};

struct Usage {
  int32_t prompt_rows = 0;          // positions the request occupies (text tokens + media rows)
  int32_t cached_rows = 0;          // of which reused from the prefix cache (OpenAI prompt_tokens_details.cached_tokens)
  int32_t completion_tokens = 0, audio_frames = 0;
};

struct Response {
  Status status = Status::Ok;
  std::string error;                // why, when status != Ok
  FinishReason finish_reason = FinishReason::Stop;
  std::string text;                 // the reply text (stop sequence and end-of-turn token excluded)
  std::vector<int32_t> tokens;      // the generated ids (end-of-turn token excluded)
  std::vector<TokenLogprob> logprobs;
  std::vector<float> audio;         // 24 kHz mono, when collected
  Message message;                  // the assistant message to append to the history (carries `generated`)
  Usage usage;
  Timings timings;
};

struct StreamEvent {
  enum class Type : uint8_t { Text, Audio, Done };
  Type type = Type::Text;
  std::string_view text;            // Text: a UTF-8-complete piece
  int32_t token = -1;               // Text: the last id of the piece
  const TokenLogprob* logprob = nullptr;
  const AudioChunk* audio = nullptr;
  const Response* response = nullptr;  // Done: the final response (also returned by chat())
};
// Return false to stop (the ixian convention): the reply ends at that piece with FinishReason::Cancelled, and the
// partial reply stays in the cache, as after cancel().
using StreamCallback = std::function<bool(const StreamEvent&)>;

struct TemplateOptions {
  bool add_generation_prompt = true;
  bool enable_thinking = false;
};

// mtmd_input_chunks, summarised (LLM::inspect).
struct PromptInfo {
  struct Chunk {
    Part::Type type = Part::Type::Text;
    int32_t rows = 0;
    int32_t first_row = 0;
  };
  std::vector<Chunk> chunks;
  int32_t rows = 0, cached_rows = 0, room = 0;  // room = max_context - rows
};

// LLM::Options (defined outside the class so it can be a default argument of the factories).
struct LLMOptions {
  // ZeticMLange 0.1.10 fields first, same meaning
  std::string system_prompt;        // generate() only; chat() takes System messages
  int32_t max_context = 0;          // positions; 0 = package (2048); up to 4096 on phones, 131072 on the IQ9 (README "Context length")
  float temperature = -1;           // default for GenerationConfig::temperature (-1 = package)
  // new
  int32_t max_new_tokens = 256;     // default for GenerationConfig::max_tokens
  std::string native_lib_dir;       // where libhexrt_v<arch>_skel.so live (Android app: nativeLibraryDir); "" = next to this library
  std::string target;               // "" = detect (DESIGN.md §3.3); "v73" / "v79" / "v81" forces a target directory
  bool verify_checksums = false;    // sha256 of every file at load (~4-8 s per GB on a phone); size check always
  bool warmup = true;               // first-run DSP setup at load, outside TTFT (DESIGN.md §5.1)
  std::string host_affinity;        // "" = package ("prime" on phones), "off", or a hex CPU mask
  bool wait_if_busy = true;         // a second concurrent call waits (true) or returns Status::Busy
  LogSink log;
};

struct __attribute__((visibility("hidden"))) LLMAccess;  // internal (tools / tests), not part of the shared library's API

class LLM final {
 public:
  using Options = LLMOptions;

  // Local files only: a model root (one subdirectory per target, DESIGN.md §3.1) or one target directory.
  // Throws Error (PackageError, DeviceError, Unsupported for a d1 package).
  [[nodiscard]] static LLM fromPath(const std::string& model_path, const Options& options = {});

  // ZeticMLange 0.1.10 signatures. v0.1 resolves from FetchOptions::cache_dir only
  // (<cache_dir>/<model_name>/<target>/manifest.json) and throws Error(Unsupported) when the package is not there;
  // the download is added later behind the same call. The key is never written to a log or a file.
  [[nodiscard]] static LLM fromModelKey(const std::string& model_name, const std::string& personal_key,
                                        const std::string& native_lib_dir = "");
  [[nodiscard]] static LLM fromModelKey(const std::string& model_name, const std::string& personal_key,
                                        const std::string& native_lib_dir, const Options& options);
  [[nodiscard]] static LLM fromModelKey(const std::string& model_name, const std::string& personal_key,
                                        const std::string& native_lib_dir, const Options& options,
                                        const FetchOptions& fetch);

  ~LLM();
  LLM(LLM&&) noexcept;
  LLM& operator=(LLM&&) noexcept;
  LLM(const LLM&) = delete;
  LLM& operator=(const LLM&) = delete;

  const ModelInfo& info() const;
  Capabilities capabilities() const;

  // One request (OpenAI chat completion). Blocks until the reply ends; events stream to on_event on this thread.
  // Never throws for request errors: Response::status says what went wrong (DESIGN.md §2.9).
  Response chat(const std::vector<Message>& messages, const GenerationConfig& config = {},
                const StreamCallback& on_event = {});

  // ZeticMLange 0.1.10: one user turn appended to an internal conversation, the reply text returned.
  [[nodiscard]] std::string generate(const std::string& user_prompt);
  void setStreamSink(std::function<void(const std::string& piece)> sink);

  void cancel();   // thread-safe; the call in flight ends at its next step (DESIGN.md §2.10)
  void reset();    // drop the KV cache, the prefix cache and generate()'s conversation
  void reload();   // re-open the DSP session after Status::DeviceError

  // HF processor-style helpers (no DSP work).
  std::string applyChatTemplate(const std::vector<Message>& messages, const TemplateOptions& = {}) const;
  std::vector<int32_t> tokenize(std::string_view text, bool parse_special = true) const;
  std::string detokenize(const std::vector<int32_t>& ids) const;
  PromptInfo inspect(const std::vector<Message>& messages, const GenerationConfig& = {}) const;


 private:
  struct __attribute__((visibility("hidden"))) Impl;
  explicit LLM(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend struct LLMAccess;  // internal (tools / tests)
};

// Convenience over chat(): keeps the message list (the shape of ixian lm::Session::respond).
class Conversation {
 public:
  explicit Conversation(LLM& llm, std::string system = {});
  Response send(std::vector<Part> user, const GenerationConfig& = {}, const StreamCallback& = {});
  Response send(std::string user_text, const GenerationConfig& = {}, const StreamCallback& = {});
  const std::vector<Message>& messages() const;
  void clear();  // next send() starts a new conversation (the cache is reused only by prefix)

 private:
  LLM* llm_;
  std::vector<Message> messages_;
};

// Short names for logs.
const char* finishReasonName(FinishReason r) noexcept;

}  // namespace hx1
}  // namespace zetic::mlange
#pragma GCC visibility pop
