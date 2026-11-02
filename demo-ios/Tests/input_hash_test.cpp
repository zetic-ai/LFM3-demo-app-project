#include "../Sources/InputFileHash.hpp"
#include <cassert>
#include <unistd.h>
#include <iostream>
int main(){
  char name[]="/tmp/lfm3-input-hash-XXXXXX";int file=mkstemp(name);assert(file>=0);assert(write(file,"abc",3)==3);close(file);
  const std::string expected="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  assert(DemoInputFileSHA256(name,3)==expected);bool rejected=false;
  try{DemoInputFileSHA256(name,2);}catch(const std::exception&){rejected=true;}assert(rejected);
  assert(DemoInputFileSHA256(name,3)==expected);unlink(name);rejected=false;
  try{DemoInputFileSHA256(name,3);}catch(const std::exception&){rejected=true;}assert(rejected);
  std::cout<<"bounded streaming image SHA256, unchanged original, missing-file rejection PASS\n";
}
