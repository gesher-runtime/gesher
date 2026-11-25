// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include <winsock2.h>  // Keep Winsock out of the probe's Windows/COM include order.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include "worker_response_server.h"

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
};

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
  uint64_t deadline = 0;
  unsigned port = 0;
  mutable std::mutex mutex;
  std::map<std::string, unsigned> counts;

  std::string Response(const std::string& request) {
    const size_t end = request.find(' ', 4);
    if (!request.starts_with("GET ") || end == std::string::npos) {
      healthy = false; return {};
    }
    const std::string path = request.substr(4, end - 4);
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (counts.size() == 32 && !counts.contains(path)) { healthy = false; return {}; }
      ++counts[path];
    }
    std::string body, type = "text/plain";
    int status = 200;
    if (path == "/index.html") { body = kPage; type = "text/html"; }
    else if (path == "/sw.js") { body = kWorkerScript; type = "application/javascript"; }
    else if (path.starts_with("/network/")) body = "origin:" + path.substr(9);
    else { status = 404; body = "not found"; }
    return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK\r\n" : " Not Found\r\n") +
        "Connection: close\r\nCache-Control: no-store\r\nX-Probe-Origin: origin\r\nContent-Type: " + type +
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
            it->input.append(bytes, size);
            if (it->input.size() > 16384) { healthy = false; finished = true; }
            else if (it->input.find("\r\n\r\n") != std::string::npos) {
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
        if (finished) { closesocket(it->socket); it = clients.erase(it); }
        else ++it;
      }
    }
    for (const auto& client : clients) closesocket(client.socket);
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

}  // namespace worker_response_probe
