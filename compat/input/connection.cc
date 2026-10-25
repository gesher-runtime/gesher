// Gesher: an input connection. See connection.h, gesher_input.h and
// docs/input.md.

#define LOG_TAG "GesherInput"

#include "connection.h"
#include "win32.h"

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <variant>

#include <android-base/unique_fd.h>
#include <attestation/HmacKeyManager.h>
#include <binder/Binder.h>
#include <log/log.h>
#include <ui/Transform.h>
#include <utils/Timers.h>

namespace gesher {
namespace input {

using android::InputChannel;
using android::InputPublisher;
using android::status_t;

Connection::Connection(Window* window, std::shared_ptr<InputChannel> channel,
                       ConnectionOptions options)
    : window_(window),
      channel_(std::move(channel)),
      publisher_(channel_),
      options_(std::move(options)),
      translator_(options_.translate) {
  if (options_.max_in_flight < 1) options_.max_in_flight = 32;
  // The window's input method side: text and compositions for its editor.
  ime_ = ImeWindow::Attach(window_->hwnd());
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  thread_ = std::thread([this] { Run(); });
}

Connection::~Connection() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
  if (thread_.joinable()) thread_.join();
  ime_.reset();  // the window's procedure is its own again
  if (wake_fd_ >= 0) close(wake_fd_);
}

void Connection::SetTextInput(const TextInputState& state) {
  if (ime_) ime_->SetTextInput(state);
}

void Connection::SetCaret(int32_t x, int32_t y, int32_t width, int32_t height) {
  if (ime_) ime_->SetCaret(x, y, width, height);
}

void Connection::CompleteComposition(bool cancel) {
  if (ime_) ime_->CompleteComposition(cancel);
}

int32_t Connection::ShowTouchKeyboard(bool show) {
  return ime_ ? ime_->ShowTouchKeyboard(show) : -1;
}

void Connection::FinishTextEvent(uint32_t seq) {
  text_finished_ = seq;
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
}

void Connection::Inject(const WindowEvent& event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    injected_.push_back(event);
  }
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
}

gesher_input_stats Connection::Stats() const {
  gesher_input_stats s;
  s.published = published_;
  s.finished = finished_;
  s.handled = handled_;
  s.in_flight = in_flight_count_;
  s.dropped = dropped_;
  return s;
}

int64_t Connection::EventTime(const WindowEvent& event) {
  // Android's event times are SystemClock.uptimeMillis's clock; a message's
  // GetMessageTime() is the tick count when it was posted.
  int64_t t = systemTime(SYSTEM_TIME_MONOTONIC);
  if (event.time_ms != 0) t -= std::min<int64_t>(MessageAgeMs(event.time_ms), 1000) * 1000000;
  t = std::max(t, last_time_);  // never earlier than an event already sent
  last_time_ = t;
  return t;
}

void Connection::Run() {
  if (options_.initial_focus) {
    WindowEvent focus;
    focus.type = WindowEvent::kFocus;
    focus.button = 1;
    Handle(focus);
  }
  for (;;) {
    {
      std::deque<WindowEvent> injected;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) break;
        injected.swap(injected_);
      }
      eventfd_t ignored;
      eventfd_read(wake_fd_, &ignored);  // nonblocking: clears the count
      for (const WindowEvent& e : injected) Handle(e);
    }
    WindowEvent event;
    while (window_->PollEvent(&event)) Handle(event);
    ReadResponses();
    SendPending();

    // Wait for the window, the consumer or an injection. While events wait
    // for the consumer's finished signals, the channel's fd wakes the loop.
    struct pollfd fds[3] = {
        {window_->event_fd(), POLLIN, 0},
        {channel_->getFd(), POLLIN, 0},
        {wake_fd_, POLLIN, 0},
    };
    // A send the channel refused (full) is retried after a while.
    // A text event's finish is waited for in short steps too.
    const bool blocked = (!pending_.empty() &&
                          in_flight_.size() < static_cast<size_t>(options_.max_in_flight)) ||
                         text_waiting_;
    int n = poll(fds, 3, blocked ? 10 : 1000);
    if (n < 0 && errno != EINTR) {
      ALOGE("input connection '%s': poll failed: %s", channel_->getName().c_str(),
            strerror(errno));
      break;
    }
  }
  if (options_.on_thread_exit) options_.on_thread_exit();
}

void Connection::Handle(const WindowEvent& event) {
  if (event.type == WindowEvent::kChar && event.ch == kTextEventMarker) {
    // The input method side's text event, in its place among the window's events.
    TextEvent text;
    if (ime_ && ime_->TakeTextEvent(&text)) HandleText(text);
    return;
  }
  if (!Translator::IsInput(event) || event.type == WindowEvent::kFocus) {
    if (options_.on_window_event) options_.on_window_event(event);
  }
  if (!Translator::IsInput(event)) return;
  std::vector<AndroidEvent> out;
  translator_.Translate(event, EventTime(event), &out);
  for (const AndroidEvent& e : out) Enqueue(e);
}

void Connection::HandleText(const TextEvent& text) {
  if (!options_.on_text_event) {
    // No text listener: typed text as characters, as the window queues them.
    if (text.kind != TextEvent::kCommit || !options_.on_window_event) return;
    for (char16_t unit : text.text) {
      WindowEvent c;
      c.type = WindowEvent::kChar;
      c.ch = unit;
      options_.on_window_event(c);
    }
    return;
  }
  AndroidEvent e;
  e.kind = AndroidEvent::kText;
  e.text_id = next_text_id_++;
  texts_[e.text_id] = text;
  pending_.push_back(e);
}

void Connection::Enqueue(const AndroidEvent& event) {
  // A move waiting behind unfinished events replaces the move before it, as
  // InputDispatcher's queue does when an app is slow.
  if (event.IsCoalescibleMove() && !pending_.empty()) {
    AndroidEvent& last = pending_.back();
    if (last.IsCoalescibleMove() && last.action == event.action &&
        last.device_id == event.device_id && last.pointer_count == event.pointer_count &&
        std::equal(last.properties, last.properties + last.pointer_count, event.properties)) {
      last = event;
      return;
    }
  }
  pending_.push_back(event);
}

void Connection::ReadResponses() {
  for (;;) {
    android::base::Result<InputPublisher::ConsumerResponse> response =
        publisher_.receiveConsumerResponse();
    if (!response.ok()) {
      if (response.error().code() != android::WOULD_BLOCK) {
        ALOGW("input connection '%s': receiving a response failed: %s",
              channel_->getName().c_str(), response.error().message().c_str());
      }
      return;
    }
    if (const auto* finished = std::get_if<InputPublisher::Finished>(&*response)) {
      if (in_flight_.erase(finished->seq) != 0) {
        finished_++;
        if (finished->handled) handled_++;
        in_flight_count_ = in_flight_.size();
      }
    }
    // Timelines (graphics timing for an event) are not used.
  }
}

void Connection::SendPending() {
  for (;;) {
    if (text_waiting_) {
      // What follows a text event waits until the editor has it.
      if (text_finished_ != text_seq_ && systemTime(SYSTEM_TIME_MONOTONIC) < text_deadline_) {
        return;
      }
      text_waiting_ = false;
    }
    if (pending_.empty()) return;
    if (pending_.front().kind == AndroidEvent::kText) {
      // A text event waits until the events before it are handled.
      if (!in_flight_.empty()) return;
      auto it = texts_.find(pending_.front().text_id);
      pending_.pop_front();
      if (it == texts_.end()) continue;
      TextEvent text = std::move(it->second);
      texts_.erase(it);
      text_seq_ = text_seq_ == UINT32_MAX ? 1 : text_seq_ + 1;
      text_waiting_ = true;
      text_deadline_ = systemTime(SYSTEM_TIME_MONOTONIC) + 1000000000LL;
      options_.on_text_event(text, text_seq_);
      continue;
    }
    if (in_flight_.size() >= static_cast<size_t>(options_.max_in_flight)) return;
    const uint32_t seq = next_seq_;
    status_t status = Publish(seq, pending_.front());
    if (status == android::WOULD_BLOCK) return;  // the channel is full: later
    pending_.pop_front();
    if (status != android::OK) {
      dropped_++;
      ALOGW("input connection '%s': publishing failed (%d)", channel_->getName().c_str(),
            status);
      continue;
    }
    next_seq_ = next_seq_ == UINT32_MAX ? 1 : next_seq_ + 1;
    in_flight_.insert(seq);
    in_flight_count_ = in_flight_.size();
    published_++;
  }
}

status_t Connection::Publish(uint32_t seq, const AndroidEvent& e) {
  const int32_t event_id = next_event_id_++;
  const android::ui::LogicalDisplayId display{options_.display_id};
  switch (e.kind) {
    case AndroidEvent::kKey:
      return publisher_.publishKeyEvent(seq, event_id, e.device_id, e.source, display,
                                        android::INVALID_HMAC, e.action, e.flags, e.key_code,
                                        e.scan_code, e.meta_state, e.repeat_count, e.down_time,
                                        e.event_time);
    case AndroidEvent::kMotion: {
      const android::ui::Transform identity;
      return publisher_.publishMotionEvent(
          seq, event_id, e.device_id, e.source, display, android::INVALID_HMAC, e.action,
          e.action_button, e.flags, e.meta_state, e.button_state,
          android::MotionClassification::NONE, identity, 1.0f, 1.0f, e.cursor_x, e.cursor_y,
          identity, e.down_time, e.event_time, e.pointer_count, e.properties, e.coords);
    }
    case AndroidEvent::kFocus:
      return publisher_.publishFocusEvent(seq, event_id, e.has_focus);
    case AndroidEvent::kText:
      break;  // never published (SendPending hands it to on_text_event)
  }
  return android::BAD_VALUE;
}

}  // namespace input
}  // namespace gesher

// --- The C API (gesher_input.h) ------------------------------------------------

struct gesher_input_connection {
  std::unique_ptr<gesher::input::Connection> connection;
};

extern "C" gesher_input_connection* gesher_input_attach(void* hwnd, int channel_fd,
                                                        const char* name,
                                                        const gesher_input_options* options) {
  gesher::Window* window = gesher::input::WindowFromHwnd(hwnd);
  if (window == nullptr || channel_fd < 0) return nullptr;
  android::base::unique_fd fd(dup(channel_fd));
  if (!fd.ok()) return nullptr;
  android::os::InputChannelCore core;
  core.name = name != nullptr ? name : "gesher-window";
  core.fd = android::os::ParcelFileDescriptor(std::move(fd));
  core.token = android::sp<android::BBinder>::make();
  std::shared_ptr<android::InputChannel> channel = android::InputChannel::create(std::move(core));
  if (channel == nullptr) return nullptr;
  gesher::input::ConnectionOptions o;
  if (options != nullptr) {
    o.translate.scale = options->scale != 0 ? options->scale : 1.0f;
    o.translate.offset_x = options->offset_x;
    o.translate.offset_y = options->offset_y;
    o.display_id = options->display_id;
    if (options->max_in_flight > 0) o.max_in_flight = options->max_in_flight;
    o.initial_focus = options->initial_focus != 0;
    if (options->on_window_event != nullptr) {
      auto callback = options->on_window_event;
      void* cookie = options->cookie;
      o.on_window_event = [callback, cookie](const gesher::WindowEvent& event) {
        callback(cookie, &event);
      };
    }
  }
  auto* c = new gesher_input_connection;
  c->connection = std::make_unique<gesher::input::Connection>(window, std::move(channel),
                                                              std::move(o));
  return c;
}

extern "C" void gesher_input_detach(gesher_input_connection* connection) {
  delete connection;
}

extern "C" void gesher_input_inject(gesher_input_connection* connection, const void* event) {
  connection->connection->Inject(*static_cast<const gesher::WindowEvent*>(event));
}

extern "C" void gesher_input_get_stats(gesher_input_connection* connection,
                                       gesher_input_stats* out) {
  *out = connection->connection->Stats();
}
