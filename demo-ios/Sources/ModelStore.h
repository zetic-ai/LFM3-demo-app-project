#import <Foundation/Foundation.h>
// Publish one verified generation with a single atomic metadata write. The
// candidate stays at its immutable generation path; the previous active model
// and settings remain intact if serialization or the atomic write fails.
BOOL DemoCommitModelRelease(NSURL* supportRoot, NSURL* candidate,
                           NSDictionary* release, NSError** error);
NSURL* DemoActiveModelURL(NSURL* supportRoot, NSDictionary* release);
NSURL* DemoDownloadedModelURL(NSURL* documents,NSString* relativePath,NSString* packageID,
                             NSString* provenance,NSError** error);
