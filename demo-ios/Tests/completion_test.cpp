#include "../Sources/OperationCompletion.hpp"
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
int main(){
  for(int i=0;i<1000;++i){
    std::atomic<bool> busy{true};std::promise<void> published;auto ready=published.get_future();
    // Simulate terminal publication to another queue. That queue immediately
    // starts the next request, before the original worker has returned.
    std::thread worker([&]{demo::releaseThenPublish(busy,[&]{published.set_value();});});
    ready.wait();assert(!busy.exchange(true));assert(busy.exchange(true)); // a second simultaneous request remains rejected
    worker.join();demo::releaseThenPublish(busy,[]{ });assert(!busy.load());
  }
  // Exception/error completion uses the identical release primitive.
  std::atomic<bool> busy{true};demo::releaseThenPublish(busy,[&]{assert(!busy.exchange(true));});
  std::cout<<"1000 concurrent terminal-callback/immediate-start handoffs and duplicate-admission protection PASS\n";
}
