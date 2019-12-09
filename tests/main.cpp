#include "cpptime.h"
#include <chrono>
#include <iostream>
#include <thread>

class Explorer {
public:
  Explorer(): t() {}
  void explore() {
    std::cout << "exploring " << xx << std::endl;
  }

  void trigger() {
    auto id = t.add(std::chrono::seconds(2), std::chrono::seconds(4), &Explorer::explore, this);
    std::cout << "start sleep " << id  << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "end sleep " << id << std::endl;
    t.remove(id);
  }
  Explorer(Explorer &&r){}
  Explorer &operator=(Explorer &&ev) {}
  Explorer(const Explorer &r) {}
  Explorer &operator=(const Explorer &r) {}

private:
  CppTime::Timer<void(Explorer::*)(), Explorer*> t;
  const static int xx = 10;

};

int main(int argc, char **argv) {
  Explorer e;
  e.trigger();
  std::cout << "main end" << std::endl;
  return 0;
}
