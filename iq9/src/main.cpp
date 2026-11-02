// LFM3 demo CLI for the Dragonwing IQ9 (ZeticMLange Hexagon SDK 0.1.0). File input only; GNU toolchain + the SDK.
//   lfm3_cli --model <dir> [options] ["prompt"]            one-shot (or chat REPL with -i)
//   lfm3_cli --model <dir> --sample chat|summary|image|image_count|image_ocr|asr|tts|vl_audio|voice
#include <zetic_llm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace zetic::mlange;

namespace {

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- P6 PPM (512 x 512, maxval 255) ----
Image readPpm(const std::string& path) {
  std::string d = slurp(path);
  size_t pos = 0;
  auto token = [&]() {
    for (;;) {
      while (pos < d.size() && isspace(static_cast<unsigned char>(d[pos]))) ++pos;
      if (pos < d.size() && d[pos] == '#') { while (pos < d.size() && d[pos] != '\n') ++pos; } else break;
    }
    size_t s = pos;
    while (pos < d.size() && !isspace(static_cast<unsigned char>(d[pos]))) ++pos;
    return d.substr(s, pos - s);
  };
  if (token() != "P6") throw std::runtime_error(path + ": not a binary PPM (P6)");
  int w = std::stoi(token()), h = std::stoi(token());
  if (token() != "255") throw std::runtime_error(path + ": maxval must be 255");
  ++pos;
  if (w != 512 || h != 512) throw std::runtime_error(path + ": must be 512x512 (the SDK never resizes), got " + std::to_string(w) + "x" + std::to_string(h));
  if (d.size() < pos + static_cast<size_t>(w) * h * 3) throw std::runtime_error(path + ": truncated");
  return Image::fromRgb(reinterpret_cast<const uint8_t*>(d.data()) + pos, w, h);
}

// ---- WAV in: PCM 8/16/24/32 bit or float, any rate / channels -> Audio::fromPcm (the SDK resamples to 16 kHz) ----
Audio readWav(const std::string& path) {
  std::string d = slurp(path);
  auto u16 = [&](size_t o) { return static_cast<uint16_t>(static_cast<uint8_t>(d[o]) | (static_cast<uint8_t>(d[o + 1]) << 8)); };
  auto u32 = [&](size_t o) { return static_cast<uint32_t>(u16(o)) | (static_cast<uint32_t>(u16(o + 2)) << 16); };
  if (d.size() < 44 || d.compare(0, 4, "RIFF") != 0 || d.compare(8, 4, "WAVE") != 0) throw std::runtime_error(path + ": not a WAV file");
  size_t pos = 12;
  int fmt = 1, ch = 1, bits = 16;
  uint32_t rate = 16000;
  while (pos + 8 <= d.size()) {
    uint32_t sz = u32(pos + 4);
    size_t body = pos + 8;
    if (sz > d.size() - body) throw std::runtime_error(path + ": truncated WAV chunk");
    if (d.compare(pos, 4, "fmt ") == 0) {
      if (sz < 16) throw std::runtime_error(path + ": truncated WAV format");
      fmt = u16(body); ch = u16(body + 2); rate = u32(body + 4); bits = u16(body + 14);
      if (fmt == 0xFFFE) { if (sz < 26) throw std::runtime_error(path + ": truncated WAV extension"); fmt = u16(body + 24); }
    } else if (d.compare(pos, 4, "data") == 0) {
      if (ch <= 0 || rate == 0 || (fmt != 1 && fmt != 3) || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || (fmt == 3 && bits != 32))
        throw std::runtime_error(path + ": unsupported WAV format");
      size_t n = std::min<size_t>(sz, d.size() - body) / (bits / 8);
      std::vector<float> s(n);
      for (size_t i = 0; i < n; ++i) {
        const char* p = d.data() + body + i * (bits / 8);
        if (fmt == 3 && bits == 32) { float f; memcpy(&f, p, 4); s[i] = f; }
        else if (bits == 16) { int16_t v; memcpy(&v, p, 2); s[i] = v / 32768.f; }
        else if (bits == 8) s[i] = (static_cast<uint8_t>(*p) - 128) / 128.f;
        else if (bits == 24) { int32_t v = (static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8) | (p[2] << 16)); s[i] = v / 8388608.f; }
        else if (bits == 32) { int32_t v; memcpy(&v, p, 4); s[i] = v / 2147483648.f; }
        else throw std::runtime_error(path + ": unsupported WAV format");
      }
      double secs = static_cast<double>(n) / ch / rate;
      if (secs > 30) fprintf(stderr, "note: %s is %.1f s; the SDK takes at most 30 s per clip, using the first 30 s\n", path.c_str(), secs);
      if (secs > 30) s.resize(static_cast<size_t>(30.0 * rate * ch));
      return Audio::fromPcm(s.data(), s.size() / ch, static_cast<int32_t>(rate), ch);
    }
    pos = body + sz + (sz & 1);
  }
  throw std::runtime_error(path + ": no data chunk");
}

// ---- WAV out: 24 kHz mono PCM16, chunks appended as they arrive, header fixed at the end ----
class WavWriter {
 public:
  void open(const std::string& path, int rate) {
    f_ = fopen(path.c_str(), "wb");
    if (!f_) throw std::runtime_error("cannot write " + path);
    rate_ = rate;
    char h[44] = {};
    fwrite(h, 1, 44, f_);
  }
  void write(const float* pcm, size_t n) {
    std::vector<int16_t> s(n);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<int16_t>(std::max(-1.f, std::min(1.f, pcm[i])) * 32767.f);
    fwrite(s.data(), 2, n, f_);
    samples_ += n;
  }
  void close() {
    if (!f_) return;
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f_); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f_); };
    fseek(f_, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f_); w32(static_cast<uint32_t>(36 + samples_ * 2)); fwrite("WAVEfmt ", 1, 8, f_);
    w32(16); w16(1); w16(1); w32(rate_); w32(rate_ * 2); w16(2); w16(16); fwrite("data", 1, 4, f_); w32(static_cast<uint32_t>(samples_ * 2));
    fclose(f_); f_ = nullptr;
  }
  ~WavWriter() { close(); }
  size_t samples() const { return samples_; }
 private:
  FILE* f_ = nullptr;
  int rate_ = 24000;
  size_t samples_ = 0;
};

std::vector<std::string> listPpm(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ppm") == 0) v.push_back(dir + "/" + n);
    }
    closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

struct Options {
  std::string model, lib_dir, sample, samples_dir, image, audio, video_dir, tts_out, voice_out, prompt;
  bool interactive = false, verify = false;
  int ctx = 0, max_tokens = -1, top_k = -1;
  float temperature = -1, repetition_penalty = -1, top_p = -1;
  uint64_t seed = 0;
  double fps = 2.0;
};

void usage(const char* a0) {
  fprintf(stderr,
    "usage: %s --model <model dir> [options] [\"prompt\"]\n"
    "  -i                    chat REPL (commands: /image f.ppm, /audio f.wav, /reset, /exit)\n"
    "  --image f.ppm         attach a 512x512 binary PPM (P6)\n"
    "  --video-frames <dir>  attach a video: the 512x512 *.ppm files in <dir> in name order; --fps N (default 2)\n"
    "  --audio in.wav        attach speech (any rate / channels, up to 30 s)\n"
    "  --tts out.wav         reply as speech only (text-to-speech), written to out.wav as the chunks arrive\n"
    "  --voice out.wav       reply as text and speech (voice chat)\n"
    "  --sample <name>       run a sample scenario: chat summary image image_count image_ocr asr tts vl_audio voice\n"
    "  --samples-dir <dir>   default: ../samples next to this program's folder\n"
    "  --temperature t --top-k k --top-p p --repetition-penalty r --max-tokens n --seed s\n"
    "  --ctx n               max context (default 4096 here; the SDK allows up to 131072 on the IQ9)\n"
    "  --lib-dir dir         where libhexrt_v73_skel.so is (default: next to the SDK library)\n"
    "  --verify              check the package checksums at load\n", a0);
}

void printTimings(const Response& r) {
  printf("\n[TTFT %.0f ms", r.timings.ttft);
  if (r.timings.decode_steps > 0) printf(", %.1f tokens/s", r.timings.tokens_per_second);
  if (r.timings.audio_seconds > 0) printf(", first audio %.0f ms, %.1f s speech (x%.2f real time)", r.timings.first_audio, r.timings.audio_seconds, r.timings.realtime_factor);
  printf(", %d prompt rows (%d cached), %s]\n", r.usage.prompt_rows, r.usage.cached_rows, finishReasonName(r.finish_reason));
}

struct Turn { std::string text, image, audio; };

// One request: stream text to stdout, write speech chunks into `wav_path` as they arrive.
Response runTurn(LLM& llm, std::vector<Message>& history, std::vector<Part> parts, GenerationConfig cfg, const std::string& wav_path) {
  if (history.empty() && !wav_path.empty()) {
    const bool text = std::find(cfg.modalities.begin(), cfg.modalities.end(), Modality::Text) != cfg.modalities.end();
    history.push_back(Message::System(text ? "Respond with interleaved text and audio." : "Perform TTS in American English using a female voice."));
  }
  history.push_back(Message::User(std::move(parts)));
  WavWriter wav;
  if (!wav_path.empty()) wav.open(wav_path, 24000);
  Response r = llm.chat(history, cfg, [&](const StreamEvent& ev) {
    if (ev.type == StreamEvent::Type::Text) { fwrite(ev.text.data(), 1, ev.text.size(), stdout); fflush(stdout); }
    else if (ev.type == StreamEvent::Type::Audio && ev.audio && ev.audio->samples) wav.write(ev.audio->pcm, ev.audio->samples);
    return true;
  });
  wav.close();
  if (r.status != Status::Ok) {
    history.pop_back();
    fprintf(stderr, "\nerror (%s): %s\n", statusName(r.status), r.error.c_str());
    return r;
  }
  history.push_back(r.message);
  printTimings(r);
  if (!wav_path.empty()) printf("[speech written to %s]\n", wav_path.c_str());
  return r;
}

int runSample(LLM& llm, const Options& o, GenerationConfig cfg) {
  std::string dir = o.samples_dir + "/" + o.sample;
  std::string expected = slurp(dir + "/expected.txt"), turns = slurp(dir + "/turns.txt");
  printf("== sample %s\nexpected: %s\n\n", o.sample.c_str(), expected.c_str());
  std::vector<std::string> blocks;
  std::stringstream ss(turns);
  std::string line, cur;
  while (std::getline(ss, line)) {
    if (line == "---") { blocks.push_back(cur); cur.clear(); } else cur += line + "\n";
  }
  blocks.push_back(cur);
  std::vector<Message> history;
  std::string out = "text";
  for (size_t b = 0; b < blocks.size(); ++b) {
    std::string text, text_file, image, audio;
    std::stringstream bs(blocks[b]);
    while (std::getline(bs, line)) {
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "out" && b == 0) out = v; else if (k == "text") text = v; else if (k == "text_file") text_file = v;
      else if (k == "image") image = v; else if (k == "audio") audio = v;
    }
    if (!text_file.empty()) { std::string t = slurp(dir + "/" + text_file); while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back(); text += (text.empty() ? "" : "\n\n") + t; }
    std::vector<Part> parts;
    if (!image.empty()) parts.push_back(Part::Img(readPpm(dir + "/" + image)));
    if (!audio.empty()) parts.push_back(Part::Aud(readWav(dir + "/" + audio)));
    if (!text.empty()) parts.push_back(Part::Text(text));
    GenerationConfig c = cfg;
    if (c.max_tokens < 0) c.max_tokens = (o.sample == "summary") ? 200 : 256;
    c.modalities.clear();
    if (out != "audio") c.modalities.push_back(Modality::Text);
    if (out != "text") c.modalities.push_back(Modality::Audio);
    std::string wav = (out != "text") ? "sample_" + o.sample + ".wav" : "";
    printf("--- turn %zu: %.100s\n", b + 1, text.c_str());
    if (runTurn(llm, history, std::move(parts), c, wav).status != Status::Ok) return 1;
    printf("\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  o.ctx = 4096;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(argv[0]); exit(2); } return argv[++i]; };
    if (a == "--model") o.model = next();
    else if (a == "-i" || a == "--chat") o.interactive = true;
    else if (a == "--image") o.image = next();
    else if (a == "--audio") o.audio = next();
    else if (a == "--video-frames") o.video_dir = next();
    else if (a == "--fps") o.fps = std::stod(next());
    else if (a == "--tts") o.tts_out = next();
    else if (a == "--voice") o.voice_out = next();
    else if (a == "--sample") o.sample = next();
    else if (a == "--samples-dir") o.samples_dir = next();
    else if (a == "--temperature") o.temperature = std::stof(next());
    else if (a == "--top-k") o.top_k = std::stoi(next());
    else if (a == "--top-p") o.top_p = std::stof(next());
    else if (a == "--repetition-penalty") o.repetition_penalty = std::stof(next());
    else if (a == "--max-tokens") o.max_tokens = std::stoi(next());
    else if (a == "--seed") o.seed = std::stoull(next());
    else if (a == "--ctx") o.ctx = std::stoi(next());
    else if (a == "--lib-dir") o.lib_dir = next();
    else if (a == "--verify") o.verify = true;
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(argv[0]); return 2; }
    else o.prompt = a;
  }
  if (o.model.empty()) { usage(argv[0]); return 2; }
  if (o.samples_dir.empty()) {
    std::string self = argv[0];
    size_t s = self.rfind('/');
    o.samples_dir = (s == std::string::npos ? "." : self.substr(0, s)) + "/../../samples";
  }
  try {
    LLMOptions lo;
    lo.max_context = o.ctx;
    lo.native_lib_dir = o.lib_dir;
    lo.verify_checksums = o.verify;
    lo.max_new_tokens = 512;
    LLM llm = LLM::fromPath(o.model, lo);
    const auto& info = llm.info();
    printf("%s on %s (v%d, %d NSP), context %d\n", info.model_id.c_str(), info.soc.c_str(), info.hexagon_arch, info.nsp_count, llm.capabilities().max_context);

    GenerationConfig cfg;
    cfg.max_tokens = o.max_tokens; cfg.temperature = o.temperature; cfg.top_k = o.top_k; cfg.top_p = o.top_p;
    cfg.repetition_penalty = o.repetition_penalty; cfg.seed = o.seed;
    if (!o.sample.empty()) return runSample(llm, o, cfg);

    std::string wav = !o.tts_out.empty() ? o.tts_out : o.voice_out;
    cfg.modalities.clear();
    if (o.tts_out.empty()) cfg.modalities.push_back(Modality::Text);
    if (!wav.empty()) cfg.modalities.push_back(Modality::Audio);

    auto media = [&](std::vector<Part>& parts, const std::string& image, const std::string& audio, const std::string& vdir) {
      if (!vdir.empty()) {
        std::vector<Image> frames;
        for (auto& p : listPpm(vdir)) frames.push_back(readPpm(p));
        if (frames.empty()) throw std::runtime_error("no .ppm frames in " + vdir);
        parts.push_back(Part::Vid(Video::fromFrames(std::move(frames), o.fps)));
      }
      if (!image.empty()) parts.push_back(Part::Img(readPpm(image)));
      if (!audio.empty()) parts.push_back(Part::Aud(readWav(audio)));
    };

    std::vector<Message> history;
    if (!o.interactive) {
      std::vector<Part> parts;
      media(parts, o.image, o.audio, o.video_dir);
      std::string text = o.prompt.empty() && o.audio.empty() && o.image.empty() && o.video_dir.empty() ? "What is the capital of France?" : o.prompt;
      if (!text.empty()) parts.push_back(Part::Text(text));
      return runTurn(llm, history, std::move(parts), cfg, wav).status == Status::Ok ? 0 : 1;
    }
    std::string image = o.image, audio = o.audio, vdir = o.video_dir, line;
    printf("chat: type a message; /image f.ppm  /audio f.wav  /reset  /exit\n");
    for (;;) {
      printf("\n> "); fflush(stdout);
      if (!std::getline(std::cin, line) || line == "/exit") break;
      if (line.rfind("/image ", 0) == 0) { image = line.substr(7); printf("image attached to the next message\n"); continue; }
      if (line.rfind("/audio ", 0) == 0) { audio = line.substr(7); printf("audio attached to the next message\n"); continue; }
      if (line == "/reset") { history.clear(); llm.reset(); printf("new conversation\n"); continue; }
      if (line.empty() && audio.empty()) continue;
      try {
        std::vector<Part> parts;
        media(parts, image, audio, vdir);
        if (!line.empty()) parts.push_back(Part::Text(line));
        runTurn(llm, history, std::move(parts), cfg, wav);
      } catch (const std::exception& e) { fprintf(stderr, "error: %s\n", e.what()); }
      image.clear(); audio.clear(); vdir.clear();
    }
  } catch (const Error& e) {
    fprintf(stderr, "error (%s): %s\n", statusName(e.status()), e.what());
    return 1;
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
