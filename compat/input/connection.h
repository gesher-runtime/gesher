// Gesher: an input connection, a Gesher window's input published on an
// InputChannel (gesher_input.h, docs/input.md).
#pragma once

#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <input/InputTransport.h>

#include "gesher_input.h"
#include "gesher_window.h"
#include "translate.h"

namespace gesher {
namespace input {

struct ConnectionOptions {
  TranslateOptions translate;
  int32_t display_id = 0;
  int32_t max_in_flight = 32;
  bool initial_focus = false;
  // Window events that are not input, and focus changes, on the
  // connection's thread.
  std::function<void(const WindowEvent&)> on_window_event;
  // Runs on the connection's thread as it ends (a JNI caller detaches it
  // from the VM).
  std::function<void()> on_thread_exit;
};

class Connection {
 public:
  // Starts the connection's thread. `window` must outlive the connection.
  Connection(Window* window, std::shared_ptr<android::InputChannel> channel,
             ConnectionOptions options);
  ~Connection();  // stops and joins the thread

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Handles `event` on the connection's thread as if the window had queued it.
  void Inject(const WindowEvent& event);
  gesher_input_stats Stats() const;

 private:
  void Run();
  void Handle(const WindowEvent& event);
  void Enqueue(const AndroidEvent& event);
  void ReadResponses();
  void SendPending();
  android::status_t Publish(uint32_t seq, const AndroidEvent& event);
  int64_t EventTime(const WindowEvent& event);

  Window* window_;
  std::shared_ptr<android::InputChannel> channel_;
  android::InputPublisher publisher_;
  ConnectionOptions options_;
  Translator translator_;

  int wake_fd_ = -1;  // eventfd: Inject and the destructor
  std::mutex mutex_;
  std::deque<WindowEvent> injected_;
  bool stop_ = false;

  // The connection's thread only.
  std::deque<AndroidEvent> pending_;
  std::unordered_set<uint32_t> in_flight_;
  uint32_t next_seq_ = 1;
  int32_t next_event_id_ = 1;
  int64_t last_time_ = 0;

  std::atomic<uint64_t> published_{0}, finished_{0}, handled_{0}, dropped_{0};
  std::atomic<uint64_t> in_flight_count_{0};

  std::thread thread_;
};

}  // namespace input
}  // namespace gesher
