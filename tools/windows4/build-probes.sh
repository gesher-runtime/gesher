#!/usr/bin/env bash
# Gesher-owned source only. Never fetches/configures/builds AOSP.
set -euo pipefail
task_toolchain=${1:?llvm-mingw directory required}
task_output=${2:-dist-windows4}
mkdir -p "$task_output"
task_compiler="$task_toolchain/bin/x86_64-w64-mingw32-clang++"
"$task_compiler" --version
task_flags=(-std=gnu++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wextra -Werror
  -mno-ms-bitfields -fno-exceptions -fsized-deallocation -static)
task_input=compat/input
task_window=compat/hwui/window
"$task_compiler" "${task_flags[@]}" -I"$task_input" -shared \
  probes/windows4/tsf_service_dll.cc -o "$task_output/windows4_tsf_service.dll" -lole32 -luuid
"$task_compiler" "${task_flags[@]}" -I"$task_input" \
  probes/windows4/text_store_probe.cc "$task_input/text_store.cc" "$task_input/text_service.cc" \
  -o "$task_output/windows4_text_store_probe.exe" -lole32 -luuid
"$task_compiler" "${task_flags[@]}" -I"$task_input" \
  probes/windows4/ime_tsf_probe.cc "$task_input/ime.cc" "$task_input/text_store.cc" \
  "$task_input/text_service.cc" "$task_input/thread_watch.cc" \
  -o "$task_output/windows4_ime_tsf_probe.exe" -lole32 -luuid -limm32
"$task_compiler" "${task_flags[@]}" -Iprobes/windows4/standalone -I"$task_window" \
  probes/windows4/input_pane_probe.cc "$task_window/gesher_window.cc" \
  "$task_window/gesher_popup.cc" "$task_window/input_pane.cc" \
  -o "$task_output/windows4_input_pane_probe.exe" -lole32 -luuid -lgdi32
