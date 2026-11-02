#import "AudioInput.h"
#import <AVFoundation/AVFoundation.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace {
void check(bool condition,NSString* message){if(!condition)throw std::runtime_error(message.UTF8String ?: "Audio input error");}
}
std::vector<float> DemoReadPCM16k(NSURL* url,DemoAudioReadStats* stats){
  NSError* error=nil;AVAudioFile* file=[[AVAudioFile alloc] initForReading:url error:&error];
  check(file!=nil,error.localizedDescription ?: @"Cannot read audio");
  check(std::isfinite(file.processingFormat.sampleRate)&&file.processingFormat.sampleRate>=8000&&file.processingFormat.sampleRate<=96000,@"Supported audio sample rates are 8–96 kHz");
  check(file.processingFormat.channelCount>=1&&file.processingFormat.channelCount<=2,@"Supported audio inputs are mono or stereo");
  if(stats)*stats={};
  const double duration=static_cast<double>(file.length)/file.processingFormat.sampleRate;
  check(std::isfinite(duration)&&duration>0&&duration<=30.001,@"Choose an audio clip of 30 seconds or less. Audio is never silently truncated.");
  AVAudioFormat* format=[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32 sampleRate:16000 channels:1 interleaved:NO];
  AVAudioConverter* converter=[[AVAudioConverter alloc] initFromFormat:file.processingFormat toFormat:format];
  check(converter!=nil,@"This audio format cannot be converted to 16 kHz mono PCM");
  const auto capacity=static_cast<AVAudioFrameCount>(std::ceil(duration*16000)+256);
  AVAudioPCMBuffer* output=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:capacity];
  check(output!=nil,@"Cannot allocate audio output");
  __block NSError* readError=nil;
  __block AVAudioPCMBuffer* retainedInput=nil;
  AVAudioConverterOutputStatus status=[converter convertToBuffer:output error:&error withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount packets,AVAudioConverterInputStatus* state){
    const auto remaining=file.length-file.framePosition;
    if(remaining<=0){*state=AVAudioConverterInputStatus_EndOfStream;return nil;}
    const AVAudioFrameCount frames=static_cast<AVAudioFrameCount>(std::min<AVAudioFramePosition>(remaining,std::min<AVAudioPacketCount>(packets,4096)));
    retainedInput=[[AVAudioPCMBuffer alloc] initWithPCMFormat:file.processingFormat frameCapacity:frames];
    AVAudioPCMBuffer* input=retainedInput;
    if(!input || ![file readIntoBuffer:input frameCount:frames error:&readError]){*state=AVAudioConverterInputStatus_EndOfStream;return nil;}
    if(stats){++stats->chunks;stats->maxInputFrames=std::max<size_t>(stats->maxInputFrames,input.frameLength);}
    *state=AVAudioConverterInputStatus_HaveData;return input;
  }];
  check(readError==nil,readError.localizedDescription ?: @"Cannot read audio samples");
  check(status!=AVAudioConverterOutputStatus_Error&&output.frameLength>0,error.localizedDescription ?: @"Audio conversion failed");
  check(file.framePosition==file.length,@"Audio conversion did not consume the complete input");
  check(output.frameLength<=480000,@"Resampled audio exceeds 30 seconds");
  std::vector<float> pcm(output.floatChannelData[0],output.floatChannelData[0]+output.frameLength);
  for(float v:pcm)check(std::isfinite(v),@"Audio contains a nonfinite sample");
  return pcm;
}
