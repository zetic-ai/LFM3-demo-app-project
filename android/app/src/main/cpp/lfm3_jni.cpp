// JNI bridge between Kotlin and the ZeticMLange Hexagon SDK (zetic::mlange::LLM). Kotlin wrapper: Hexagon.kt.
// One model is active per process. The conversation (messages with their images / audio / video) is kept here;
// every chat() sends the whole history and the SDK reuses the cached prefix (README: requests are stateless).
// Flow of one turn: addText / addImage / addAudio / addVideo (in prompt order) -> chat(cfg, stop, listener).
#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>

#include <zetic_llm.h>
#include <zetic_decision.h>

using namespace zetic::mlange;

#define TAG "LFM3Demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {
std::unique_ptr<LLM> g_llm;
std::unique_ptr<Decision> g_decision;
std::string g_decision_path;
std::vector<Message> g_history;
std::vector<Part> g_pending;  // parts of the user turn being built
std::mutex g_mu;              // serializes model operations
std::mutex g_lifetime_mu;     // protects the model pointer against cancel racing load/unload

std::string str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r(c);
  env->ReleaseStringUTFChars(s, c);
  return r;
}
jstring jstr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }
// Error text for Kotlin: "<StatusName>|<dsp error>|<message>"
std::string errText(const Error& e) {
  return std::string(statusName(e.status())) + "|" + std::to_string(e.dspError()) + "|" + e.what();
}
}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_load(JNIEnv* env, jclass, jstring modelDir, jstring libDir,
                                                              jint maxContext, jstring target, jboolean verify,
                                                              jboolean warmup, jstring hostAffinity, jint maxNew, jstring systemPrompt,
                                                              jfloat temperature, jboolean waitIfBusy) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    { std::lock_guard<std::mutex> life(g_lifetime_mu); g_llm.reset(); }
    g_history.clear();
    g_pending.clear();
    LLMOptions opt;
    opt.system_prompt = str(env, systemPrompt);
    opt.temperature = temperature;
    opt.wait_if_busy = waitIfBusy;
    opt.native_lib_dir = str(env, libDir);  // where libhexrt_v<arch>_skel.so were extracted (nativeLibraryDir)
    opt.max_context = maxContext;
    opt.target = str(env, target);
    opt.verify_checksums = verify;
    opt.warmup = warmup;
    opt.host_affinity = str(env, hostAffinity);
    if (maxNew > 0) opt.max_new_tokens = maxNew;
    opt.log = [](LogLevel l, std::string_view m) {
      __android_log_print(l >= LogLevel::Warn ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "zetic-sdk", "%.*s",
                          static_cast<int>(m.size()), m.data());
    };
    auto model = std::make_unique<LLM>(LLM::fromPath(str(env, modelDir), opt));
    { std::lock_guard<std::mutex> life(g_lifetime_mu); g_llm = std::move(model); }
    return jstr(env, "");
  } catch (const Error& e) {
    LOGI("load failed: %s", errText(e).c_str());
    return jstr(env, errText(e));
  } catch (const std::exception& e) {
    return jstr(env, std::string("Internal|0|") + e.what());
  }
}

// key=value lines: model_id, soc, arch, nsp, text/image/audio/video/speech_out, max_context, sdk_version
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_info(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_llm) return jstr(env, "");
  const auto& i = g_llm->info();
  const auto c = g_llm->capabilities();
  std::string s = "model_id=" + i.model_id + "\nsoc=" + i.soc + "\narch=" + std::to_string(i.hexagon_arch) +
                  "\nnsp=" + std::to_string(i.nsp_count) + "\nsdk_version=" + i.sdk_version +
                  "\nrevision=" + i.revision + "\ntarget=" + i.target + "\npackage_dir=" + i.package_dir +
                  "\npackage_version=" + i.package_version + "\nruntime_commit=" + i.runtime_commit +
                  "\ntext=" + std::to_string(c.text) + "\nimage=" + std::to_string(c.image) +
                  "\naudio=" + std::to_string(c.audio) + "\nvideo=" + std::to_string(c.video) +
                  "\nspeech_out=" + std::to_string(c.speech_out) + "\nmax_context=" + std::to_string(c.max_context) +
                  "\nmulti_turn=" + std::to_string(c.multi_turn) + "\nmedia_in_followup_turns=" + std::to_string(c.media_in_followup_turns) +
                  "\nmax_media_per_turn=" + std::to_string(c.max_media_per_turn) + "\ninput_sample_rate=" + std::to_string(c.input_sample_rate) +
                  "\nspeech_sample_rate=" + std::to_string(c.speech_sample_rate) + "\n";
  return jstr(env, s);
}

// Destroys the LLM object: this frees the NPU.
JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_unload(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  { std::lock_guard<std::mutex> life(g_lifetime_mu); g_llm.reset(); }
  g_history.clear();
  g_pending.clear();
  LOGI("unloaded");
}

// New conversation: drops the history, the pending turn and the SDK's KV / prefix cache.
JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_reset(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_history.clear();
  g_pending.clear();
  if (g_llm) g_llm->reset();
}

JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_cancel(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> life(g_lifetime_mu);
  if (g_llm) g_llm->cancel();  // SDK cancellation is thread-safe during chat
}

JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_decisionCancel(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> life(g_lifetime_mu);
  if (g_decision) g_decision->cancel();
}

JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_decisionLoad(JNIEnv* env, jclass, jstring modelDir, jstring libDir) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    const auto path = str(env, modelDir);
    if (g_decision && g_decision_path == path) return jstr(env, "Ok|0");
    if (g_llm) return jstr(env, "DeviceError|0|close the LFM3 model before loading D1");
    { std::lock_guard<std::mutex> life(g_lifetime_mu); g_decision.reset(); g_decision_path.clear(); }
    DecisionOptions options;
    options.native_lib_dir = str(env, libDir);
    auto decision = std::make_unique<Decision>(Decision::fromPath(path, options));
    { std::lock_guard<std::mutex> life(g_lifetime_mu); g_decision = std::move(decision); g_decision_path = path; }
    return jstr(env, "Ok|1");
  } catch (const Error& e) { return jstr(env, errText(e)); }
  catch (const std::exception& e) { return jstr(env, std::string("Internal|0|") + e.what()); }
}

JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_decisionUnload(JNIEnv*, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::lock_guard<std::mutex> life(g_lifetime_mu);
  g_decision.reset();
  g_decision_path.clear();
}

// One D1 decision request. Return status|milliseconds|passes|human-readable answers.
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_decision(JNIEnv* env, jclass, jstring modelDir,
                                                                 jstring libDir, jstring kind, jstring state,
                                                                 jfloatArray pcm, jint rate, jint channels,
                                                                 jbyteArray rgb, jint width, jint height) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    if (g_llm) return jstr(env, "DeviceError|0|close the LFM3 model before loading D1");
    if (!g_decision || g_decision_path != str(env, modelDir))
      return jstr(env, "DeviceError|0|D1 model is not prepared");
    const std::string sample = str(env, kind);
    std::vector<std::pair<std::string, Question>> questions;
    std::vector<Part> media;
    State input = State::Text(str(env, state));
    if (sample == "d1_text") {
      questions = {
          {"refund", Question::Noul("Is the customer asking for a refund?")},
          {"team", Question::Choice("Which team should handle this?", {{"billing", "Charges, refunds, invoices"},
                                                                   {"shipping", "Delivery and parcels"},
                                                                   {"technical", "App or site faults"}})},
          {"urgency", Question::Score("How urgent is this ticket?", {"low", "medium", "high"})}};
    } else if (sample == "d1_audio" && pcm && rate > 0 && channels > 0) {
      input = State::None();
      const jsize n = env->GetArrayLength(pcm);
      if (n == 0 || n % channels) throw std::invalid_argument("invalid audio samples");
      std::vector<float> samples(n);
      env->GetFloatArrayRegion(pcm, 0, n, samples.data());
      media.push_back(Part::Aud(Audio::fromPcm(samples.data(), n / channels, rate, channels)));
      questions = {{"speech", Question::Noul("Is this a formal public speech in English?")},
                   {"topic", Question::Choice("What is the speaker discussing?",
                                              {{"civic_duty", "What citizens can do for their country"},
                                               {"weather", "Weather conditions"},
                                               {"sports", "Sports events"}})}};
    } else if (sample == "d1_image" && rgb && width == 512 && height == 512) {
      input = State::None();
      std::vector<uint8_t> pixels(512 * 512 * 3);
      if (env->GetArrayLength(rgb) != static_cast<jsize>(pixels.size()))
        throw std::invalid_argument("invalid image pixels");
      env->GetByteArrayRegion(rgb, 0, pixels.size(), reinterpret_cast<jbyte*>(pixels.data()));
      media.push_back(Part::Img(Image::fromRgb(pixels.data(), width, height)));
      questions = {{"cat_count", Question::Choice("How many cats are in this image?",
                                                 {{"0", "No cats"}, {"1", "One cat"},
                                                  {"2", "Two cats"}, {"3", "Three cats"}})},
                   {"animal", Question::Choice("What animals are visible?",
                                               {{"cats", "Cats"}, {"dogs", "Dogs"}, {"other", "Other animals"}})}};
    } else {
      throw std::invalid_argument("unknown D1 sample or missing audio");
    }
    DecisionResult result = g_decision->systemOne(input, questions, media);
    if (result.status != Status::Ok) return jstr(env, std::string(statusName(result.status)) + "|0|" + result.error);
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    for (size_t i = 0; i < result.answers.size(); ++i) {
      const auto& [name, answer] = result.answers[i];
      out << questions[i].second.instructions << "\n";
      if (answer.type == Question::Type::Noul) {
        out << "Selected: " << (answer.noul >= 0.5 ? "true" : "false") << "\n"
            << "  true: " << answer.noul << "  false: " << (1.0 - answer.noul) << "\n";
      } else {
        out << "Selected: " << (answer.type == Question::Type::Choice ? answer.choice : answer.legend)
            << " (" << answer.confidence << ")\n";
        for (size_t j = 0; j < answer.labels.size() && j < answer.probs.size(); ++j)
          out << "  " << answer.labels[j] << ": " << answer.probs[j] << "\n";
      }
      out << '\n';
    }
    return jstr(env, "Ok|" + std::to_string(result.timings.ttft) + "|" +
                         std::to_string(result.passes) + "|" + out.str());
  } catch (const Error& e) {
    return jstr(env, errText(e));
  } catch (const std::exception& e) {
    return jstr(env, std::string("Internal|0|") + e.what());
  }
}

JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_addSystem(JNIEnv* env, jclass, jstring text) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_history.push_back(Message::System(str(env, text)));
}

JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_appendMessage(JNIEnv* env, jclass, jint role) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (role < 0 || role > 2 || g_pending.empty()) return jstr(env, "InvalidArgument|0|invalid role or empty message");
  Message message;
  message.role = static_cast<Role>(role);
  message.content = std::move(g_pending);
  g_pending.clear();
  g_history.push_back(std::move(message));
  return jstr(env, "");
}

JNIEXPORT void JNICALL Java_ai_zetic_lfm3demo_Native_addText(JNIEnv* env, jclass, jstring text) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_pending.push_back(Part::Text(str(env, text)));
}

// rgb = width*height*3 interleaved bytes; the SDK needs exactly 512 x 512 and never resizes.
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_addImage(JNIEnv* env, jclass, jbyteArray rgb, jint w, jint h) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    if (!rgb || w != 512 || h != 512 || env->GetArrayLength(rgb) != 512 * 512 * 3)
      return jstr(env, "InvalidArgument|0|image must be 512x512 RGB");
    std::vector<uint8_t> px(env->GetArrayLength(rgb));
    env->GetByteArrayRegion(rgb, 0, static_cast<jsize>(px.size()), reinterpret_cast<jbyte*>(px.data()));
    g_pending.push_back(Part::Img(Image::fromRgb(px.data(), w, h)));
    return jstr(env, "");
  } catch (const Error& e) { return jstr(env, errText(e)); }
}

// Float PCM (mono or interleaved), any rate: the SDK mixes to mono and resamples to 16 kHz; up to 30 s.
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_addAudio(JNIEnv* env, jclass, jfloatArray pcm, jint rate,
                                                                  jint channels) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    if (!pcm || rate <= 0 || channels <= 0 || env->GetArrayLength(pcm) % channels != 0)
      return jstr(env, "InvalidArgument|0|invalid PCM format");
    std::vector<float> s(env->GetArrayLength(pcm));
    env->GetFloatArrayRegion(pcm, 0, static_cast<jsize>(s.size()), s.data());
    g_pending.push_back(Part::Aud(Audio::fromPcm(s.data(), s.size() / channels, rate, channels)));
    return jstr(env, "");
  } catch (const Error& e) { return jstr(env, errText(e)); }
}

// frames: Array<ByteArray>, each 512*512*3 RGB; fps = rate of the frames.
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_addVideo(JNIEnv* env, jclass, jobjectArray frames, jint w, jint h,
                                                                  jdouble fps) {
  std::lock_guard<std::mutex> lk(g_mu);
  try {
    std::vector<Image> fr;
    const jsize n = env->GetArrayLength(frames);
    for (jsize i = 0; i < n; ++i) {
      auto a = static_cast<jbyteArray>(env->GetObjectArrayElement(frames, i));
      if (!a || w != 512 || h != 512 || env->GetArrayLength(a) != 512 * 512 * 3) {
        if (a) env->DeleteLocalRef(a);
        return jstr(env, "InvalidArgument|0|video frames must be 512x512 RGB");
      }
      std::vector<uint8_t> px(env->GetArrayLength(a));
      env->GetByteArrayRegion(a, 0, static_cast<jsize>(px.size()), reinterpret_cast<jbyte*>(px.data()));
      env->DeleteLocalRef(a);
      fr.push_back(Image::fromRgb(px.data(), w, h));
    }
    g_pending.push_back(Part::Vid(Video::fromFrames(std::move(fr), fps)));
    return jstr(env, "");
  } catch (const Error& e) { return jstr(env, errText(e)); }
}

JNIEXPORT jint JNICALL Java_ai_zetic_lfm3demo_Native_tokenCount(JNIEnv* env, jclass, jstring text) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_llm ? static_cast<jint>(g_llm->tokenize(str(env, text)).size()) : -1;
}

JNIEXPORT jintArray JNICALL Java_ai_zetic_lfm3demo_Native_tokenize(JNIEnv* env, jclass, jstring text, jboolean special) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto tokens = g_llm ? g_llm->tokenize(str(env, text), special) : std::vector<int32_t>{};
  auto result = env->NewIntArray(static_cast<jsize>(tokens.size()));
  if (!tokens.empty()) env->SetIntArrayRegion(result, 0, static_cast<jsize>(tokens.size()), tokens.data());
  return result;
}
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_detokenize(JNIEnv* env, jclass, jintArray input) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<int32_t> tokens(env->GetArrayLength(input));
  env->GetIntArrayRegion(input, 0, static_cast<jsize>(tokens.size()), tokens.data());
  return jstr(env, g_llm ? g_llm->detokenize(tokens) : "");
}
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_reload(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_llm) return jstr(env, "DeviceError|0|no model loaded");
  try { g_llm->reload(); return jstr(env, ""); }
  catch (const Error& e) { return jstr(env, errText(e)); }
  catch (const std::exception& e) { return jstr(env, std::string("Internal|0|") + e.what()); }
}

JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_chatTemplate(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lk(g_mu);
  return jstr(env, g_llm ? g_llm->applyChatTemplate(g_history) : "");
}

// Sends the pending parts as one user message. cfg indices (-1 = package default where the SDK has one):
//  0 max_tokens 1 temperature 2 top_p 3 min_p 4 top_k 5 repetition_penalty 6 repeat_last_n 7 seed 8 enable_thinking
//  9 modalities (bit0 Text, bit1 Audio) 10 first_chunk_frames 11 chunk_frames 12 audio_temperature 13 audio_top_k
//  14 audio_repetition_penalty 15 audio_repetition_window 16 max_audio_seconds 17 video_fps 18 video_min_frames
//  19 video_max_groups 20 cache_prompt
// listener: onText(byte[] utf8), onAudio(float[] pcm24k, int rate) per streamed piece / chunk, on this thread.
// Returns "status|finish|ttft_ms|tok_per_s|decode_steps|first_audio_ms|audio_s|realtime_factor|prompt_rows|cached_rows|
//          completion_tokens|audio_frames|prefill_ms|encode_ms|error".
JNIEXPORT jstring JNICALL Java_ai_zetic_lfm3demo_Native_chat(JNIEnv* env, jclass, jdoubleArray cfgArr,
                                                              jobjectArray stopArr, jobject listener) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto fail = [&](const std::string& s) { return jstr(env, std::string(s)); };
  if (!g_llm) return fail("DeviceError|0|no model loaded");
  jclass lc = env->GetObjectClass(listener);
  jmethodID onText = env->GetMethodID(lc, "onText", "([B)V");
  jmethodID onAudio = env->GetMethodID(lc, "onAudio", "([FI)V");

  double c[24];
  env->GetDoubleArrayRegion(cfgArr, 0, 24, c);
  GenerationConfig g;
  g.max_tokens = static_cast<int32_t>(c[0]);
  g.temperature = static_cast<float>(c[1]);
  g.top_p = static_cast<float>(c[2]);
  g.min_p = static_cast<float>(c[3]);
  g.top_k = static_cast<int32_t>(c[4]);
  g.repetition_penalty = static_cast<float>(c[5]);
  g.repeat_last_n = static_cast<int32_t>(c[6]);
  g.seed = static_cast<uint64_t>(c[7]);
  g.enable_thinking = c[8] != 0;
  g.modalities.clear();
  if (static_cast<int>(c[9]) & 1) g.modalities.push_back(Modality::Text);
  if (static_cast<int>(c[9]) & 2) g.modalities.push_back(Modality::Audio);
  g.audio.first_chunk_frames = static_cast<int32_t>(c[10]);
  g.audio.chunk_frames = static_cast<int32_t>(c[11]);
  g.audio.temperature = static_cast<float>(c[12]);
  g.audio.top_k = static_cast<int32_t>(c[13]);
  g.audio.repetition_penalty = static_cast<float>(c[14]);
  g.audio.repetition_window = static_cast<int32_t>(c[15]);
  g.max_audio_seconds = static_cast<float>(c[16]);
  g.video.fps = c[17];
  g.video.min_frames = static_cast<int32_t>(c[18]);
  g.video.max_groups = static_cast<int32_t>(c[19]);
  g.cache_prompt = c[20] != 0;
  g.logprobs = c[21] != 0;
  g.top_logprobs = static_cast<int32_t>(c[22]);
  g.audio.collect = c[23] != 0;
  for (jsize i = 0, n = stopArr ? env->GetArrayLength(stopArr) : 0; i < n; ++i) {
    auto js = static_cast<jstring>(env->GetObjectArrayElement(stopArr, i));
    g.stop.push_back(str(env, js));
    env->DeleteLocalRef(js);
  }

  if (g_pending.empty()) return fail("InvalidArgument|0|empty turn");
  if ((g_history.empty() || g_history.front().role != Role::System) && (static_cast<int>(c[9]) & 2))
    g_history.insert(g_history.begin(), Message::System((static_cast<int>(c[9]) & 1) ? "Respond with interleaved text and audio." : "Perform TTS in American English using a female voice."));
  g_history.push_back(Message::User(std::move(g_pending)));
  g_pending.clear();

  Response r;
  try {
    r = g_llm->chat(g_history, g, [&](const StreamEvent& ev) {
      if (ev.type == StreamEvent::Type::Text) {
        jbyteArray b = env->NewByteArray(static_cast<jsize>(ev.text.size()));
        env->SetByteArrayRegion(b, 0, static_cast<jsize>(ev.text.size()), reinterpret_cast<const jbyte*>(ev.text.data()));
        env->CallVoidMethod(listener, onText, b);
        env->DeleteLocalRef(b);
      } else if (ev.type == StreamEvent::Type::Audio && ev.audio && ev.audio->samples) {
        jfloatArray f = env->NewFloatArray(static_cast<jsize>(ev.audio->samples));
        env->SetFloatArrayRegion(f, 0, static_cast<jsize>(ev.audio->samples), ev.audio->pcm);
        env->CallVoidMethod(listener, onAudio, f, ev.audio->sample_rate);
        env->DeleteLocalRef(f);
      }
      return true;
    });
  } catch (const Error& e) {
    g_history.pop_back();
    return fail(errText(e));
  }
  if (r.status != Status::Ok) {
    g_history.pop_back();  // a failed request is not part of the conversation
    return fail(std::string(statusName(r.status)) + "|0|" + r.error);
  }
  g_history.push_back(r.message);
  char buf[512];
  snprintf(buf, sizeof buf, "Ok|%s|%.1f|%.2f|%d|%.1f|%.2f|%.2f|%d|%d|%d|%d|%.1f|%.1f|", finishReasonName(r.finish_reason),
           r.timings.ttft, r.timings.tokens_per_second, r.timings.decode_steps, r.timings.first_audio,
           r.timings.audio_seconds, r.timings.realtime_factor, r.usage.prompt_rows, r.usage.cached_rows,
           r.usage.completion_tokens, r.usage.audio_frames, r.timings.prefill, r.timings.encode);
  std::ostringstream detail;
  detail << buf;
  for (size_t i = 0; i < r.tokens.size(); ++i) { if (i) detail << ','; detail << r.tokens[i]; }
  detail << '|';
  for (size_t i = 0; i < r.logprobs.size(); ++i) {
    if (i) detail << ';';
    const auto& lp = r.logprobs[i];
    detail << lp.token << ',' << lp.logprob << ',';
    for (unsigned char b : lp.piece) detail << std::hex << std::setw(2) << std::setfill('0') << int(b);
    detail << std::dec;
    for (const auto& t : lp.top) detail << ',' << t.first << ':' << t.second;
  }
  detail << '|' << r.timings.tokenize << ',' << r.timings.template_render << ',' << r.timings.preprocess
         << ',' << r.timings.assemble << ',' << r.timings.ttft_module << ',' << r.timings.decode
         << ',' << r.timings.callbacks << ',' << r.timings.dsp_prefill << ',' << r.timings.dsp_decode << ',' << r.timings.dsp_encode;
  return jstr(env, detail.str());
}

}  // extern "C"
