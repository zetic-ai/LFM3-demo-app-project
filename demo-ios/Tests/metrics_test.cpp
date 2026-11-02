#include "../Sources/DemoMetrics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
void check(bool condition){if(!condition)throw std::runtime_error("metric assertion failed");}
int main(){
  using demo::Span;
  check(demo::covered({{20,50},{40,80},{75,90}},0,100)==70);
  check(demo::covered({{0,40},{30,80},{80,150}},20,100)==80);
  check(demo::covered({{0,10},{110,200}},20,100)==0);
  check(demo::covered({{10,20}},20,20)==0);
  const std::vector<Span> loads{{10000000,40000000},{30000000,50000000},{90000000,120000000}};
  check(std::abs(demo::adjustedMs(0,80000000,loads)-40.)<1e-9);
  check(std::abs(demo::adjustedMs(0,130000000,loads)-60.)<1e-9);
  check(demo::rate(1,0,100,{})==0);
  check(std::abs(demo::rate(6,0,1000000000,{})-5.)<1e-9);
  auto time=demo::boundary(10000000,50000000,60000000,160000000);
  check(time.valid && time.verificationMs==40 && time.responseWallMs==100 && time.totalWallMs==150);
  check(!demo::boundary(10,5,20,30).valid && !demo::boundary(0,20,10,30).valid);
  std::cout<<"clipped union, nested intervals, first-event boundary and actual event-rate PASS\n";
}
