#import <Foundation/Foundation.h>
#include "../Sources/AudioOutput.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
int main(){@autoreleasepool{
  NSURL* root=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
  NSURL* cache=[root URLByAppendingPathComponent:@"Audio/session"];
  for(int n=0;n<2;++n){NSURL* wav=DemoNewGeneratedAudioURL(cache);FILE* file=fopen(wav.fileSystemRepresentation,"wb");assert(file);assert(fwrite("PCM",1,3,file)==3);fclose(file);assert([NSFileManager.defaultManager removeItemAtURL:cache error:nil]);}
  [NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
  NSURL* blocked=[root URLByAppendingPathComponent:@"blocked"];assert([[@"x" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:blocked atomically:YES]);
  bool rejected=false;try{DemoNewGeneratedAudioURL([blocked URLByAppendingPathComponent:@"child"]);}catch(const std::exception&){rejected=true;}assert(rejected);
  NSURL* symbolic=[root URLByAppendingPathComponent:@"link"];assert([NSFileManager.defaultManager createSymbolicLinkAtURL:symbolic withDestinationURL:root error:nil]);
  rejected=false;try{DemoNewGeneratedAudioURL(symbolic);}catch(const std::exception&){rejected=true;}assert(rejected);
  [NSFileManager.defaultManager removeItemAtURL:root error:nil];
  std::cout<<"audio cache create/write, purge/recreate, invalid parent and symlink rejection PASS\n";
}}
