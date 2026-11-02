#pragma once
#include "text_tokenizer.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
namespace demo {
constexpr size_t prefillCapacity=1536,contextCapacity=2048;
inline std::vector<int32_t> omniPrompt(lfm3_ios::TextTokenizer& tokenizer,const std::string& system,
  const std::string& text,size_t imageRows,size_t audioRows,int32_t speechMarker=0){
  std::vector<int32_t> ids;std::string error;
  const auto append=[&](const std::string& value,bool special){std::vector<int32_t> part;if(!tokenizer.encode(value,special,part,error))throw std::runtime_error(error);ids.insert(ids.end(),part.begin(),part.end());};
  append("<|startoftext|>",true);
  if(!system.empty()){append("<|im_start|>system\n",true);append(system,false);append("<|im_end|>\n",true);}
  append("<|im_start|>user\n",true);
  if(imageRows){ids.push_back(125009);ids.insert(ids.end(),imageRows,125030);ids.push_back(125010);}
  if(audioRows){ids.push_back(125009);ids.insert(ids.end(),audioRows,125031);ids.push_back(125010);}
  append(text,false);append("<|im_end|>\n<|im_start|>assistant\n<think></think>",true);
  if(speechMarker)ids.push_back(speechMarker);
  return ids;
}
inline size_t actualPrefillRows(size_t promptRows,bool speech){return speech&&promptRows?promptRows-1:promptRows;}
struct FilledPrompt { std::string text,system;std::vector<int32_t> ids;size_t textRows=0; };
template<class RenderFull,class CountText>
inline FilledPrompt fitDocumentPrompt(const std::string& task,const std::string& document,
 RenderFull&& full,CountText&& textCount,size_t prefillLimit,bool taskLast=false){
  constexpr size_t textLimit=1024;
  FilledPrompt best{task,"",full(task),textCount(task)};
  if(best.ids.size()>prefillLimit || best.textRows>textLimit)throw std::runtime_error("Task exceeds this model's text-token or full-prefill capacity");
  std::vector<size_t> boundaries;
  for(size_t n=1;n<=document.size();++n){const char c=document[n-1];if((c=='.'||c=='!'||c=='?'||c=='\n') && (n==document.size()||document[n]==' '||document[n]=='\n'||document[n]=='\r'))boundaries.push_back(n);}
  if(boundaries.empty()||boundaries.back()!=document.size())boundaries.push_back(document.size());
  for(size_t n:boundaries){
    const std::string text=taskLast?"Reference extract:\n"+document.substr(0,n)+"\n\nTask:\n"+task:task+"\n\nReference extract:\n"+document.substr(0,n);
    auto ids=full(text);const size_t textRows=textCount(text);
    if(ids.size()<=prefillLimit && textRows<=textLimit && textRows>best.textRows)best={text,"",std::move(ids),textRows};
  }
  return best;
}
inline FilledPrompt buildDocumentPrompt(lfm3_ios::TextTokenizer& tokenizer,const std::string& task,
 const std::string& document,size_t imageRows,size_t audioRows,int32_t marker){
  return fitDocumentPrompt(task,document,[&](const std::string& text){return omniPrompt(tokenizer,"",text,imageRows,audioRows,marker);},
    [&](const std::string& text){return omniPrompt(tokenizer,"",text,0,0,marker).size();},prefillCapacity);
}
}
