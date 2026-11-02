#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace demo {
struct Span { uint64_t begin=0,end=0; };
// Clip before merging: a load observed after the first event must not be
// subtracted from TTFT, and nested observers must never be counted twice.
inline uint64_t covered(std::vector<Span> spans,uint64_t begin,uint64_t end) {
  if(end<=begin)return 0;
  std::vector<Span> clipped;
  for(auto s:spans){s.begin=std::max(begin,s.begin);s.end=std::min(end,s.end);if(s.end>s.begin)clipped.push_back(s);}
  std::sort(clipped.begin(),clipped.end(),[](auto a,auto b){return a.begin<b.begin;});
  uint64_t total=0,lo=0,hi=0;bool active=false;
  for(auto s:clipped){if(!active){lo=s.begin;hi=s.end;active=true;}else if(s.begin<=hi)hi=std::max(hi,s.end);else{total+=hi-lo;lo=s.begin;hi=s.end;}}
  return total+(active?hi-lo:0);
}
inline double adjustedMs(uint64_t begin,uint64_t end,const std::vector<Span>& loads) {
  return end>begin?static_cast<double>((end-begin)-covered(loads,begin,end))/1e6:0;
}
inline double rate(size_t events,uint64_t first,uint64_t last,const std::vector<Span>& loads) {
  const double seconds=adjustedMs(first,last,loads)/1000.;return events>1&&seconds>0?static_cast<double>(events-1)/seconds:0;
}
struct Boundary {bool valid=false;double verificationMs=0,responseWallMs=0,totalWallMs=0;};
inline Boundary boundary(uint64_t openBegin,uint64_t openEnd,uint64_t responseBegin,uint64_t end){
  if(openEnd<openBegin||responseBegin<openEnd||end<responseBegin)return {};
  return {true,(openEnd-openBegin)/1e6,(end-responseBegin)/1e6,(end-openBegin)/1e6};
}
}
