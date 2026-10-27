#include "net4_resolver.h"
#include <iostream>
int main(int argc, char** argv) {
  if (argc < 2 || (strcmp(argv[1], "allow") != 0 && strcmp(argv[1], "deny") != 0)) return 2;
  const std::string report = net4resolver::Run(strcmp(argv[1], "allow") == 0, argc > 2 ? argv[2] : nullptr);
  std::cout << report << std::flush;
  return report.find("net4 resolver: all passed;") != std::string::npos ? 0 : 1;
}
