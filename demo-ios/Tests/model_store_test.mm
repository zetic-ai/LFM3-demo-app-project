#import <Foundation/Foundation.h>
#include "../Sources/ModelStore.h"
#include <sys/stat.h>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){@autoreleasepool{
  NSURL* root=[[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
  NSFileManager* fm=NSFileManager.defaultManager;[fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
  try{
    NSURL* old=[root URLByAppendingPathComponent:@"OmniModel-old" isDirectory:YES];NSURL* candidate=[root URLByAppendingPathComponent:@"OmniModel-new" isDirectory:YES];
    [fm createDirectoryAtURL:old withIntermediateDirectories:YES attributes:nil error:nil];[fm createDirectoryAtURL:candidate withIntermediateDirectories:YES attributes:nil error:nil];
    [@"old verified payload" writeToURL:[old URLByAppendingPathComponent:@"payload"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSError* error=nil;check(DemoCommitModelRelease(root,old,@{@"packageID":@"old"},&error),"initial publish failed");
    NSURL* active=[root URLByAppendingPathComponent:@"ModelRelease.plist"];NSData* before=[NSData dataWithContentsOfURL:active];
    check(!DemoCommitModelRelease(root,candidate,@{@"invalid":[NSObject new]},&error),"invalid metadata must fail");
    check([before isEqualToData:[NSData dataWithContentsOfURL:active]],"serialization failure changed active release");
    chmod(root.fileSystemRepresentation,0500);error=nil;BOOL ok=DemoCommitModelRelease(root,candidate,@{@"packageID":@"new"},&error);chmod(root.fileSystemRepresentation,0700);
    check(!ok,"read-only directory must fail publish");check([before isEqualToData:[NSData dataWithContentsOfURL:active]],"write failure changed active release");
    check([fm fileExistsAtPath:[old URLByAppendingPathComponent:@"payload"].path],"failure removed old model");
    check(DemoActiveModelURL(root,@{@"directory":@"../outside"})==nil,"unsafe active model directory accepted");
    error=nil;check(DemoCommitModelRelease(root,candidate,@{@"packageID":@"new"},&error),"valid replacement failed");
    NSDictionary* after=[NSDictionary dictionaryWithContentsOfURL:active];check([after[@"directory"] isEqualToString:@"OmniModel-new"],"wrong active generation");
    check([fm fileExistsAtPath:old.path] && [fm fileExistsAtPath:candidate.path],"commit mutated model payload directories");
    NSURL* documents=[root URLByAppendingPathComponent:@"Documents" isDirectory:YES];NSURL* downloaded=[documents URLByAppendingPathComponent:@"OmniModel" isDirectory:YES];
    [fm createDirectoryAtURL:downloaded withIntermediateDirectories:YES attributes:nil error:nil];NSString* trusted=[@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0];
    check(![fm fileExistsAtPath:[documents URLByAppendingPathComponent:@"ModelRelease.plist"].path],"unexpected active metadata in Documents");
    check(DemoDownloadedModelURL(documents,@"OmniModel",trusted,@"",&error)!=nil,"downloaded fallback requires an active plist");
    check(DemoDownloadedModelURL(documents,@"OmniModel",@"invalid",@"",&error)==nil,"invalid package fingerprint accepted");
    check(DemoDownloadedModelURL(documents,@"OmniModel",trusted,@"bad",&error)==nil,"invalid explicit provenance accepted");
    check(DemoDownloadedModelURL(documents,@"../OmniModel-old",trusted,@"",&error)==nil,"downloaded parent traversal accepted");
    [fm createSymbolicLinkAtURL:[documents URLByAppendingPathComponent:@"escape"] withDestinationURL:old error:nil];
    check(DemoDownloadedModelURL(documents,@"escape",trusted,@"",&error)==nil,"downloaded symlink escape accepted");
    [fm removeItemAtURL:root error:nil];std::cout<<"atomic model metadata publication, failure preservation and generation paths PASS\n";
  }catch(const std::exception& e){chmod(root.fileSystemRepresentation,0700);[fm removeItemAtURL:root error:nil];std::cerr<<e.what()<<"\n";return 1;}
}}
