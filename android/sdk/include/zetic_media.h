// ZeticMLange Hexagon SDK — media inputs and chat messages (OpenAI chat-completions shape, llama.cpp mtmd bitmaps).
// Design: sdk/DESIGN.md §2.2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zetic_types.h"

// Everything declared below is the public API of libzetic_mlange_hexagon.so (the library is built with -fvisibility=hidden).
#pragma GCC visibility push(default)
namespace zetic::mlange {
inline namespace hx1 {

// An image as RGB8, row-major, no padding (llama.cpp mtmd_bitmap_init).
// SIZE: exactly 512 x 512 (the vision encoder's grid). The SDK does no resolution handling: no resize, letterbox or
// tiling. A request with an image of any other size returns Status::InvalidArgument ("images must be exactly 512x512
// RGB"); resize / letterbox / crop on the caller side (that choice affects accuracy and speed, so it stays yours).
// fromFile decodes PNG / JPEG / PPM / BMP (stb_image) to get the pixels only; the size is checked when it is used.
struct Image {
  int32_t width = 0, height = 0;
  std::vector<uint8_t> rgb;          // width * height * 3
  std::string id;                    // cache key (mtmd_bitmap_set_id); "" = 64-bit content hash, computed once
  // Copies width * height * 3 bytes.
  static Image fromRgb(const uint8_t* rgb, int32_t width, int32_t height);
  // PNG / JPEG / PPM (stb_image, public domain). Throws Error(InvalidArgument) for an unreadable file.
  static Image fromFile(const std::string& path);
};

// Mono float PCM. fromPcm mixes channels and resamples to the model's input rate (16 kHz for LFM3 / d1-Omni).
struct Audio {
  std::vector<float> pcm;
  int32_t sample_rate = 16000;
  std::string id;                    // cache key; "" = content hash
  // samples: frames * channels interleaved floats in [-1, 1].
  static Audio fromPcm(const float* samples, size_t frames, int32_t sample_rate, int32_t channels = 1);
  // PCM16 / float32 WAV. Throws Error(InvalidArgument) for an unreadable file.
  static Audio fromWav(const std::string& path);
};

// A clip as decoded frames (HF {"type": "video", "video": [frames], "fps": 24}; ixian lm::VideoFrames).
// The SDK samples them at GenerationConfig::video.fps (2 fps), pairs frames into temporal patches (2 frames -> 1 patch
// -> 256 rows) and adds HF's "<t seconds>" markers (DESIGN.md §2.7). No container decoding in the SDK (no ffmpeg).
// Every frame must be exactly 512 x 512 RGB, like Image (no resize / letterbox in the SDK; other sizes ->
// Status::InvalidArgument). Sampling: with fps (or one timestamp per frame) the SDK samples at
// GenerationConfig::video.fps exactly as HF Lfm3VlVideoProcessor; with neither, the frames are taken as given, paired in
// order into temporal patches, and timed as if they were at GenerationConfig::video.fps. An odd count repeats the last
// frame. One temporal patch = 256 rows + ~8 marker tokens of the 2,048-position context.
struct Video {
  std::vector<Image> frames;
  double fps = 0;                    // the frames' rate; or set timestamps
  std::vector<double> timestamps;    // seconds, one per frame (optional, overrides fps)
  std::string id;
  static Video fromFrames(std::vector<Image> frames, double fps);
};

// One content part (OpenAI content part). Media are shared_ptr so a history can be re-sent every turn for free.
struct Part {
  enum class Type : uint8_t { Text, Image, Audio, Video };
  Type type = Type::Text;
  std::string text;
  std::shared_ptr<const Image> image;
  std::shared_ptr<const Audio> audio;
  std::shared_ptr<const Video> video;
  static Part Text(std::string t);
  static Part Img(Image im);
  static Part Img(std::shared_ptr<const Image> im);
  static Part Aud(Audio a);
  static Part Aud(std::shared_ptr<const Audio> a);
  static Part Vid(Video v);
  static Part Vid(std::shared_ptr<const Video> v);
};

enum class Role : uint8_t { System, User, Assistant };

namespace detail {
struct __attribute__((visibility("hidden"))) Generated;  // opaque: the exact ids / speech frames a reply put into the cache
}

struct Message {
  Role role = Role::User;
  std::vector<Part> content;         // order = prompt order (media placed where they appear, like mtmd markers)
  // Set on the assistant message the SDK returns (Response::message). When it is present and the text is unchanged,
  // the next request renders this turn from the cached ids instead of re-tokenizing it, so the prefix cache matches
  // (DESIGN.md §2.6). Hand-written assistant messages work too; they are tokenized normally.
  std::shared_ptr<const detail::Generated> generated;

  static Message System(std::string text);
  static Message User(std::string text);
  static Message User(std::vector<Part> parts);
  static Message Assistant(std::string text);

  // The concatenated text parts (media skipped).
  std::string text() const;
};

}  // namespace hx1
}  // namespace zetic::mlange
#pragma GCC visibility pop
