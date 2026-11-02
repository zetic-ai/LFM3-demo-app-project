#import <Foundation/Foundation.h>
#include <vector>
struct DemoAudioReadStats { size_t chunks=0,maxInputFrames=0; };
std::vector<float> DemoReadPCM16k(NSURL* url,DemoAudioReadStats* stats=nullptr);
