#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
// All callbacks are delivered on the main queue. The engine serializes work.
@interface OmniBridge : NSObject
- (void)useModelInDocuments:(NSString *)kind directories:(NSArray<NSString *> *)directories
          expectedPackageID:(NSString *)packageID provenanceSHA256:(NSString *)provenance
                 completion:(void (^)(NSDictionary *result))completion;
- (void)importModelAtURL:(NSURL *)url expectedPackageID:(NSString *)packageID
       provenanceSHA256:(NSString *)provenance completion:(void (^)(NSDictionary *result))completion;
- (void)restoreModelWithCompletion:(void (^)(NSDictionary *result))completion;
- (void)useDownloadedModelInDocuments:(NSString *)relativePath expectedPackageID:(NSString *)packageID
                     provenanceSHA256:(NSString *)provenance completion:(void (^)(NSDictionary *result))completion;
- (void)preparePrompt:(NSString *)prompt imageURL:(nullable NSURL *)imageURL
             audioURL:(nullable NSURL *)audioURL speech:(BOOL)speech interleaved:(BOOL)interleaved
       documentContext:(BOOL)document
           completion:(void (^)(NSDictionary *result))completion;
- (void)runWithEvent:(void (^)(NSDictionary *event))event;
- (void)cancel;
@end
NS_ASSUME_NONNULL_END
