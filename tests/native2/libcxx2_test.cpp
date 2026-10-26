// native2 track: basic checks of the NDK's libc++ (libc++_shared.so) on the
// bionic shim, as an Android ELF executable run by gesher-exec
// (tests/windows/tracks/44-native2.ps1). Prints "libcxx2_test: all passed".
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("libcxx2_test: FAIL %s\n", what);
    ++g_failures;
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  // iostreams and formatting.
  std::ostringstream os;
  os << std::setw(6) << std::setfill('0') << 42 << ' ' << std::fixed << std::setprecision(3) << 3.14159 << ' '
     << std::hex << 255 << ' ' << std::boolalpha << true;
  Check(os.str() == "000042 3.142 ff true", "ostringstream formatting");
  std::istringstream is("12 3.5 word");
  int i;
  double d;
  std::string w;
  is >> i >> d >> w;
  Check(i == 12 && d == 3.5 && w == "word", "istringstream parsing");
  Check(std::to_string(1.5) == "1.500000" && std::stod("2.25") == 2.25 && std::stoll("-9000000000") == -9000000000LL,
        "to_string/stod/stoll");
  // Locale: the C locale and its facets.
  std::locale loc = std::locale::classic();
  Check(std::use_facet<std::ctype<char>>(loc).toupper('a') == 'A', "ctype facet");
  Check(std::use_facet<std::numpunct<char>>(loc).decimal_point() == '.', "numpunct facet");
  // Regex.
  std::regex re(R"((\w+)@(\w+)\.com)");
  std::smatch m;
  std::string mail = "write to gesher@example.com today";
  Check(std::regex_search(mail, m, re) && m[1] == "gesher" && m[2] == "example", "regex_search");
  Check(std::regex_replace(std::string("a1b22c333"), std::regex("[0-9]+"), "#") == "a#b#c#", "regex_replace");
  // Containers and algorithms.
  std::vector<int> v(1000);
  std::iota(v.begin(), v.end(), 0);
  std::mt19937 gen(7);
  std::shuffle(v.begin(), v.end(), gen);
  std::sort(v.begin(), v.end());
  Check(std::accumulate(v.begin(), v.end(), 0LL) == 499500 && std::is_sorted(v.begin(), v.end()), "sort/accumulate");
  std::unordered_map<std::string, int> um;
  for (int k = 0; k < 5000; ++k) um["k" + std::to_string(k)] = k;
  Check(um.size() == 5000 && um["k4999"] == 4999, "unordered_map");
  std::variant<int, std::string> var = std::string("x");
  Check(std::holds_alternative<std::string>(var), "variant");
  std::optional<int> opt;
  Check(!opt && opt.value_or(3) == 3, "optional");
  // random_device (getrandom / /dev/urandom).
  std::random_device rd;
  unsigned a = rd(), b = rd();
  Check(a != b || rd() != a, "random_device");
  // Threads, mutexes, condition variables, futures, shared_mutex.
  std::mutex mu;
  std::condition_variable cv;
  int ready = 0;
  std::vector<std::thread> threads;
  std::atomic<long> sum{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int k = 0; k < 10000; ++k) sum += k % (t + 1);
      std::lock_guard<std::mutex> lock(mu);
      ++ready;
      cv.notify_one();
    });
  }
  {
    std::unique_lock<std::mutex> lock(mu);
    Check(cv.wait_for(lock, std::chrono::seconds(30), [&] { return ready == 8; }), "condition_variable wait_for");
  }
  for (auto& t : threads) t.join();
  long want = 0;
  for (int t = 0; t < 8; ++t)
    for (int k = 0; k < 10000; ++k) want += k % (t + 1);
  Check(sum == want, "atomic sum across threads");
  auto f = std::async(std::launch::async, [] { return 6 * 7; });
  Check(f.get() == 42, "std::async");
  std::promise<std::string> pr;
  auto fut = pr.get_future();
  std::thread([&pr] { pr.set_value("done"); }).join();
  Check(fut.get() == "done", "promise/future");
  std::shared_mutex sm;
  {
    std::shared_lock<std::shared_mutex> r1(sm);
    std::shared_lock<std::shared_mutex> r2(sm, std::try_to_lock);
    Check(r2.owns_lock(), "shared_mutex shared twice");
  }
  Check(sm.try_lock(), "shared_mutex exclusive");
  sm.unlock();
  std::timed_mutex tm;
  tm.lock();
  auto t0 = std::chrono::steady_clock::now();
  bool got = std::async(std::launch::async, [&] { return tm.try_lock_for(std::chrono::milliseconds(100)); }).get();
  auto waited = std::chrono::steady_clock::now() - t0;
  Check(!got && waited >= std::chrono::milliseconds(90), "timed_mutex try_lock_for times out");
  tm.unlock();
  // Exceptions across threads and through std::function.
  std::exception_ptr ep;
  std::thread([&ep] {
    try {
      throw std::runtime_error("from a thread");
    } catch (...) {
      ep = std::current_exception();
    }
  }).join();
  try {
    std::rethrow_exception(ep);
  } catch (const std::runtime_error& e) {
    Check(std::string(e.what()) == "from a thread", "exception_ptr across threads");
  }
  std::function<void()> thrower = [] { throw std::out_of_range("range"); };
  bool caught = false;
  try {
    thrower();
  } catch (const std::logic_error&) {
    caught = true;
  }
  Check(caught, "exception through std::function, caught as a base");
  // Time.
  auto sys = std::chrono::system_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
  Check(secs > 1700000000, "system_clock");
  auto s0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(std::chrono::steady_clock::now() - s0 >= std::chrono::milliseconds(45), "sleep_for/steady_clock");
  // Files: fstream and std::filesystem.
  std::filesystem::path base = std::filesystem::path(dir) / "libcxx2";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "a" / "b");
  { std::ofstream(base / "a" / "b" / "f.txt") << "line1\nline2\n"; }
  std::ifstream in(base / "a" / "b" / "f.txt");
  std::string l1, l2;
  std::getline(in, l1);
  std::getline(in, l2);
  Check(l1 == "line1" && l2 == "line2", "ofstream/ifstream");
  in.close();
  Check(std::filesystem::file_size(base / "a" / "b" / "f.txt") == 12, "filesystem::file_size");
  int count = 0;
  for (auto& e : std::filesystem::recursive_directory_iterator(base)) count += e.is_regular_file() ? 1 : 0;
  Check(count == 1, "recursive_directory_iterator");
  std::filesystem::rename(base / "a" / "b" / "f.txt", base / "g.txt");
  Check(std::filesystem::exists(base / "g.txt") && !std::filesystem::exists(base / "a" / "b" / "f.txt"),
        "filesystem::rename");
  auto perms = std::filesystem::status(base / "g.txt").permissions();
  Check((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none, "filesystem::status");
  Check(std::filesystem::remove_all(base) == 4, "filesystem::remove_all");
  std::cout << "libcxx2_test: " << (g_failures == 0 ? "all passed" : "FAILED") << std::endl;
  return g_failures == 0 ? 0 : 1;
}
