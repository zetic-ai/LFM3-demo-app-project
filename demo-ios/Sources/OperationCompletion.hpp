#pragma once
#include <atomic>
#include <utility>
namespace demo {
// Admission must reopen before a terminal callback can synchronously start
// another operation. Intermediate streaming callbacks retain the gate.
template<class Publish> void releaseThenPublish(std::atomic<bool>& busy,Publish&& publish){
  busy.store(false,std::memory_order_release);
  std::forward<Publish>(publish)();
}
}
