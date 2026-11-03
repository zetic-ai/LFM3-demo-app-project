// Persistent SDK process. stdout is NDJSON; SDK diagnostics go to stderr.
#include "vendor/json.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <zetic_decision.h>
#include <zetic_llm.h>
using namespace zetic::mlange;
using json = nlohmann::ordered_json;
namespace {
FILE *protocol;
std::mutex life, output;
std::unique_ptr<LLM> llm;
std::unique_ptr<Decision> decision;
std::map<std::string, Message> generated;
double now() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void emit(const json &value) {
  std::lock_guard<std::mutex> lock(output);
  auto s = value.dump() + "\n";
  fwrite(s.data(), 1, s.size(), protocol);
  fflush(protocol);
}
std::string b64(const std::string &data) {
  const char *abc =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned val = 0;
  int bits = -6;
  for (unsigned char c : data) {
    val = (val << 8) | c;
    bits += 8;
    while (bits >= 0) {
      out += abc[(val >> bits) & 63];
      bits -= 6;
    }
  }
  if (bits > -6)
    out += abc[((val << 8) >> (bits + 8)) & 63];
  while (out.size() % 4)
    out += '=';
  return out;
}
std::string pcm16(const float *p, size_t n) {
  std::string s(n * 2, '\0');
  for (size_t i = 0; i < n; i++) {
    int16_t v = static_cast<int16_t>(std::clamp(p[i], -1.f, 1.f) * 32767);
    s[i * 2] = v & 255;
    s[i * 2 + 1] = (v >> 8) & 255;
  }
  return s;
}
json timings(const Timings &t) {
  return {{"ttft_ms", t.ttft},
          {"tps", t.tokens_per_second},
          {"first_audio_ms", t.first_audio},
          {"audio_seconds", t.audio_seconds},
          {"realtime_x", t.realtime_factor},
          {"encode_ms", t.encode},
          {"prefill_ms", t.prefill},
          {"decode_ms", t.decode}};
}
json capabilities(const Capabilities &c) {
  return {{"text", c.text},
          {"image", c.image},
          {"audio", c.audio},
          {"video", c.video},
          {"speech", c.speech_out},
          {"decision", c.decision},
          {"max_context", c.max_context}};
}
std::vector<Part> parts(const json &msg) {
  std::vector<Part> out;
  for (const auto &a : msg.value("attachments", json::array())) {
    auto type = a.at("type").get<std::string>(),
         path = a.value("path", std::string());
    if (type == "image")
      out.push_back(Part::Img(Image::fromFile(path)));
    else if (type == "audio")
      out.push_back(Part::Aud(Audio::fromWav(path)));
    else if (type == "video") {
      std::vector<Image> frames;
      for (const auto &f : a.at("frames"))
        frames.push_back(Image::fromFile(f.get<std::string>()));
      out.push_back(
          Part::Vid(Video::fromFrames(std::move(frames), a.value("fps", 2.0))));
    }
  }
  auto text = msg.value("text", std::string());
  if (!text.empty())
    out.push_back(Part::Text(text));
  return out;
}
void run(const json &q) {
  const auto id = q.value("id", std::string()),
             op = q.at("op").get<std::string>();
  auto send = [&](std::string event, json data = json::object()) {
    data["event"] = event;
    data["id"] = id;
    emit(data);
  };
  try {
    if (op == "unload") {
      std::lock_guard<std::mutex> lock(life);
      decision.reset();
      llm.reset();
      generated.clear();
      send("done");
      return;
    }
    if (op == "load") {
      double start = now();
      {
        std::lock_guard<std::mutex> lock(life);
        decision.reset();
        llm.reset();
        generated.clear();
      }
      const auto path = q.at("path").get<std::string>();
      if (q.value("decision", false)) {
        DecisionOptions opt;
        opt.native_lib_dir = q.value("lib_dir", std::string());
        opt.set_size = q.value("set_size", 16);
        opt.calibrate = q.value("calibrate", true);
        auto loaded = std::make_unique<Decision>(Decision::fromPath(path, opt));
        auto caps = capabilities(loaded->capabilities());
        {
          std::lock_guard<std::mutex> lock(life);
          decision = std::move(loaded);
        }
        send("loaded", {{"load_ms", now() - start}, {"capabilities", caps}});
      } else {
        LLMOptions opt;
        opt.native_lib_dir = q.value("lib_dir", std::string());
        opt.max_context = q.value("context", 2048);
        auto loaded = std::make_unique<LLM>(LLM::fromPath(path, opt));
        auto caps = capabilities(loaded->capabilities());
        {
          std::lock_guard<std::mutex> lock(life);
          llm = std::move(loaded);
        }
        send("loaded", {{"load_ms", now() - start}, {"capabilities", caps}});
      }
      return;
    }
    if (op == "chat") {
      if (!llm)
        throw std::runtime_error("Generation model is not loaded");
      const auto cfg = q.value("settings", json::object());
      GenerationConfig g;
      g.max_tokens = cfg.value("max_tokens", 256);
      g.temperature = cfg.value("temperature", 0.1f);
      g.top_p = cfg.value("top_p", 0.95f);
      g.top_k = cfg.value("top_k", 50);
      g.seed = cfg.value("seed", 1ull);
      g.repetition_penalty = cfg.value("repetition_penalty", 1.1f);
      g.max_audio_seconds = cfg.value("max_audio_seconds", 30.f);
      g.enable_thinking = cfg.value("thinking", false);
      g.cache_prompt = cfg.value("cache_prompt", true);
      auto mode = q.value("output", std::string("text"));
      g.modalities.clear();
      if (mode != "audio")
        g.modalities.push_back(Modality::Text);
      if (mode != "text")
        g.modalities.push_back(Modality::Audio);
      std::vector<Message> history;
      for (const auto &m : q.at("messages")) {
        auto key = m.value("generated_id", std::string());
        auto cached = generated.find(key);
        if (cached != generated.end() &&
            cached->second.text() == m.value("text", std::string())) {
          history.push_back(cached->second);
          continue;
        }
        Message msg;
        auto role = m.value("role", std::string("user"));
        msg.role = role == "system"      ? Role::System
                   : role == "assistant" ? Role::Assistant
                                         : Role::User;
        msg.content = parts(m);
        history.push_back(std::move(msg));
      }
      if (mode != "text") {
        bool has_system = false;
        for (const auto &m : history)
          has_system = has_system || m.role == Role::System;
        if (!has_system)
          history.insert(
              history.begin(),
              Message::System(mode == "audio"
                                  ? "Perform TTS in American English using a female voice."
                                  : "Respond with interleaved text and audio."));
      }
      auto inspect = llm->inspect(history, g);
      if (inspect.rows + g.max_tokens > llm->capabilities().max_context)
        throw std::runtime_error(
            "Context exceeded: increase context or start a new conversation");
      double start = now();
      Response r = llm->chat(history, g, [&](const StreamEvent &ev) {
        if (ev.type == StreamEvent::Type::Text)
          send("text", {{"delta", std::string(ev.text)}});
        if (ev.type == StreamEvent::Type::Audio && ev.audio)
          send("audio", {{"pcm", b64(pcm16(ev.audio->pcm, ev.audio->samples))},
                         {"rate", ev.audio->sample_rate}});
        return true;
      });
      if (r.status != Status::Ok)
        throw std::runtime_error(std::string(statusName(r.status)) + ": " +
                                 r.error);
      generated[id] = r.message;
      while (generated.size() > 128)
        generated.erase(generated.begin());
      send("done", {{"text", r.text},
                    {"generated_id", id},
                    {"finish", finishReasonName(r.finish_reason)},
                    {"metrics", timings(r.timings)},
                    {"total_ms", now() - start},
                    {"usage",
                     {{"prompt_tokens", r.usage.prompt_rows},
                      {"cached_tokens", r.usage.cached_rows},
                      {"output_tokens", r.usage.completion_tokens}}}});
      return;
    }
    if (op == "decision") {
      if (!decision)
        throw std::runtime_error("Decision model is not loaded");
      State state = State::None();
      auto s = q.value("state", json());
      if (s.is_string())
        state = State::Text(s.get<std::string>());
      else if (!s.is_null())
        state = State::Json(s.dump());
      std::vector<std::pair<std::string, Question>> questions;
      for (const auto &item : q.at("questions")) {
        auto type = item.at("type").get<std::string>(),
             text = item.at("instructions").get<std::string>();
        Question question;
        if (type == "noul")
          question = Question::Noul(text);
        else if (type == "score") {
          std::vector<std::string> levels;
          for (const auto &c : item.at("criteria"))
            levels.push_back(c.at("label"));
          question = Question::Score(text, levels);
        } else {
          std::vector<std::pair<std::string, std::string>> criteria;
          for (const auto &c : item.at("criteria"))
            criteria.emplace_back(c.at("label").get<std::string>(),
                                  c.value("description", std::string()));
          question =
              Question::Choice(text, criteria, item.value("multiple", false));
          question.abstain = item.value("abstain", std::string());
        }
        questions.emplace_back(item.at("id").get<std::string>(),
                               std::move(question));
      }
      auto media =
          parts({{"attachments", q.value("attachments", json::array())}});
      double start = now();
      auto r = decision->systemOne(state, questions, media);
      if (r.status != Status::Ok)
        throw std::runtime_error(std::string(statusName(r.status)) + ": " +
                                 r.error);
      json answers = json::array();
      for (size_t i = 0; i < r.answers.size(); i++) {
        const auto &a = r.answers[i].second;
        auto out = q["questions"][i];
        out["probabilities"] = json::array();
        if (a.type == Question::Type::Noul) {
          out["selected"] = a.noul >= 0.5;
          out["probabilities"] = {
              {{"label", "true"}, {"probability", a.noul}},
              {{"label", "false"}, {"probability", 1 - a.noul}}};
        } else {
          out["selected"] = a.type == Question::Type::Choice ? json(a.choice)
                                                             : json(a.legend);
          out["choices"] = a.choices;
          out["score"] = a.score;
          out["confidence"] = a.confidence;
          for (size_t j = 0; j < a.labels.size(); j++)
            out["probabilities"].push_back(
                {{"label", a.labels[j]}, {"probability", a.probs[j]}});
        }
        answers.push_back(out);
      }
      send("done", {{"answers", answers},
                    {"passes", r.passes},
                    {"metrics", timings(r.timings)},
                    {"total_ms", now() - start},
                    {"usage", {{"prompt_tokens", r.input_tokens}}}});
      return;
    }
    throw std::runtime_error("Unknown operation");
  } catch (const std::exception &e) {
    send("error", {{"message", e.what()}});
  }
}
} // namespace
int main() {
  protocol = fdopen(dup(STDOUT_FILENO), "w");
  dup2(STDERR_FILENO, STDOUT_FILENO);
  std::mutex mu;
  std::condition_variable cv;
  std::deque<json> queue;
  bool finished = false;
  std::thread thread([&] {
    for (;;) {
      json q;
      {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return finished || !queue.empty(); });
        if (queue.empty() && finished)
          break;
        q = std::move(queue.front());
        queue.pop_front();
      }
      run(q);
    }
  });
  emit({{"event", "ready"}});
  std::string line;
  while (std::getline(std::cin, line)) {
    try {
      auto q = json::parse(line);
      if (q.value("op", std::string()) == "cancel") {
        std::lock_guard<std::mutex> lock(life);
        if (llm)
          llm->cancel();
        if (decision)
          decision->cancel();
      } else {
        std::lock_guard<std::mutex> lock(mu);
        queue.push_back(std::move(q));
        cv.notify_one();
      }
    } catch (const std::exception &e) {
      emit({{"event", "error"}, {"message", e.what()}});
    }
  }
  {
    std::lock_guard<std::mutex> lock(life);
    if (llm)
      llm->cancel();
    if (decision)
      decision->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    finished = true;
    queue.clear();
  }
  cv.notify_one();
  thread.join();
}
