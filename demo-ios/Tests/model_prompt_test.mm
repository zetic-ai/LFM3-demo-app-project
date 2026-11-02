#import <Foundation/Foundation.h>
#include "../Sources/ModelPrompt.hpp"
#include <iostream>
void check(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
int main(int argc,char** argv){@autoreleasepool{try{
  check(argc==7,"usage: model_prompt_test PRESETS.json DOCUMENT.txt TEXT_DIR AUDIO_DIR VL_DIR OMNI_DIR");
  const auto read=[](NSString* path){NSData* data=[NSData dataWithContentsOfFile:path];return data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;};
  NSArray* presets=read(@(argv[1]));NSString* document=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];check(presets.count==6&&document,"Missing actual task metadata or document");
  NSDictionary* roots=@{@"text":@(argv[3]),@"audio":@(argv[4]),@"vl":@(argv[5]),@"omni":@(argv[6])};
  for(NSDictionary* preset in presets){
    NSString* kind=preset[@"model"],*root=roots[kind];check(root!=nil,"Unknown preset model");
    NSDictionary* meta=read([root stringByAppendingPathComponent:@"metadata.json"]);size_t capacity=0;
    for(NSDictionary* module in meta[@"modules"])if([module[@"module_name"] isEqual:@"prefill"])for(NSDictionary* input in module[@"io"][@"inputs"])if([input[@"name"] isEqual:@"input_embeds"])capacity=[input[@"shape"][1] unsignedLongLongValue];
    check(capacity>0,"Missing actual prefill input shape");
    lfm3_ios::TextTokenizer tokenizer;std::string error;check(tokenizer.load([[root stringByAppendingPathComponent:@"tokenizer.json"] UTF8String],error),error.c_str());
    const std::string type=kind.UTF8String,task=[preset[@"task"] UTF8String],system=demo::systemForModel(type);const size_t image=[preset[@"image_rows"] unsignedLongLongValue],audio=[preset[@"audio_rows"] unsignedLongLongValue];const int marker=[preset[@"marker"] intValue];
    const auto render=[&](const std::string& text,bool media){return demo::renderModelPrompt(tokenizer,type,system,text,media?image:0,media?audio:0,marker);};
    auto fitted=demo::fitDocumentPrompt(task,document.UTF8String,[&](const std::string& text){return render(text,true);},[&](const std::string& text){return render(text,false).size();},capacity,image>0);
    check(fitted.ids.size()<=capacity&&fitted.textRows<=1024,"Model-specific prefill/text limit violated");
    if(image>0)check(fitted.text.rfind("Reference extract:\n",0)==0&&fitted.text.find("\n\nTask:\n"+task)!=std::string::npos,"VL task must follow the actual document");
    else check(fitted.text.rfind(task+"\n\nReference extract:\n",0)==0,"Task or actual document missing");
    check(fitted.ids==render(fitted.text,true),"Prepared IDs differ from shared SDK template");
    check(fitted.text.find("I'm preparing a short introduction")==std::string::npos,"Question audio transcript leaked into text");
    if(type=="vl")check(capacity==1024&&fitted.textRows<=766&&fitted.ids.size()==fitted.textRows+258,"VL image rows were omitted from the full 1024 budget");
    if(type=="text"){check(capacity==1024&&fitted.ids.size()==fitted.textRows,"Text full-prompt capacity mismatch");std::vector<int32_t> suffix;check(tokenizer.encode("<think></think>",true,suffix,error),"Thinking-off suffix encode failed");check(suffix.size()==2&&std::equal(suffix.rbegin(),suffix.rend(),fitted.ids.rbegin()),"Text prompt must explicitly disable thinking");}
    if(type=="audio")check(capacity==1536&&fitted.ids.size()==fitted.textRows+audio,"Audio prompt row mismatch");
    std::cout<<[preset[@"id"] UTF8String]<<" model="<<type<<" text_tokens="<<fitted.textRows<<" prompt_rows="<<fitted.ids.size()<<" prefill_capacity="<<capacity<<"\n";
  }
  std::cout<<"six actual tasks use their model tokenizer/template/capacity without filler or question transcript PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}}
