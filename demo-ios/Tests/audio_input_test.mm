#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include "../Sources/AudioInput.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
NSURL* make(NSURL* root,double rate,int channels,double seconds){
  NSURL* url=[root URLByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"wav"]];
  AVAudioFormat* format=[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32 sampleRate:rate channels:channels interleaved:NO];
  if(channels==4)format=[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32 sampleRate:rate interleaved:NO channelLayout:[[AVAudioChannelLayout alloc] initWithLayoutTag:kAudioChannelLayoutTag_Quadraphonic]];
  check(format!=nil,"synthetic audio format creation failed");
  const auto count=static_cast<AVAudioFrameCount>(rate*seconds);AVAudioPCMBuffer* buffer=[[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:count];buffer.frameLength=count;
  for(int c=0;c<channels;++c)for(size_t i=0;i<count;++i)buffer.floatChannelData[c][i]=.1f*std::sin(2.*3.141592653589793*440*i/rate);
  NSError* error=nil;AVAudioFile* file=[[AVAudioFile alloc] initForWriting:url settings:format.settings error:&error];check(file&&[file writeFromBuffer:buffer error:&error],"synthetic WAV creation failed");return url;
}
int main(){@autoreleasepool{NSURL* root=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];[NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];try{
  struct Case{double rate,seconds;int channels;};
  for(auto value:{Case{16000,30,1},Case{44100,1.25,2},Case{48000,.5,2}}){DemoAudioReadStats stats;auto pcm=DemoReadPCM16k(make(root,value.rate,value.channels,value.seconds),&stats);const auto expected=static_cast<size_t>(16000*value.seconds);check(pcm.size()+16>=expected&&pcm.size()<=expected+16,"resampling duration mismatch");check(pcm.size()<=480000,"resampled duration exceeds contract");check(stats.chunks>1&&stats.maxInputFrames<=4096,"unbounded or non-chunked audio read");}
  bool rejected=false;try{DemoReadPCM16k(make(root,16000,1,30.1));}catch(const std::exception&){rejected=true;}check(rejected,"oversized input not rejected");
  for(auto unsupported:{Case{192000,.1,1},Case{16000,.1,4}}){rejected=false;try{DemoReadPCM16k(make(root,unsupported.rate,unsupported.channels,unsupported.seconds));}catch(const std::exception&){rejected=true;}check(rejected,"unsupported input format not rejected");}
  [NSFileManager.defaultManager removeItemAtURL:root error:nil];std::cout<<"bounded multi-chunk mono/stereo resampling, 30s boundary, high-rate/multichannel/oversize rejection PASS\n";
}catch(const std::exception& e){[NSFileManager.defaultManager removeItemAtURL:root error:nil];std::cerr<<e.what()<<"\n";return 1;}}}
