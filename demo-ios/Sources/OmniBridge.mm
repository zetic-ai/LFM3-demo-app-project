#import "OmniBridge.h"
#import "ModelStore.h"
#import "AudioInput.h"
#import <AVFoundation/AVFoundation.h>
#import <CommonCrypto/CommonDigest.h>
#include "DemoMetrics.hpp"
#include "OperationCompletion.hpp"
#include "AudioOutput.hpp"
#include "PromptContract.hpp"
#include "ModelPrompt.hpp"
#include "InputFileHash.hpp"
#include "lfm3_omni_session.h"
#include "lfm3_audio_encoder.h"
#include "lfm3_audio_session.h"
#include "lfm3_audio_bundle.h"
#include "lfm3_vision_session.h"
#include "text_chat.h"
#include "lfm3_image_preprocess.h"
#include "text_tokenizer.h"
#include "model_load_timing.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace {
constexpr size_t kPrefill=1536,kContext=2048,kImageRows=256;
constexpr int32_t kMixed=125015,kSpeech=125012;
NSString* ns(const std::string& value){return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding] ?: @"Invalid UTF-8";}
std::string str(NSString* value){return value.UTF8String ?: "";}
void require(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
void mainResult(void (^completion)(NSDictionary*),NSDictionary* value){dispatch_async(dispatch_get_main_queue(),^{completion(value);});}
void finishWork(std::atomic<bool>& busy,void (^completion)(NSDictionary*),NSDictionary* value){demo::releaseThenPublish(busy,[&]{mainResult(completion,value);});}
NSURL* support(){NSURL* url=[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;[NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil];return url;}
NSDictionary* failure(const std::string& error){return @{@"ok":@NO,@"error":ns(error)};}
bool digest(const std::string& value){return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos;}

std::vector<int32_t> promptIDs(lfm3_ios::TextTokenizer& tokenizer,const lfm3_ios::OmniRequest& request,bool speech,bool interleaved=true){
  size_t audioRows=0;if(!request.pcm_samples.empty()){const auto mel=lfm3_ios::AudioEncoder::melFrames(request.pcm_samples.size());require(mel>0,"Invalid audio length");audioRows=(static_cast<size_t>(mel)+7)/8;}
  return demo::omniPrompt(tokenizer,request.system_prompt,request.user_text,request.rgb.empty()?0:kImageRows,audioRows,speech?(interleaved?kMixed:kSpeech):0);
}
struct Prepared {lfm3_ios::OmniRequest request;std::vector<int32_t> ids;std::string imagePath,imageFileSHA;bool speech=false,interleaved=true;};
std::vector<int32_t> modelPrompt(lfm3_ios::TextTokenizer& tokenizer,const lfm3_ios::OmniRequest& request,const std::string& kind,bool media,bool speech,bool interleaved){
  const size_t audioRows=media&&!request.pcm_samples.empty()?(static_cast<size_t>(lfm3_ios::AudioEncoder::melFrames(request.pcm_samples.size()))+7)/8:0;
  return demo::renderModelPrompt(tokenizer,kind,request.system_prompt,request.user_text,media&&(kind=="vl"||!request.rgb.empty())?kImageRows:0,audioRows,speech?(interleaved?kMixed:kSpeech):0);
}
NSString* inputFingerprint(const Prepared& p){
  CC_SHA256_CTX context;CC_SHA256_Init(&context);
  const auto append=[&](const void* bytes,size_t count){const uint64_t length=count;CC_SHA256_Update(&context,&length,sizeof(length));if(count)CC_SHA256_Update(&context,bytes,static_cast<CC_LONG>(count));};
  append(p.ids.data(),p.ids.size()*sizeof(int32_t));append(p.request.rgb.data(),p.request.rgb.size());append(p.request.pcm_samples.data(),p.request.pcm_samples.size()*sizeof(float));
  if(!p.imageFileSHA.empty())append(p.imageFileSHA.data(),p.imageFileSHA.size());
  uint8_t hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(hash,&context);NSMutableString* result=[NSMutableString stringWithCapacity:64];for(auto byte:hash)[result appendFormat:@"%02x",byte];return result;
}
struct WavWriter {
  FILE* file=nullptr;uint64_t samples=0;std::string path;
  explicit WavWriter(const std::string& p):path(p){file=std::fopen(path.c_str(),"wb+");if(!file)throw std::runtime_error(std::string("Cannot create generated-audio file: ")+std::strerror(errno));char header[44]={};if(std::fwrite(header,1,44,file)!=44){std::fclose(file);file=nullptr;throw std::runtime_error("Cannot write WAV header");}}
  ~WavWriter(){if(file)std::fclose(file);}
  void append(const float* pcm,size_t count){
    require(count<=static_cast<size_t>(UINT32_MAX/2)-samples,"Generated audio is too large");
    std::vector<int16_t> data(count);for(size_t i=0;i<count;++i){require(std::isfinite(pcm[i]),"Nonfinite generated audio");data[i]=static_cast<int16_t>(std::lrint(std::clamp(pcm[i],-1.f,1.f)*32767.f));}
    require(std::fwrite(data.data(),sizeof(int16_t),count,file)==count,"Cannot write generated audio");samples+=count;
  }
  void finish(){
    const uint32_t bytes=static_cast<uint32_t>(samples*2);uint8_t h[44]={};
    std::memcpy(h,"RIFF",4);std::memcpy(h+8,"WAVEfmt ",8);std::memcpy(h+36,"data",4);
    const auto u32=[&](size_t p,uint32_t v){for(size_t j=0;j<4;++j)h[p+j]=static_cast<uint8_t>(v>>(j*8));};
    const auto u16=[&](size_t p,uint16_t v){h[p]=v&255;h[p+1]=v>>8;};
    u32(4,bytes+36);u32(16,16);u16(20,1);u16(22,1);u32(24,24000);u32(28,48000);u16(32,2);u16(34,16);u32(40,bytes);
    require(std::fseek(file,0,SEEK_SET)==0 && std::fwrite(h,1,44,file)==44 && std::fflush(file)==0,"Cannot finalize generated WAV");
  }
};
NSString* stopName(lfm3_ios::OmniStopReason reason){switch(reason){case lfm3_ios::OmniStopReason::EndOfSequence:return @"End of sequence";case lfm3_ios::OmniStopReason::Cancelled:return @"Cancelled";case lfm3_ios::OmniStopReason::GenerationBudget:return @"Output budget reached";case lfm3_ios::OmniStopReason::ContextCapacity:return @"Context capacity reached";default:return @"Error";}}
NSString* finishName(lfm3_ios::GenerationFinish reason){switch(reason){case lfm3_ios::GenerationFinish::EndOfSequence:return @"End of sequence";case lfm3_ios::GenerationFinish::CallbackCancelled:return @"Cancelled";case lfm3_ios::GenerationFinish::StopString:return @"Stop string";default:return @"Output budget reached";}}
NSDictionary* runSpecialized(const Prepared& p,const std::string& kind,const std::string& root,const std::string& expected,const std::string& provenance,size_t contextCapacity,
 void (^event)(NSDictionary*),std::atomic<bool>& cancelled){
  const auto now=[] {return lfm3_ios::monotonicNanoseconds();};const uint64_t openBegin=now();std::string error;
  std::unique_ptr<lfm3_ios::TextSession> text;std::unique_ptr<lfm3_ios::AudioTextSession> audio;std::unique_ptr<lfm3_ios::VisionTextSession> vision;
  if(kind=="text"){text=std::make_unique<lfm3_ios::TextSession>();require(text->open(root,error),error);require(text->packageId()==expected,"Text model identity changed");}
  else if(kind=="audio"){audio=std::make_unique<lfm3_ios::AudioTextSession>();require(audio->open(root,expected,error),error);}
  else {vision=std::make_unique<lfm3_ios::VisionTextSession>();lfm3_ios::VisionBundlePolicy policy;policy.allow_explicit_vision_assets=!provenance.empty();policy.expected_provenance_sha256=provenance;require(vision->open(root,expected,policy,error),error);}
  const uint64_t openEnd=now(),start=now();uint64_t first=0,last=0,visible=0;size_t tokens=0;std::vector<demo::Span> noLoads;
  const auto publish=[&]{NSMutableDictionary* value=[@{@"type":@"metrics",@"tokens":@(tokens),@"speech":@NO} mutableCopy];if(first)value[@"ttftMs"]=@((first-start)/1e6);if(visible)value[@"visibleMs"]=@((visible-start)/1e6);if(tokens>1)value[@"decodeTPS"]=@(demo::rate(tokens,first,last,noLoads));mainResult(event,value);};
  const auto onToken=[&](int32_t,size_t){const auto stamp=now();if(!first)first=stamp;last=stamp;++tokens;if(tokens==1||tokens%8==0)publish();};
  const auto onText=[&](std::string_view chunk){if(!visible)visible=now();mainResult(event,@{@"type":@"text",@"text":ns(std::string(chunk))});return !cancelled.load();};
  lfm3_ios::GenerationFinish finish=lfm3_ios::GenerationFinish::CallbackCancelled;size_t promptRows=p.ids.size();bool generated=false;
  const size_t budget=std::min<size_t>(256,contextCapacity-p.ids.size());require(budget>0,"No generation context remains");
  if(!cancelled.load()){
    generated=true;
    if(text){lfm3_ios::TextTokenizer tokenizer;require(tokenizer.load(root+"/tokenizer.json",error),error);int32_t eos;require(tokenizer.specialTokenId("<|im_end|>",eos,error),error);size_t generated=0;require(lfm3_ios::generateTextWithMedia(*text,tokenizer,p.ids,{},budget,{eos},{},onText,generated,error,onToken,{},&finish,false,false,[&]{return cancelled.load();}),error);}
    else if(audio){lfm3_ios::AudioTextRequest request;request.pcm=p.request.pcm_samples.data();request.samples=p.request.pcm_samples.size();request.system=p.request.system_prompt;request.user_text=p.request.user_text;request.max_tokens=budget;lfm3_ios::AudioTextResult result;require(audio->generate(request,onText,result,error,onToken),error);finish=result.finish;promptRows=result.prompt_rows;}
    else {lfm3_ios::VisionTextOptions options;options.max_tokens=budget;lfm3_ios::VisionTextResult result;require(vision->generateImage(p.imagePath,lfm3_ios::visionQuestionPrompt(p.request.user_text),options,onText,result,error,onToken),error);finish=result.finish;promptRows=result.prompt_rows;}
  }
  const uint64_t end=now();publish();require(promptRows==p.ids.size(),"Returned runtime prompt rows differ from prepared rows");
  if(generated)mainResult(event,@{@"type":@"promptValidated",@"promptRows":@(promptRows),@"rows":@(promptRows)});
  return @{@"type":@"complete",@"reason":finishName(finish),@"wallMs":@((end-openBegin)/1e6),@"responseWallMs":@((end-start)/1e6),@"openMs":@((openEnd-openBegin)/1e6),@"rows":@(promptRows),@"timingBoundary":@"TTFT starts after session.open and includes generate-time encoding, prefill and first selection. Session verification/loading is excluded as one open interval; it is not a measured model-load-only interval."};
}
}

@implementation OmniBridge {
  dispatch_queue_t _work;
  std::atomic<bool> _cancelled;
  std::atomic<bool> _busy;
  std::unique_ptr<Prepared> _prepared;
  bool _ready;
  std::string _packageID;
  lfm3_ios::OmniBundlePolicy _policy;
  NSURL* _modelURL;
  NSURL* _audioDirectory;
  std::string _modelKind;
  size_t _prefillCapacity,_contextCapacity;
}
- (instancetype)init {if((self=[super init])){_work=dispatch_queue_create("ai.zetic.omni.demo.inference",DISPATCH_QUEUE_SERIAL);_cancelled=false;_busy=false;_ready=false;_modelKind="omni";_prefillCapacity=kPrefill;_contextCapacity=kContext;
  NSURL* root=[[NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"OmniDemoAudio" isDirectory:YES];
  _audioDirectory=[root URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
  dispatch_async(_work,^{[NSFileManager.defaultManager createDirectoryAtURL:self->_audioDirectory withIntermediateDirectories:YES attributes:nil error:nil];for(NSURL* old in [NSFileManager.defaultManager contentsOfDirectoryAtURL:root includingPropertiesForKeys:nil options:0 error:nil]){if(![old.path isEqualToString:self->_audioDirectory.path] && [[NSUUID alloc] initWithUUIDString:old.lastPathComponent])[NSFileManager.defaultManager removeItemAtURL:old error:nil];}});
}return self;}
- (void)cancel {_cancelled=true;}
- (void)useModelInDocuments:(NSString*)kind directories:(NSArray<NSString*>*)directories expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {
  if(_busy.exchange(true)){mainResult(completion,failure("A request is already running"));return;}
  dispatch_async(_work,^{@autoreleasepool{try{
    const std::string type=str(kind),expected=str(packageID),prov=str(provenance);
    require(type=="text"||type=="audio"||type=="vl"||type=="omni","Unsupported model kind");require(digest(expected)&&(prov.empty()||digest(prov)),"Invalid trusted model fingerprints");
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL* candidate=nil;NSError* pathError=nil;
    for(NSString* relative in directories){NSURL* path=DemoDownloadedModelURL(documents,relative,packageID,provenance,&pathError);if(path){candidate=path;break;}}
    require(candidate!=nil,type+" model is missing. Place the verified bundle at Documents/"+str(directories.firstObject));
    size_t prefill=0,context=0;std::string error;
    lfm3_ios::OmniBundlePolicy policy;policy.allow_explicit_export=!prov.empty();policy.expected_provenance_sha256=prov;
    if(type=="text"){lfm3_ios::TextSession session;require(session.open(str(candidate.path),error),error);require(session.packageId()==expected,"Text package fingerprint differs from the trusted release");prefill=session.prefillCapacity();context=session.contextCapacity();}
    else if(type=="audio"){lfm3_ios::AudioBundleInfo info;require(lfm3_ios::verifyAudioBundle(str(candidate.path),expected,info,error),error);prefill=info.prefill_rows;context=info.context_rows;}
    else if(type=="vl"){lfm3_ios::VisionBundleInfo info;lfm3_ios::VisionBundlePolicy vp;vp.allow_explicit_vision_assets=!prov.empty();vp.expected_provenance_sha256=prov;require(lfm3_ios::verifyVisionBundle(str(candidate.path),expected,info,error,vp),error);prefill=info.prefill_rows;context=info.context_rows;}
    else {lfm3_ios::OmniBundleInfo info;require(lfm3_ios::verifyOmniBundle(str(candidate.path),expected,info,error,policy),error);prefill=info.prefill_rows;context=info.context_rows;}
    require(prefill&&context>prefill,"Invalid model context capacities");self->_modelURL=candidate;self->_packageID=expected;self->_policy=policy;self->_modelKind=type;self->_prefillCapacity=prefill;self->_contextCapacity=context;self->_ready=true;self->_prepared.reset();
    finishWork(self->_busy,completion,@{@"ok":@YES,@"packageID":packageID,@"prefillCapacity":@(prefill),@"contextCapacity":@(context),@"modelKind":kind});
  }catch(const std::exception& e){finishWork(self->_busy,completion,failure(e.what()));}}});
}
- (void)restoreModelWithCompletion:(void (^)(NSDictionary*))completion {
  if(_busy.exchange(true)){mainResult(completion,failure("A request is already running"));return;}
  dispatch_async(_work,^{@autoreleasepool{try{
    NSDictionary* release=[NSDictionary dictionaryWithContentsOfURL:[support() URLByAppendingPathComponent:@"ModelRelease.plist"]];
    self->_packageID=str(release[@"packageID"] ?: @"");self->_policy.expected_provenance_sha256=str(release[@"provenance"] ?: @"");self->_policy.allow_explicit_export=!self->_policy.expected_provenance_sha256.empty();
    self->_modelURL=DemoActiveModelURL(support(),release);require(self->_modelURL && digest(self->_packageID),"Model release is not configured");
    lfm3_ios::OmniBundleInfo info;std::string error;require(lfm3_ios::verifyOmniBundle(str(self->_modelURL.path),self->_packageID,info,error,self->_policy),error);self->_ready=true;finishWork(self->_busy,completion,@{@"ok":@YES,@"packageID":ns(self->_packageID)});
  }catch(const std::exception&){self->_ready=false;finishWork(self->_busy,completion,@{@"ok":@NO,@"error":@"Choose the optimized Omni model directory to begin."});}}});
}
- (void)useDownloadedModelInDocuments:(NSString*)relativePath expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {
  if(_busy.exchange(true)){mainResult(completion,failure("A request is already running"));return;}
  dispatch_async(_work,^{@autoreleasepool{try{
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;NSError* pathError=nil;
    NSURL* candidate=DemoDownloadedModelURL(documents,relativePath,packageID,provenance,&pathError);require(candidate!=nil,str(pathError.localizedDescription));
    std::string expected=str(packageID),prov=str(provenance);require(digest(expected)&&(prov.empty()||digest(prov)),"Configure trusted model release fingerprints");
    lfm3_ios::OmniBundlePolicy policy;policy.allow_explicit_export=!prov.empty();policy.expected_provenance_sha256=prov;
    lfm3_ios::OmniBundleInfo info;std::string error;require(lfm3_ios::verifyOmniBundle(str(candidate.path),expected,info,error,policy),error);
    self->_modelURL=candidate;self->_packageID=std::move(expected);self->_policy=std::move(policy);self->_ready=true;self->_prepared.reset();
    finishWork(self->_busy,completion,@{@"ok":@YES,@"packageID":ns(self->_packageID)});
  }catch(const std::exception& e){finishWork(self->_busy,completion,failure(e.what()));}}});
}
- (void)importModelAtURL:(NSURL*)url expectedPackageID:(NSString*)packageID provenanceSHA256:(NSString*)provenance completion:(void (^)(NSDictionary*))completion {
  if(_busy.exchange(true)){mainResult(completion,failure("A request is already running"));return;}
  dispatch_async(_work,^{@autoreleasepool{
    const BOOL scoped=[url startAccessingSecurityScopedResource];NSURL* stage=[support() URLByAppendingPathComponent:[@"OmniModel-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
    bool committed=false;NSDictionary* terminal=nil;
    try{
      std::string expected=str(packageID);const std::string provenanceHash=str(provenance);require(digest(expected),"Set the expected package fingerprint from the trusted model release before importing");require(provenanceHash.empty()||digest(provenanceHash),"Invalid export provenance fingerprint");lfm3_ios::OmniBundlePolicy policy;policy.allow_explicit_export=!provenanceHash.empty();policy.expected_provenance_sha256=provenanceHash;
      NSError* copyError=nil;require([NSFileManager.defaultManager copyItemAtURL:url toURL:stage error:&copyError],str(copyError.localizedDescription ?: @"Cannot import model directory"));
      lfm3_ios::OmniBundleInfo info;std::string error;require(lfm3_ios::verifyOmniBundle(str(stage.path),expected,info,error,policy),error);
      NSURL* previous=self->_modelURL;
      require(DemoCommitModelRelease(support(),stage,@{@"packageID":packageID,@"provenance":provenance},&copyError),str(copyError.localizedDescription ?: @"Cannot save model release settings"));
      committed=true;self->_modelURL=stage;self->_ready=true;self->_packageID.swap(expected);self->_policy=std::move(policy);self->_prepared.reset();
      const BOOL managedPrevious=previous && [previous.URLByDeletingLastPathComponent.path isEqualToString:support().path] && [previous.lastPathComponent hasPrefix:@"OmniModel-"];
      if(managedPrevious && ![previous.path isEqualToString:stage.path])[NSFileManager.defaultManager removeItemAtURL:previous error:nil];
      NSNumber* excluded=@YES;[stage setResourceValue:excluded forKey:NSURLIsExcludedFromBackupKey error:nil];
      terminal=@{@"ok":@YES,@"packageID":ns(self->_packageID)};
    }catch(const std::exception& e){if(!committed)[NSFileManager.defaultManager removeItemAtURL:stage error:nil];terminal=failure(e.what());}
    if(scoped)[url stopAccessingSecurityScopedResource];finishWork(self->_busy,completion,terminal);
  }});
}
- (void)preparePrompt:(NSString*)prompt imageURL:(NSURL*)imageURL audioURL:(NSURL*)audioURL speech:(BOOL)speech interleaved:(BOOL)interleaved documentContext:(BOOL)document completion:(void (^)(NSDictionary*))completion {
  if(_busy.exchange(true)){mainResult(completion,failure("A request is already running"));return;}
  dispatch_async(_work,^{@autoreleasepool{try{
    require(self->_ready,"Import the optimized Omni model directory first");
    auto prepared=std::make_unique<Prepared>();prepared->speech=speech;prepared->interleaved=interleaved;prepared->request.user_text=str(prompt);
    if(self->_modelKind=="audio")prepared->request.system_prompt=demo::systemForModel(self->_modelKind);
    if(imageURL)prepared->imagePath=str(imageURL.path);
    if(self->_modelKind=="vl"){require(imageURL!=nil,"The VL example requires an image");prepared->imageFileSHA=DemoInputFileSHA256(prepared->imagePath,128*1024*1024);}
    else if(imageURL){zetic_mlange::lm::diagnostic::Lfm3ImagePixels pixels;std::string error;require(zetic_mlange::lm::diagnostic::decodeImageLfm3(str(imageURL.path),{},&pixels,&error),error);prepared->request.rgb=std::move(pixels.rgb);}
    if(audioURL)prepared->request.pcm_samples=DemoReadPCM16k(audioURL);
    lfm3_ios::TextTokenizer tokenizer;std::string error;require(tokenizer.load(str([self->_modelURL URLByAppendingPathComponent:@"tokenizer.json"].path),error),error);
    prepared->ids=modelPrompt(tokenizer,prepared->request,self->_modelKind,true,speech,interleaved);require(prepared->ids.size()<=self->_prefillCapacity,"The task and media exceed this model's full prefill capacity.");
    const int32_t marker=speech?(interleaved?kMixed:kSpeech):0;
    if(document){
      NSURL* contextURL=[NSBundle.mainBundle URLForResource:@"DarwinIntroduction" withExtension:@"txt" subdirectory:@"Samples"];
      NSString* content=contextURL?[NSString stringWithContentsOfURL:contextURL encoding:NSUTF8StringEncoding error:nil]:nil;
      require(content!=nil,"Reference document resource is missing");const std::string full=str(content);
      const size_t ar=prepared->request.pcm_samples.empty()?0:(static_cast<size_t>(lfm3_ios::AudioEncoder::melFrames(prepared->request.pcm_samples.size()))+7)/8;
      const auto render=[&](const std::string& text,bool media){auto request=prepared->request;request.user_text=text;return modelPrompt(tokenizer,request,self->_modelKind,media,speech,interleaved);};
      auto fitted=demo::fitDocumentPrompt(prepared->request.user_text,full,[&](const std::string& text){return render(text,true);},[&](const std::string& text){return render(text,false).size();},self->_prefillCapacity,imageURL!=nil);
      prepared->request.user_text=std::move(fitted.text);prepared->ids=std::move(fitted.ids);
    }
    require(!prepared->request.user_text.empty()||imageURL||audioURL,"Provide a task or a media input");
    const size_t rows=prepared->ids.size(),textRows=modelPrompt(tokenizer,prepared->request,self->_modelKind,false,speech,interleaved).size();require(textRows<=1024,"Text plus chat template exceeds the iOS 1,024-token limit");const double seconds=prepared->request.pcm_samples.size()/16000.;
    const std::string visible=prepared->request.system_prompt.empty()?prepared->request.user_text:"SYSTEM:\n"+prepared->request.system_prompt+"\n\nUSER:\n"+prepared->request.user_text;
    NSDictionary* value=@{@"ok":@YES,@"inputSHA256":inputFingerprint(*prepared),@"rows":@(demo::actualPrefillRows(rows,speech)),@"promptRows":@(rows),@"textRows":@(textRows),@"capacity":@(self->_prefillCapacity),@"textBudget":@(std::min<size_t>(1024,self->_prefillCapacity-(rows-textRows))),@"remaining":@(self->_contextCapacity-rows),@"effectivePrompt":ns(visible),@"audioSeconds":@(seconds),@"imageRows":@(imageURL?kImageRows:0),@"audioRows":@(audioURL?(static_cast<size_t>(lfm3_ios::AudioEncoder::melFrames(prepared->request.pcm_samples.size()))+7)/8:0)};
    self->_prepared=std::move(prepared);finishWork(self->_busy,completion,value);
  }catch(const std::exception& e){self->_prepared.reset();finishWork(self->_busy,completion,failure(e.what()));}}});
}
- (void)runWithEvent:(void (^)(NSDictionary*))event {
  if(_busy.exchange(true)){mainResult(event,@{@"type":@"error",@"message":@"A request is already running"});return;}
  _cancelled=false;
  dispatch_async(_work,^{@autoreleasepool{NSURL* generatedURL=nil;try{
    require(self->_ready && self->_prepared!=nullptr,"Prepare the input before running");auto& p=*self->_prepared;
    if(self->_modelKind!="omni"){
      mainResult(event,@{@"type":@"status",@"message":@"Opening selected model…"});
      NSDictionary* done=runSpecialized(p,self->_modelKind,str(self->_modelURL.path),self->_packageID,self->_policy.expected_provenance_sha256,self->_contextCapacity,event,self->_cancelled);
      finishWork(self->_busy,event,done);return;
    }
    mainResult(event,@{@"type":@"status",@"message":@"Verifying model…"});
    const uint64_t openBegin=lfm3_ios::monotonicNanoseconds();
    lfm3_ios::OmniSession session;std::string error;require(session.open(str(self->_modelURL.path),self->_packageID,error,self->_policy),error);
    const uint64_t openEnd=lfm3_ios::monotonicNanoseconds();
    mainResult(event,@{@"type":@"status",@"message":@"Running on device…"});
    std::vector<demo::Span> loads,setups;uint64_t first=0,last=0,visible=0,pcmFirst=0,pcmLast=0,frameFirst=0,frameLast=0;size_t tokens=0,frames=0,pcmSamples=0;
    NSURL* audioURL=p.speech?DemoNewGeneratedAudioURL(self->_audioDirectory):nil;if(p.speech)generatedURL=audioURL;
    std::unique_ptr<WavWriter> wav;if(p.speech)wav=std::make_unique<WavWriter>(str(audioURL.path));
    lfm3_ios::OmniOptions options;options.max_generation_steps=std::min<size_t>(p.speech?512:256,self->_contextCapacity-p.ids.size());options.mode=p.interleaved?lfm3_ios::OmniSpeechMode::Interleaved:lfm3_ios::OmniSpeechMode::Audio;options.collect_audio=false;options.seed=42;options.codec_chunk_frames=1;
    options.cancelled=[self]{return self->_cancelled.load();};
    options.on_model_load=[&](const char*,uint64_t a,uint64_t b){require(b>=a,"Invalid model load interval");loads.push_back({a,b});};
    options.on_setup=[&](const char*,uint64_t a,uint64_t b){require(b>=a,"Invalid setup interval");setups.push_back({a,b});};
    options.on_prompt=[&](const std::vector<int32_t>& ids){require(ids==p.ids,"Runtime prompt differs from prepared input. No backbone inference was run.");mainResult(event,@{@"type":@"promptValidated",@"promptRows":@(ids.size()),@"rows":@(demo::actualPrefillRows(ids.size(),p.speech))});};
    const uint64_t start=lfm3_ios::monotonicNanoseconds();
    const auto publish=[&]{NSMutableDictionary* metrics=[@{@"type":@"metrics",@"rows":@(p.ids.size()),@"tokens":@(tokens),@"audioFrames":@(frames),@"audioSeconds":@(pcmSamples/24000.),@"speech":@(p.speech)} mutableCopy];if(first)metrics[@"ttftMs"]=@(demo::adjustedMs(start,first,loads));if(visible)metrics[@"visibleMs"]=@(demo::adjustedMs(start,visible,loads));if(tokens>1)metrics[@"decodeTPS"]=@(demo::rate(tokens,first,last,loads));if(pcmFirst)metrics[@"firstPCMMs"]=@(demo::adjustedMs(start,pcmFirst,loads));if(frames>1)metrics[@"frameRate"]=@(demo::rate(frames,frameFirst,frameLast,loads));mainResult(event,metrics);};
    const auto onText=[&](std::string_view chunk){if(!visible)visible=lfm3_ios::monotonicNanoseconds();mainResult(event,@{@"type":@"text",@"text":ns(std::string(chunk))});return !self->_cancelled.load();};
    options.on_speech_frame=[&](const std::array<int32_t,12>&,size_t,bool eos){if(eos)return;uint64_t now=lfm3_ios::monotonicNanoseconds();if(!frameFirst)frameFirst=now;frameLast=now;++frames;};
    lfm3_ios::OmniResult result;bool ok=false;
    if(p.speech){ok=session.respondSpeech(p.request,options,onText,[&](const float* pcm,size_t count,int32_t rate){require(rate==24000,"Unexpected PCM rate");if(count){uint64_t now=lfm3_ios::monotonicNanoseconds();if(!pcmFirst)pcmFirst=now;pcmLast=now;pcmSamples+=count;wav->append(pcm,count);double energy=0;for(size_t i=0;i<count;++i)energy+=pcm[i]*pcm[i];mainResult(event,@{@"type":@"audioLevel",@"level":@(std::sqrt(energy/std::max<size_t>(count,1)))});publish();}return !self->_cancelled.load();},result);}
    else {ok=session.respondText(p.request,options,onText,result,[&](int32_t,size_t){uint64_t now=lfm3_ios::monotonicNanoseconds();if(!first)first=now;last=now;++tokens;if(tokens==1||tokens%8==0)publish();});}
    const uint64_t end=lfm3_ios::monotonicNanoseconds();publish();
    require(ok,result.error);if(wav){wav->finish();wav.reset();}const auto times=demo::boundary(openBegin,openEnd,start,end);require(times.valid,"Invalid request timing boundary");
    const bool cancelled=result.stop_reason==lfm3_ios::OmniStopReason::Cancelled;
    if(cancelled&&generatedURL){[NSFileManager.defaultManager removeItemAtURL:generatedURL error:nil];generatedURL=nil;}
    NSMutableDictionary* done=[@{@"type":@"complete",@"reason":stopName(result.stop_reason),@"wallMs":@(times.totalWallMs),@"responseWallMs":@(times.responseWallMs),@"verificationMs":@(times.verificationMs),@"modelLoadMs":@(demo::covered(loads,start,end)/1e6),@"setupMs":@(demo::covered(setups,start,end)/1e6),@"rows":@(result.prompt_rows),@"audioSeconds":@(pcmSamples/24000.)} mutableCopy];
    if(pcmSamples&&!cancelled){done[@"audioURL"]=audioURL;const double elapsed=demo::adjustedMs(start,pcmLast,loads)/1000.;if(elapsed>0)done[@"audioRealtime"]=@((pcmSamples/24000.)/elapsed);}
    finishWork(self->_busy,event,done);
  }catch(const std::exception& e){if(generatedURL)[NSFileManager.defaultManager removeItemAtURL:generatedURL error:nil];finishWork(self->_busy,event,@{@"type":@"error",@"message":ns(e.what())});}}});
}
@end
