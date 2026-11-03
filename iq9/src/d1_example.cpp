// Two D1 Decision API examples using the same SDK library as the LFM3 CLI.
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zetic_decision.h>

using namespace zetic::mlange;

int main(int argc, char** argv) {
  std::string model, mode, wav, ppm, text_file, ticket = "I was charged twice and my parcel never arrived. Please refund one charge.";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::puts("usage: d1_example --model <d1 model dir> --mode ticket|image|audio [--text-file <ticket.txt>] [--ppm <image.ppm>] [--wav <input.wav>]");
      return 0;
    }
    if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", arg.c_str()); return 2; }
    if (arg == "--model") model = argv[++i];
    else if (arg == "--mode") mode = argv[++i];
    else if (arg == "--wav") wav = argv[++i];
    else if (arg == "--ppm") ppm = argv[++i];
    else if (arg == "--text-file") text_file = argv[++i];
    else { std::fprintf(stderr, "unknown option %s\n", arg.c_str()); return 2; }
  }
  if (model.empty() || (mode != "ticket" && mode != "image" && mode != "audio") ||
      (mode == "audio" && wav.empty()) || (mode == "image" && ppm.empty())) {
    std::fprintf(stderr, "usage: d1_example --model <d1 model dir> --mode ticket|image|audio [--text-file <ticket.txt>] [--ppm <image.ppm>] [--wav <input.wav>]\n");
    return 2;
  }
  try {
    if (mode == "ticket" && !text_file.empty()) {
      std::ifstream in(text_file);
      if (!in) { std::fprintf(stderr, "cannot read %s\n", text_file.c_str()); return 2; }
      std::ostringstream contents; contents << in.rdbuf(); ticket = contents.str();
    }
    Decision d = Decision::fromPath(model);
    State state = State::Text(ticket);
    std::vector<Part> media;
    std::vector<std::pair<std::string, Question>> questions;
    if (mode == "ticket") {
      questions = {{"refund", Question::Noul("Is the customer asking for a refund?")},
                   {"team", Question::Choice("Which team should handle this?",
                                              {{"billing", "Charges, refunds, invoices"},
                                               {"shipping", "Delivery and parcels"},
                                               {"technical", "App or site faults"}})},
                   {"urgency", Question::Score("How urgent is this ticket?", {"low", "medium", "high"})}};
    } else if (mode == "audio") {
      state = State::None();
      media.push_back(Part::Aud(Audio::fromWav(wav)));
      questions = {{"speech", Question::Noul("Is this a formal public speech in English?")},
                   {"topic", Question::Choice("What is the speaker discussing?",
                                              {{"civic_duty", "What citizens can do for their country"},
                                               {"weather", "Weather conditions"},
                                               {"sports", "Sports events"}})}};
    } else {
      state = State::None();
      media.push_back(Part::Img(Image::fromFile(ppm)));
      questions = {{"cat_count", Question::Choice("How many cats are in this image?",
                                                  {{"0", "No cats"}, {"1", "One cat"},
                                                   {"2", "Two cats"}, {"3", "Three cats"}})},
                   {"animal", Question::Choice("What animals are visible?",
                                               {{"cats", "Cats"}, {"dogs", "Dogs"}, {"other", "Other animals"}})}};
    }
    const DecisionResult r = d.systemOne(state, questions, media);
    if (r.status != Status::Ok) {
      std::fprintf(stderr, "D1 failed (%s): %s\n", statusName(r.status), r.error.c_str());
      return 1;
    }
    for (size_t i = 0; i < r.answers.size(); ++i) {
      const auto& a = r.answers[i].second;
      std::printf("%s\n", questions[i].second.instructions.c_str());
      if (a.type == Question::Type::Noul) {
        std::printf("Selected: %s\n  true: %.3f  false: %.3f\n\n", a.noul >= 0.5 ? "true" : "false", a.noul, 1.0 - a.noul);
      } else {
        const std::string& selected = a.type == Question::Type::Choice ? a.choice : a.legend;
        std::printf("Selected: %s (%.3f)\n", selected.c_str(), a.confidence);
        for (size_t j = 0; j < a.labels.size() && j < a.probs.size(); ++j)
          std::printf("  %s: %.3f\n", a.labels[j].c_str(), a.probs[j]);
        std::puts("");
      }
    }
    std::printf("decision %.1f ms, %d pass(es)\n", r.timings.ttft, r.passes);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "D1 error: %s\n", e.what());
    return 1;
  }
}
