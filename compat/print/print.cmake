# Gesher (print, docs/print.md): Windows printers for the system server's print
# service. com.gesher.server.WindowsPrint's natives join libandroid_runtime's
# builtin library (android_runtime.cc calls register_gesher_print). winspool,
# comdlg32 (PrintDlgEx) and GDI print; Windows.Data.Pdf renders the app's PDF,
# reached through combase's WinRT functions found at run time (compat/devices/winrt.cc).
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/print/print_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/print/print_win32.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/print)
target_link_libraries(framework_runtime PUBLIC winspool comdlg32 gdi32 user32 ole32)
