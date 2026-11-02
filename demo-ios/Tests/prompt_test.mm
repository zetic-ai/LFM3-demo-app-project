#import <Foundation/Foundation.h>
#include "../Sources/PromptContract.hpp"
#include <iostream>
void check(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
std::vector<int32_t> values(NSArray* array){std::vector<int32_t> result;for(NSNumber* n in array)result.push_back(n.intValue);return result;}
int main(int argc,char** argv){@autoreleasepool{try{
  check(argc==6,"usage: prompt_test TOKENIZER.json OMNI_FIXTURE.json JIRA_FIXTURE.json DOCUMENT.txt PRESETS.json");
  lfm3_ios::TextTokenizer tokenizer;std::string error;check(tokenizer.load(argv[1],error),error.c_str());
  const auto read=[](const char* path){NSData* data=[NSData dataWithContentsOfFile:@(path)];return data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;};
  NSDictionary* original=read(argv[2]);NSDictionary* jira=read(argv[3]);check(original&&jira,"missing fixtures");
  const auto test=[&](NSDictionary* f,size_t image,size_t audio,int32_t marker){std::string system=[f[@"system"] UTF8String] ?: "";std::string text=[f[@"question"] UTF8String] ?: ([f[@"text"] UTF8String] ?: "");auto actual=demo::omniPrompt(tokenizer,system,text,image,audio,marker);check(actual==values(f[@"prompt_ids"]),"prompt fixture mismatch");return actual.size();};
  check(test(original[@"image"],256,0,0)==276,"image row count");
  const size_t shortRows=values(original[@"audio"][@"prompt_ids"]).size();
  size_t placeholders=0;for(auto id:values(original[@"audio"][@"prompt_ids"]))if(id==125031)++placeholders;
  check(test(original[@"audio"],0,placeholders,0)==shortRows,"short audio contract");
  check(test(jira[@"audio"],0,375,0)==1399,"30s plus 1024 contract");
  auto speech=values(original[@"speech"][@"prompt_ids"]);check(!speech.empty(),"speech fixture missing");
  check(test(original[@"speech"],0,0,speech.back())==speech.size(),"speech marker contract");
  check(demo::actualPrefillRows(1536,false)==1536 && demo::actualPrefillRows(1536,true)==1535,"speech marker is not a prefill row");
  NSString* context=[NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];check(context!=nil,"missing context");
  for(size_t image:{0u,256u})for(size_t audio:{0u,375u})for(int32_t marker:{0,125012,125015}){
    const std::string task="Summarize the reference extract in two short sentences.";
    auto fitted=demo::buildDocumentPrompt(tokenizer,task,context.UTF8String,image,audio,marker);
    check(fitted.ids.size()<=1536 && fitted.textRows<=1024,"text/full prompt capacity failure");
    check(fitted.system.empty()&&fitted.text.rfind(task+"\n\nReference extract:\n",0)==0,"document is not part of actual user task");
    check(fitted.ids==demo::omniPrompt(tokenizer,"",fitted.text,image,audio,marker),"prepared IDs mismatch");
  }
  NSArray* presets=read(argv[5]);check([presets isKindOfClass:NSArray.class]&&presets.count==6,"six actual preset metadata entries required");
  for(NSDictionary* preset in presets){
    const std::string task=[preset[@"task"] UTF8String];
    const size_t image=[preset[@"image_rows"] unsignedLongLongValue],audio=[preset[@"audio_rows"] unsignedLongLongValue];
    const int32_t marker=[preset[@"marker"] intValue];
    auto fitted=demo::buildDocumentPrompt(tokenizer,task,context.UTF8String,image,audio,marker);
    check(fitted.system.empty()&&fitted.textRows>900&&fitted.textRows<=1024&&fitted.ids.size()<=1536,"actual preset text/prompt budget failure");
    const std::string prefix=task+"\n\nReference extract:\n";
    check(fitted.text.rfind(prefix,0)==0,"original task was altered");
    check(std::string(context.UTF8String).rfind(fitted.text.substr(prefix.size()),0)==0,"reference document was altered or padded");
    check(fitted.ids==demo::omniPrompt(tokenizer,"",fitted.text,image,audio,marker),"actual preset IDs mismatch");
    std::cout<<[preset[@"id"] UTF8String]<<" text_tokens="<<fitted.textRows<<" prompt_rows="<<fitted.ids.size()<<" prefill_rows="<<demo::actualPrefillRows(fitted.ids.size(),marker!=0)<<" image_rows="<<image<<" audio_rows="<<audio<<" marker="<<marker<<"\n";
  }
  std::cout<<"exact reference fixtures, 12 media/output budget combinations and six actual document tasks PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}}
