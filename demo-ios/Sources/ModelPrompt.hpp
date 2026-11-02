#pragma once
#include "PromptContract.hpp"
#include "text_chat.h"
#include "lfm3_audio_session.h"
#include "lfm3_vision_session.h"
namespace demo {
inline std::string systemForModel(const std::string& kind){return kind=="audio"?"Answer the spoken question using the reference text provided by the user.":"";}
inline std::vector<int32_t> renderModelPrompt(lfm3_ios::TextTokenizer& tokenizer,const std::string& kind,
 const std::string& system,const std::string& user,size_t imageRows,size_t audioRows,int32_t speechMarker){
  std::vector<int32_t> ids;std::string error;
  const auto checked=[&](bool ok){if(!ok)throw std::runtime_error(error);};
  if(kind=="text"){lfm3_ios::ChatInput chat;chat.user=user;chat.system=system;checked(lfm3_ios::encodeTextChat(tokenizer,chat,ids,error));std::vector<int32_t> thinkingOff;checked(tokenizer.encode("<think></think>",true,thinkingOff,error));ids.insert(ids.end(),thinkingOff.begin(),thinkingOff.end());return ids;}
  if(kind=="audio"){
    std::string prefix,suffix;lfm3_ios::renderAudioPrompt(system,user,prefix,suffix);
    std::vector<int32_t> tail;checked(tokenizer.encode(prefix,true,ids,error)&&tokenizer.encode(suffix,true,tail,error));
    ids.insert(ids.end(),audioRows,0);ids.insert(ids.end(),tail.begin(),tail.end());return ids;
  }
  if(kind=="vl"){
    const auto prompt=lfm3_ios::visionQuestionPrompt(user);
    checked(tokenizer.encode(imageRows?lfm3_ios::renderVisionImagePrompt(prompt):prompt.prefix_text+prompt.suffix_text,true,ids,error));return ids;
  }
  if(kind!="omni")throw std::runtime_error("Unsupported model prompt kind");
  return omniPrompt(tokenizer,system,user,imageRows,audioRows,speechMarker);
}
}
