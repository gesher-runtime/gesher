// Virtual resource retirement after a CRT close or replacement. The caller
// holds the descriptor transaction for Retire, then releases it before Finish.
#pragma once
#include <cstdint>
#include <memory>
namespace gesher {
namespace ws { struct SocketDescription; }
class RetiredDescriptor {
 public:
  ~RetiredDescriptor();
  void Retire(int fd);
  int Finish();
 private:
  uintptr_t socket_ = ~uintptr_t{0};
  std::shared_ptr<ws::SocketDescription> description_;
};
}
