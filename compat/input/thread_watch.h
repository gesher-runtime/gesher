// Gesher: a watch on a window's thread (docs/input.md, "Text input"). An IME
// or the text services framework can keep the thread that owns a window
// waiting (inside GetMessage's key processing, a layout switch, or a call
// into the IME's process), so that the window takes no input for a long
// time. The watch posts the window a message every second; when one waits
// longer than 5 s, the thread's stack is written to stderr (module!export+off
// per frame, from the x64 unwind tables), again a minute later while the wait
// lasts, and how long it was once the thread answers.
#pragma once

#include <stdint.h>

namespace gesher {
namespace input {

class ThreadWatch;

// Starts watching `hwnd`'s thread; `name` names the window in what is
// written. Null when the thread cannot be opened.
ThreadWatch* StartThreadWatch(void* hwnd, const char* name);
// Stops the watch (any thread but the window's is fine; waits for it).
void StopThreadWatch(ThreadWatch* watch);
// Stops the worker without freeing the state: a reentrant window callback may
// still hold it until its procedure returns. StopThreadWatch releases it later.
void RetireThreadWatch(ThreadWatch* watch);
// The window's procedure saw the watch's message (on the window's thread):
// true when `msg` was it.
bool ThreadWatchMessage(ThreadWatch* watch, uint32_t msg);
// The message the window's procedure is in (0 when none), for the report.
void ThreadWatchEnter(ThreadWatch* watch, uint32_t msg, uint32_t* saved);
void ThreadWatchLeave(ThreadWatch* watch, uint32_t saved);

// Explicit diagnostic observer: at most 40 threads, each with its native stack
// and wait chain. Call from a separate process, never the process being read.
// Synchronous Windows WCT may block: the caller must bound this observer's life.
void DumpProcessThreadWaits(uint32_t pid);

}  // namespace input
}  // namespace gesher
