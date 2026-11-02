#pragma once
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
inline std::string DemoInputFileSHA256(const std::string& path,size_t limit){
  std::unique_ptr<FILE,decltype(&std::fclose)> file(std::fopen(path.c_str(),"rb"),&std::fclose);
  if(!file)throw std::runtime_error(std::string("Cannot read image input: ")+std::strerror(errno));
  CC_SHA256_CTX hash;CC_SHA256_Init(&hash);std::array<uint8_t,65536> buffer;size_t total=0;
  while(const size_t count=std::fread(buffer.data(),1,buffer.size(),file.get())){
    if(count>limit-total)throw std::runtime_error("Image exceeds the supported 128 MiB limit");
    total+=count;CC_SHA256_Update(&hash,buffer.data(),static_cast<CC_LONG>(count));
  }
  if(!std::feof(file.get()))throw std::runtime_error("Cannot read complete image input");
  uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&hash);std::string result;
  for(uint8_t value:digest){result.push_back("0123456789abcdef"[value>>4]);result.push_back("0123456789abcdef"[value&15]);}return result;
}
