// ZeticMLange Hexagon SDK — d1 decision models (LiquidAI/d1-2B, d1-Omni-2B), modelled on Liquid's HF API
// (`system_one` / `system_one_batch` / `probabilities`) and llama.cpp's /v1/systemone.
// d1 has no decode: one prefill-like pass with d1's set attention, then the logits of a few code tokens at the answer
// slots. Design: sdk/DESIGN.md §2.14.
//
//   Decision d1 = Decision::fromPath("/data/local/tmp/zsdk/models/d1-2b");
//   auto out = d1.systemOne(State::Text("I was charged twice ..."), {
//       {"refund", Question::Noul("Is the customer asking for a refund?")},
//       {"team",   Question::Choice("Which team should handle this?", {{"billing", "Charges, refunds"}, {"shipping", ""}})},
//       {"urgency", Question::Score("How urgent is this ticket?", {"low", "medium", "high"})}});
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetic_media.h"
#include "zetic_selection.h"
#include "zetic_types.h"

// Everything declared below is the public API of libzetic_mlange_hexagon.so (the library is built with -fvisibility=hidden).
#pragma GCC visibility push(default)
namespace zetic::mlange {
inline namespace hx1 {

struct Question {
  enum class Type : uint8_t { Noul, Choice, Score /* v0.2: Text, Number, Date, Box, Boxes (pointer readout) */ };
  Type type = Type::Noul;
  std::string instructions;
  // Choice: ordered label -> description ("" = none). Score: ordered levels (labels only, descriptions "").
  std::vector<std::pair<std::string, std::string>> criteria;
  bool multiple = false;            // Choice: multi-label (one slot per option)
  std::string abstain;              // Choice: optional abstain label

  static Question Noul(std::string instructions);
  static Question Choice(std::string instructions, std::vector<std::pair<std::string, std::string>> criteria,
                         bool multiple = false);
  static Question Score(std::string instructions, std::vector<std::string> levels);
};

// A state is text, a JSON document, or nothing (the media are the whole state), as in HF system_one.
struct State {
  enum class Kind : uint8_t { None, Text, Json };
  Kind kind = Kind::None;
  std::string value;                // Json: a serialized document; rendered as HF prompt.py renders a parsed one
  static State None();
  static State Text(std::string s);
  static State Json(std::string json);
};

struct Answer {
  Question::Type type = Question::Type::Noul;
  double noul = 0;                  // Noul: P(yes)
  std::string choice;               // Choice: argmax label
  std::vector<std::string> choices; // Choice multiple: labels over 0.5
  double confidence = 0;
  int32_t score = 0;                // Score: median level index
  std::string legend;               // Score: that level's label
  std::vector<std::string> labels;  // option labels in order
  std::vector<double> probs;        // calibrated probabilities per option (temperature from calibration.json)
  std::vector<float> raw_scores;    // per option: max raw logit over its code tokens (yes / Yes / YES, " A" / "A", ...)
};

struct DecisionResult {
  Status status = Status::Ok;
  std::string error;
  std::vector<std::pair<std::string, Answer>> answers;  // in request order (HF returns a dict)
  int32_t input_tokens = 0, passes = 0;
  Timings timings;                  // encode / prefill (= d1 pass) / readout
};

struct DecisionRequest {
  State state;
  std::vector<std::pair<std::string, Question>> questions;
  std::vector<Part> media;          // images, audio clips (d1-Omni), videos
};

// Decision::Options (outside the class so it can be a default argument).
struct DecisionOptions {
  int32_t set_size = 0;             // 0 = model's d1_set_size (16); 1 = one pass per question (llama.cpp behaviour)
  bool calibrate = true;            // apply calibration.json
  std::string native_lib_dir, target, host_affinity;
  bool verify_checksums = false, warmup = true;
  LogSink log;
};

struct __attribute__((visibility("hidden"))) DecisionAccess;  // internal (tools / tests), not part of the shared library's API

class Decision final {
 public:
  using Options = DecisionOptions;

  // Throws Error (PackageError, DeviceError, Unsupported for a generative package).
  [[nodiscard]] static Decision fromPath(const std::string& model_path, const Options& options = {});
  [[nodiscard]] static Decision fromModelKey(const std::string& model_name, const std::string& personal_key,
                                             const Options& options = {}, const FetchOptions& fetch = {});

  ~Decision();
  Decision(Decision&&) noexcept;
  Decision& operator=(Decision&&) noexcept;
  Decision(const Decision&) = delete;
  Decision& operator=(const Decision&) = delete;

  const ModelInfo& info() const;
  Capabilities capabilities() const;

  // HF system_one: named questions over one state, in as few passes as set_size allows. Media are encoded once per
  // request and overlaid on every pass of it.
  DecisionResult systemOne(const State& state, const std::vector<std::pair<std::string, Question>>& questions,
                           const std::vector<Part>& media = {});
  // HF system_one_batch: requests one after another (each request's state is its own d1 set).
  std::vector<DecisionResult> systemOneBatch(const std::vector<DecisionRequest>& requests);
  // HF probabilities(): calibrated per-option probabilities only.
  std::vector<std::vector<double>> probabilities(const DecisionRequest& request);

  // Raw pass (eval / custom prompts): pre-tokenized ids, state length, question groups [start, end), slot rows, media
  // rows to overlay, and the code-token ids to read per slot. Returns raw logits per slot. This is what the .seq eval
  // files already contain, so the 181 / 231 checks run before the C++ prompt renderer exists.
  struct RawPass {
    std::vector<int32_t> ids;
    int32_t state_len = 0;
    std::vector<std::pair<int32_t, int32_t>> groups;
    std::vector<int32_t> slots;
    std::vector<std::vector<int32_t>> read_ids;            // per slot
    std::vector<std::pair<int32_t, Part>> media;           // (first row, media)
  };
  std::vector<std::vector<float>> runRaw(const RawPass& pass);

  void cancel();

 private:
  struct __attribute__((visibility("hidden"))) Impl;
  explicit Decision(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend struct DecisionAccess;  // internal (tools / tests)
};

}  // namespace hx1
}  // namespace zetic::mlange
#pragma GCC visibility pop
