// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include <winsock2.h>  // Keep Winsock out of the probe's Windows/COM include order.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <functional>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include "worker_response_server.h"
#include "worker_denial_scripts.h"

namespace worker_response_probe {
namespace {

// This script executes in an actual service worker. In particular, fetch and
// body-read errors have different catch blocks. A 403 is a resolved response.
constexpr char kWorkerScript[] = R"JS(
'use strict';
const workerIdentity = crypto.randomUUID();
self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('message', event => {
  event.waitUntil((async () => {
    const name = event.data;
    const abort = new AbortController();
    const request = new Request(new URL('/network/' + name, self.location.href),
                                {cache:'no-store', signal:abort.signal});
    const out = {event:'case', name, fetch:'pending', body:'none', status:0,
                 originHeader:'', bytes:0, text:'', error:'', errorName:'', timedOut:false,
                 aborted:false, requestUrl:request.url, finalRequestUrl:'', responseUrl:'',
                 workerIdentity, workerScript:self.location.href,
                 timingCount:0, timingName:'', timingType:''};
    performance.clearResourceTimings();
    const timer = setTimeout(() => { out.timedOut = true; abort.abort(); }, 10000);
    try {
      let response;
      try {
        response = await fetch(request);
      } catch (error) {
        out.fetch = 'rejected'; out.error = String(error).slice(0, 512);
        out.errorName = String(error.name).slice(0, 64);
      }
      if (response) {
        out.fetch = 'resolved'; out.status = response.status;
        out.responseUrl = response.url;
        out.originHeader = response.headers.get('X-Probe-Origin') || '';
        try {
          const reader = response.body.getReader();
          const decoder = new TextDecoder();
          for (;;) {
            const part = await reader.read();
            if (part.done) break;
            out.bytes += part.value.byteLength;
            if (out.bytes > 262144) { await reader.cancel(); throw new Error('body limit'); }
            if (out.text.length < 64) out.text += decoder.decode(part.value).slice(0, 64 - out.text.length);
          }
          out.body = 'complete';
        } catch (error) {
          out.body = 'rejected'; out.error = String(error).slice(0, 512);
          out.errorName = String(error.name).slice(0, 64);
        }
      }
    } finally {
      clearTimeout(timer);
      out.aborted = abort.signal.aborted;
      out.finalRequestUrl = request.url;
      // Let the resource-timing entry be delivered after fetch/body settlement.
      // Report absence or rewritten names instead of substituting the input URL.
      await new Promise(resolve => setTimeout(resolve, 0));
      const timings = performance.getEntriesByType('resource');
      out.timingCount = timings.length;
      if (timings.length === 1) {
        out.timingName = timings[0].name;
        out.timingType = timings[0].initiatorType;
      }
      event.ports[0].postMessage(out);
    }
  })());
});
)JS";

constexpr char kPage[] = R"HTML(<!doctype html><meta charset="utf-8"><title>worker response qualification</title>
<script>
'use strict';
(async () => {
  const post = value => chrome.webview.postMessage(value);
  const within = (promise, ms, label) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(label + ' timeout')), ms);
    promise.then(value => { clearTimeout(timer); resolve(value); }, error => { clearTimeout(timer); reject(error); });
  });
  let registration;
  try {
    post({event:'page-ready', secure:isSecureContext});
    const direct = await fetch('/network/document', {cache:'no-store'});
    post({event:'document', status:direct.status, body:await direct.text()});
    registration = await within(navigator.serviceWorker.register('/sw.js', {scope:'/'}), 15000, 'register');
    const worker = registration.active || registration.waiting || registration.installing;
    if (!worker) throw new Error('no actual worker');
    await new Promise((resolve, reject) => {
      const finish = error => {
        clearTimeout(timer); worker.removeEventListener('statechange', changed);
        error ? reject(error) : resolve();
      };
      const changed = () => {
        if (worker.state === 'activated') finish();
        else if (worker.state === 'redundant') finish(new Error('worker became redundant'));
      };
      const timer = setTimeout(() => finish(new Error('activation timeout')), 15000);
      worker.addEventListener('statechange', changed); changed();
    });
    post({event:'worker-ready', script:worker.scriptURL, state:worker.state, scope:registration.scope});
    for (const name of ['pass', 'fulfill', 'http403', 'first-read', 'first-abort', 'mid-body',
                       'uri-deferred', 'invoke-abort', 'followup']) {
      const result = await new Promise((resolve, reject) => {
        const channel = new MessageChannel();
        const timer = setTimeout(() => { channel.port1.close(); reject(new Error('worker message timeout: ' + name)); }, 12000);
        channel.port1.onmessage = event => {
          clearTimeout(timer); channel.port1.close();
          if (event.data.name !== name) reject(new Error('wrong worker result'));
          else resolve(event.data);
        };
        worker.postMessage(name, [channel.port2]);
      });
      post(result);
    }
    const unregistered = await within(registration.unregister(), 5000, 'unregister'); registration = null;
    post({event:'complete', unregistered});
  } catch (error) {
    if (registration) await within(registration.unregister(), 5000, 'failure unregister').catch(() => false);
    post({event:'failure', error:String(error).slice(0, 512)});
  }
})();
</script>)HTML";

struct Client {
  SOCKET socket = INVALID_SOCKET;
  std::string input, output;
  size_t sent = 0;
  uint64_t deadline = 0;
  size_t wanted = 0;
  bool recorded = false;
  size_t record_index = static_cast<size_t>(-1);
};

bool RequestLength(std::string_view headers, size_t* length) {
  *length = 0;
  bool seen = false;
  size_t start = headers.find("\r\n");
  if (start == std::string_view::npos) return false;
  start += 2;
  while (start < headers.size()) {
    size_t end = headers.find("\r\n", start);
    if (end == std::string_view::npos) end = headers.size();
    const size_t colon = headers.find(':', start);
    if (colon == std::string_view::npos || colon >= end) return false;
    std::string name(headers.substr(start, colon - start));
    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (name == "transfer-encoding") return false;
    if (name == "content-length") {
      if (seen) return false;
      seen = true;
      size_t position = colon + 1;
      while (position < end && headers[position] == ' ') ++position;
      if (position == end) return false;
      for (; position < end; ++position) {
        const char c = headers[position];
        if (c < '0' || c > '9') return false;
        *length = *length * 10 + c - '0';
        if (*length > 65536) return false;
      }
    }
    start = end + 2;
  }
  return true;
}

}  // namespace

struct LoopbackServer::Impl {
  ~Impl() {
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (thread) CloseHandle(thread);
    if (winsock) WSACleanup();
  }
  bool winsock = false;
  SOCKET listener = INVALID_SOCKET;
  HANDLE thread = nullptr;
  std::atomic<bool> stop{false}, healthy{true};
  std::atomic<unsigned> connections{0};
  std::atomic<uint64_t> received_bytes{0}, unattributed_bytes{0};
  uint64_t deadline = 0;
  unsigned port = 0;
  mutable std::mutex mutex;
  std::map<std::string, unsigned> counts;
  std::vector<RequestRecord> requests;
  std::string redirect_origin;

  bool Observe(Client& client) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!client.recorded) {
      const size_t first = client.input.find(' ');
      const size_t end = first == std::string::npos ? std::string::npos : client.input.find(' ', first + 1);
      if (end == std::string::npos) return true;  // Classify any leftover prefix at close.
      const std::string method = client.input.substr(0, first), path = client.input.substr(first + 1, end - first - 1);
      if (method.empty() || method.size() > 16 || path.empty() || path.front() != '/' ||
          path.find_first_of("\r\n\0", 0, 3) != std::string::npos) return false;
      for (char c : method) if (c < 'A' || c > 'Z') return false;
      if (counts.size() == 32 && !counts.contains(path)) return false;
      ++counts[path];  // A complete request target counts before its headers finish.
      client.record_index = requests.size();
      requests.push_back({method, path, {}, false, 0});
      client.recorded = true;
    }
    auto& record = requests[client.record_index];
    record.request_bytes = client.input.size();
    const size_t header_end = client.input.find("\r\n\r\n");
    if (header_end != std::string::npos) record.body = client.input.substr(header_end + 4);
    record.complete = client.wanted && client.input.size() == client.wanted;
    return true;
  }

  bool Receive(Client& client, const char* bytes, size_t size) {
    received_bytes += size;
    client.input.append(bytes, size);
    if (!Observe(client)) return false;
    const size_t header_end = client.input.find("\r\n\r\n");
    if (header_end == std::string::npos) return client.input.size() <= 16384;
    size_t body_length = 0;
    if (header_end > 16384 || !RequestLength(std::string_view(client.input).substr(0, header_end), &body_length)) return false;
    client.wanted = header_end + 4 + body_length;
    return Observe(client) && client.input.size() <= client.wanted;
  }

  void Finish(Client& client) {
    // Deadline/Stop may arrive while accepted bytes still wait in the socket.
    // Drain nonblocking input before classifying the connection's final prefix.
    char bytes[4096];
    while (client.input.size() <= 81920) {
      const int size = recv(client.socket, bytes, sizeof(bytes), 0);
      if (size <= 0) break;
      if (!Receive(client, bytes, static_cast<size_t>(size))) { healthy = false; break; }
    }
    if (!client.recorded && !client.input.empty()) {
      unattributed_bytes += client.input.size();
      healthy = false;
    }
    closesocket(client.socket);
  }

  std::string Response(const std::string& request) {
    const size_t first = request.find(' '), end = request.find(' ', first + 1);
    if (first == std::string::npos || end == std::string::npos) {
      healthy = false; return {};
    }
    const std::string method = request.substr(0, first), path = request.substr(first + 1, end - first - 1);
    const bool matrix = path.starts_with("/denial/network/");
    if (method != "GET" && (!matrix || (method != "POST" && method != "OPTIONS"))) {
      healthy = false; return {};
    }
    std::string redirect;
    {
      std::lock_guard<std::mutex> lock(mutex);
      redirect = redirect_origin;
    }
    std::string body, type = "text/plain", extra;
    int status = 200;
    if (path == "/index.html") { body = kPage; type = "text/html"; }
    else if (path == "/sw.js") { body = kWorkerScript; type = "application/javascript"; }
    else if (path.starts_with("/network/")) body = "origin:" + path.substr(9);
    else if (path.starts_with("/denial/index.html?")) { body = worker_denial_probe::kPage; type = "text/html"; }
    else if (path.starts_with("/denial/observer.html?")) { body = worker_denial_probe::kPage; type = "text/html"; }
    else if (path == "/denial/sw.js") { body = worker_denial_probe::kWorker; type = "application/javascript"; }
    else if (matrix) {
      extra = "Access-Control-Allow-Origin: *\r\nTiming-Allow-Origin: *\r\n"
              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
              "Access-Control-Allow-Headers: content-type, x-gesher-diagnostic\r\n"
              "Access-Control-Expose-Headers: X-Probe-Origin\r\nAccess-Control-Max-Age: 0\r\n";
      if (method == "OPTIONS") { status = 204; }
      else if (path == "/denial/network/redirect-pass" || path == "/denial/network/redirect-deny") {
        if (redirect.empty()) { healthy = false; return {}; }
        status = 302; extra += "Location: " + redirect + path + "-target\r\n";
      } else body = "origin:" + path.substr(16);
    }
    else { status = 404; body = "not found"; }
    return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK\r\n" : status == 204 ? " No Content\r\n" :
        status == 302 ? " Found\r\n" : " Not Found\r\n") +
        "Connection: close\r\nCache-Control: no-store\r\nX-Probe-Origin: origin\r\n" + extra + "Content-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }

  void Run() {
    std::vector<Client> clients;
    while (!stop && GetTickCount64() < deadline && healthy) {
      fd_set reads, writes; FD_ZERO(&reads); FD_ZERO(&writes); FD_SET(listener, &reads);
      for (const auto& client : clients) {
        if (client.output.empty()) FD_SET(client.socket, &reads);
        else FD_SET(client.socket, &writes);
      }
      timeval interval{0, 50000};
      if (select(0, &reads, &writes, nullptr, &interval) == SOCKET_ERROR) { healthy = false; break; }
      if (FD_ISSET(listener, &reads)) {
        SOCKET accepted = accept(listener, nullptr, nullptr);
        if (accepted != INVALID_SOCKET) {
          ++connections;  // Count TCP acceptance even if no HTTP request follows.
          u_long nonblocking = 1;
          if (clients.size() == 16 || ioctlsocket(accepted, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(accepted); healthy = false;
          } else clients.push_back({accepted, {}, {}, 0, GetTickCount64() + 3000});
        } else if (WSAGetLastError() != WSAEWOULDBLOCK) healthy = false;
      }
      for (auto it = clients.begin(); it != clients.end();) {
        bool finished = GetTickCount64() >= it->deadline;
        if (!finished && FD_ISSET(it->socket, &reads)) {
          char bytes[4096]; const int size = recv(it->socket, bytes, sizeof(bytes), 0);
          if (size > 0) {
            if (!Receive(*it, bytes, static_cast<size_t>(size))) { healthy = false; finished = true; }
            else if (it->wanted && it->input.size() == it->wanted) {
              it->output = Response(it->input);
              if (it->output.empty()) finished = true;
            }
          } else if (size == 0 || WSAGetLastError() != WSAEWOULDBLOCK) finished = true;
        }
        if (!finished && FD_ISSET(it->socket, &writes)) {
          const int size = send(it->socket, it->output.data() + it->sent,
                                static_cast<int>(it->output.size() - it->sent), 0);
          if (size > 0) { it->sent += size; finished = it->sent == it->output.size(); }
          else if (size == 0 || WSAGetLastError() != WSAEWOULDBLOCK) finished = true;
        }
        if (finished) { Finish(*it); it = clients.erase(it); }
        else ++it;
      }
    }
    for (auto& client : clients) Finish(client);
    std::lock_guard<std::mutex> lock(mutex);
    uint64_t accounted = unattributed_bytes;
    for (const auto& request : requests) accounted += request.request_bytes;
    if (accounted != received_bytes) healthy = false;
  }
};

LoopbackServer::LoopbackServer() : impl_(std::make_shared<Impl>()) {}
LoopbackServer::~LoopbackServer() { Stop(); }

bool LoopbackServer::Start(uint64_t deadline) {
  if (impl_->winsock || impl_->thread) return false;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
  impl_->winsock = true; impl_->deadline = deadline;
  impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (impl_->listener == INVALID_SOCKET) return false;
  BOOL exclusive = TRUE; u_long nonblocking = 1;
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
  int length = sizeof(address);
  if (setsockopt(impl_->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
      bind(impl_->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
      getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR ||
      listen(impl_->listener, 16) == SOCKET_ERROR ||
      ioctlsocket(impl_->listener, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
  impl_->port = ntohs(address.sin_port);
  auto retained = std::make_unique<std::shared_ptr<Impl>>(impl_);
  impl_->thread = CreateThread(nullptr, 0, [](void* arg) -> DWORD {
    std::unique_ptr<std::shared_ptr<Impl>> owner(static_cast<std::shared_ptr<Impl>*>(arg));
    (*owner)->Run(); return 0;
  }, retained.get(), 0, nullptr);
  if (!impl_->thread) return false;
  retained.release(); return true;
}

bool LoopbackServer::Stop() {
  impl_->stop = true;
  // Nonblocking socket operations and 50 ms select waits make normal shutdown
  // prompt. The thread owns Impl if the OS fails to join within this bound.
  return !impl_->thread || WaitForSingleObject(impl_->thread, 5000) == WAIT_OBJECT_0;
}

bool LoopbackServer::Healthy() const { return impl_->healthy; }
std::wstring LoopbackServer::Origin() const { return L"http://127.0.0.1:" + std::to_wstring(impl_->port); }
unsigned LoopbackServer::Count(const std::string& path) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto found = impl_->counts.find(path); return found == impl_->counts.end() ? 0 : found->second;
}
unsigned LoopbackServer::TotalCount() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  unsigned total = 0;
  for (const auto& [path, count] : impl_->counts) total += count;
  return total;
}
unsigned LoopbackServer::Connections() const { return impl_->connections; }
uint64_t LoopbackServer::ReceivedBytes() const { return impl_->received_bytes; }
uint64_t LoopbackServer::UnattributedBytes() const { return impl_->unattributed_bytes; }
std::vector<RequestRecord> LoopbackServer::Requests() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->requests;
}
bool LoopbackServer::SetRedirectOrigin(const std::wstring& origin) {
  constexpr std::wstring_view prefix = L"http://127.0.0.1:";
  if (!origin.starts_with(prefix) || origin.size() <= prefix.size() || origin.size() > prefix.size() + 5) return false;
  unsigned port = 0;
  for (wchar_t c : std::wstring_view(origin).substr(prefix.size())) {
    if (c < L'0' || c > L'9') return false;
    port = port * 10 + c - L'0';
  }
  if (!port || port > 65535) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->redirect_origin.assign(origin.begin(), origin.end());
  return true;
}

bool CheckLoopbackRequestAccounting(uint64_t deadline) {
  deadline = std::min<uint64_t>(deadline, GetTickCount64() + 12000);
  const auto wait = [&](const std::function<bool()>& done) {
    while (!done()) { if (GetTickCount64() >= deadline) return false; Sleep(1); }
    return true;
  };
  struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { Close(); }
    void Close() { if (value != INVALID_SOCKET) { closesocket(value); value = INVALID_SOCKET; } }
  };
  const auto send_prefix = [&](LoopbackServer& server, const std::string& input, Socket* result) {
    result->value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (result->value == INVALID_SOCKET) return false;
    u_long nonblocking = 1;
    if (ioctlsocket(result->value, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
    const std::wstring origin = server.Origin();
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(std::wcstoul(origin.c_str() + origin.find_last_of(':') + 1, nullptr, 10)));
    if (connect(result->value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) return false;
    int error = 0;
    if (!wait([&] {
      fd_set writable, errors; FD_ZERO(&writable); FD_ZERO(&errors);
      FD_SET(result->value, &writable); FD_SET(result->value, &errors); timeval poll{};
      const int ready = select(0, nullptr, &writable, &errors, &poll);
      if (ready == SOCKET_ERROR) { error = WSAGetLastError(); return true; }
      return ready > 0;
    }) || error) return false;
    int error_length = sizeof(error);
    if (getsockopt(result->value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_length) == SOCKET_ERROR || error) return false;
    size_t sent = 0;
    while (sent < input.size()) {
      const int count = send(result->value, input.data() + sent, static_cast<int>(input.size() - sent), 0);
      if (count > 0) sent += count;
      else if (!count || WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount64() >= deadline) return false;
      else Sleep(1);
    }
    return true;
  };
  {
    LoopbackServer server;
    if (!server.Start(deadline)) return false;
    const std::vector<std::string> inputs = {
        "POST /denial/network/partial-headers HTTP/1.1\r\nContent-Length: 32768\r\nX",
        "POST /denial/network/partial-line ",
        "POST /denial/network/partial-body HTTP/1.1\r\nContent-Length: 7\r\n\r\nab",
        "POST /denial/network/full HTTP/1.1\r\nContent-Length: 4\r\n\r\ndata",
        "POST /denial/network/malformed-partial HTTP/1.1\r\nNot-A-Header"};
    uint64_t expected = 0;
    for (const auto& input : inputs) {
      Socket peer;
      if (!send_prefix(server, input, &peer)) return false;
      expected += input.size();
      if (!wait([&] { return server.ReceivedBytes() >= expected; })) return false;
      peer.Close();
    }
    Socket empty;
    if (!send_prefix(server, {}, &empty) || !wait([&] { return server.Connections() == inputs.size() + 1; }) ||
        !server.Stop() || !server.Healthy() || server.UnattributedBytes() || server.ReceivedBytes() != expected) return false;
    const auto rows = server.Requests();
    if (rows.size() != inputs.size()) return false;
    const char* routes[] = {"partial-headers", "partial-line", "partial-body", "full", "malformed-partial"};
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].path != std::string("/denial/network/") + routes[i] ||
          rows[i].method != "POST" || rows[i].request_bytes != inputs[i].size() ||
          rows[i].complete != (i == 3) || rows[i].body != (i == 2 ? "ab" : i == 3 ? "data" : "") ||
          server.Count(rows[i].path) != 1) return false;
    }
  }
  // EOF, the per-client deadline, and Stop must all reject a nonempty prefix
  // whose request target is incomplete. Empty speculative connections above
  // are intentionally not treated as HTTP requests.
  for (unsigned finish = 0; finish < 3; ++finish) {
    LoopbackServer server; Socket peer;
    if (!server.Start(deadline) || !send_prefix(server, "POST /unfinished", &peer) ||
        !wait([&] { return server.ReceivedBytes() == 16; })) return false;
    if (finish == 0) peer.Close();
    if (finish != 2 && !wait([&] { return !server.Healthy(); })) return false;
    if (!server.Stop() || server.Healthy() || server.UnattributedBytes() != 16 || !server.Requests().empty()) return false;
  }
  {
    LoopbackServer server; Socket peer;
    const std::string malformed = "POST /denial/network/malformed HTTP/1.1\r\nNot-A-Header\r\n\r\n";
    if (!server.Start(deadline) || !send_prefix(server, malformed, &peer) ||
        !wait([&] { return !server.Healthy(); }) || !server.Stop() || server.UnattributedBytes() ||
        server.ReceivedBytes() != malformed.size() || server.Count("/denial/network/malformed") != 1) return false;
  }
  {
    // The storage-deletion fixture shares this server and fetches this ordinary
    // route every 25 ms. Repeated successful requests must not exhaust a record
    // limit while the owned production operation is still in progress.
    LoopbackServer server;
    if (!server.Start(deadline)) return false;
    const std::string request = "GET /network/storage-delete-ping HTTP/1.1\r\nHost: loopback\r\n\r\n";
    for (unsigned i = 0; i < 80; ++i) {
      Socket peer;
      if (!send_prefix(server, request, &peer)) return false;
      std::string response; bool failed = false;
      if (!wait([&] {
        char bytes[4096]; const int count = recv(peer.value, bytes, sizeof(bytes), 0);
        if (count > 0) { response.append(bytes, count); if (response.size() > 4096) failed = true; return failed; }
        if (!count) return true;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return false;
        failed = true; return true;
      }) || failed || !response.starts_with("HTTP/1.1 200 OK\r\n") ||
          !response.ends_with("\r\n\r\norigin:storage-delete-ping")) return false;
    }
    if (!server.Stop() || !server.Healthy() || server.UnattributedBytes() ||
        server.Count("/network/storage-delete-ping") != 80 || server.ReceivedBytes() != 80 * request.size()) return false;
    const auto rows = server.Requests();
    if (rows.size() != 80) return false;
    for (const auto& row : rows) if (row.method != "GET" || row.path != "/network/storage-delete-ping" ||
        !row.complete || !row.body.empty() || row.request_bytes != request.size()) return false;
  }
  return true;
}

}  // namespace worker_response_probe
