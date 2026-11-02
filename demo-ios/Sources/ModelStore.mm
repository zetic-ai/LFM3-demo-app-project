#import "ModelStore.h"
NSURL* DemoDownloadedModelURL(NSURL* documents,NSString* relativePath,NSString* packageID,NSString* provenance,NSError** error){
  NSCharacterSet* nonHex=[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
  const BOOL fingerprint=packageID.length==64&&[packageID rangeOfCharacterFromSet:nonHex].location==NSNotFound;
  const BOOL provenanceOK=provenance.length==0||(provenance.length==64&&[provenance rangeOfCharacterFromSet:nonHex].location==NSNotFound);
  NSURL* root=documents.URLByResolvingSymlinksInPath;
  NSURL* candidate=[[root URLByAppendingPathComponent:relativePath isDirectory:YES] URLByResolvingSymlinksInPath];
  BOOL directory=NO;
  if(!fingerprint||!provenanceOK||relativePath.length==0||relativePath.isAbsolutePath||[relativePath.pathComponents containsObject:@".."]||![candidate.path hasPrefix:[root.path stringByAppendingString:@"/"]]||![NSFileManager.defaultManager fileExistsAtPath:candidate.path isDirectory:&directory]||!directory){
    if(error)*error=[NSError errorWithDomain:@"ai.zetic.omni.model" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Prepare the downloaded model under Documents with trusted release fingerprints"}];return nil;
  }
  return candidate;
}
NSURL* DemoActiveModelURL(NSURL* root,NSDictionary* release){
  NSString* name=release[@"directory"];
  if(![name isKindOfClass:NSString.class] || ![name hasPrefix:@"OmniModel-"] || name.length<=10 || [name containsString:@"/"] || [name containsString:@"\\"] || [name containsString:@".."])return nil;
  return [root URLByAppendingPathComponent:name isDirectory:YES];
}
BOOL DemoCommitModelRelease(NSURL* root,NSURL* candidate,NSDictionary* release,NSError** error){
  NSMutableDictionary* value=[release mutableCopy];value[@"directory"]=candidate.lastPathComponent;
  NSURL* check=DemoActiveModelURL(root,value);
  if(!check || ![check.path isEqualToString:candidate.path]){
    if(error)*error=[NSError errorWithDomain:@"ai.zetic.omni.model" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Invalid model generation path"}];return NO;
  }
  NSData* data=[NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
  if(!data)return NO;
  return [data writeToURL:[root URLByAppendingPathComponent:@"ModelRelease.plist"] options:NSDataWritingAtomic error:error];
}
