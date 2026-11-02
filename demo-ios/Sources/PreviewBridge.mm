#import "OmniBridge.h"
// Deliberately no SDK includes or link dependency. This target renders only UI.
@implementation OmniBridge
- (void)useModelInDocuments:(NSString*)kind directories:(NSArray<NSString*>*)directories expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {dispatch_async(dispatch_get_main_queue(),^{completion(@{@"ok":@NO,@"error":@"UI preview has no inference backend"});});}
- (void)restoreModelWithCompletion:(void (^)(NSDictionary*))completion {dispatch_async(dispatch_get_main_queue(),^{completion(@{@"ok":@NO,@"error":@"UI preview: model inference is disabled"});});}
- (void)useDownloadedModelInDocuments:(NSString*)path expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {dispatch_async(dispatch_get_main_queue(),^{completion(@{@"ok":@NO,@"error":@"UI preview does not load models"});});}
- (void)importModelAtURL:(NSURL*)url expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {dispatch_async(dispatch_get_main_queue(),^{completion(@{@"ok":@NO,@"error":@"UI preview does not import or run models"});});}
- (void)preparePrompt:(NSString*)prompt imageURL:(NSURL*)imageURL audioURL:(NSURL*)audioURL speech:(BOOL)speech interleaved:(BOOL)interleaved documentContext:(BOOL)document completion:(void (^)(NSDictionary*))completion {dispatch_async(dispatch_get_main_queue(),^{completion(@{@"ok":@NO,@"error":@"UI preview has no inference backend"});});}
- (void)runWithEvent:(void (^)(NSDictionary*))event {dispatch_async(dispatch_get_main_queue(),^{event(@{@"type":@"error",@"message":@"UI preview has no inference backend"});});}
- (void)cancel {}
@end
