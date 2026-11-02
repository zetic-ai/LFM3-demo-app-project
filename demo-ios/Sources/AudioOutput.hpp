#pragma once
#import <Foundation/Foundation.h>
#include <stdexcept>
#include <string>
// Caches may be purged after startup. Recreate/check the app-owned directory
// immediately before each WAV is opened, rather than relying on initialization.
inline NSURL* DemoNewGeneratedAudioURL(NSURL* directory){
  NSError* error=nil;
  if(!directory.isFileURL || ![NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error])
    throw std::runtime_error(std::string("Cannot create generated-audio directory: ")+(error.localizedDescription.UTF8String ?: "Invalid cache URL"));
  NSNumber* link=nil;NSNumber* folder=nil;
  if(![directory getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:&error] || link.boolValue ||
     ![directory getResourceValue:&folder forKey:NSURLIsDirectoryKey error:&error] || !folder.boolValue)
    throw std::runtime_error("Generated-audio cache is not a regular directory");
  return [directory URLByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"wav"]];
}
