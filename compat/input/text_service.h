// Window-thread TSF manager/document ownership. The Java/ART boundary only
// supplies portable snapshots and consumes edit transactions.
#pragma once
#include "text_store.h"
#include <memory>
namespace gesher::input {
class TextService {
 public:
  static std::shared_ptr<TextService> Attach(HWND window, TextDocument initial,
      TextStore::Emit emit, TextStore::Schedule schedule, HRESULT* result);
  ~TextService();
  void Update(TextDocument document);
  void Caret(const RECT& screen);
  void Pump();
  bool Key(UINT message, WPARAM key, LPARAM flags);
  void Focus(bool focused);
  // Only called by the explicit native fixture entry point. The COM DLL is
  // retained until the thread manager is deactivated, after all its callbacks.
  HRESULT RunTestService(const wchar_t* path);
 private:
  TextService() = default;
  HWND window_ = nullptr;
  ITfThreadMgr* manager_ = nullptr;
  ITfDocumentMgr* document_ = nullptr;
  ITfDocumentMgr* previous_ = nullptr;
  ITfContext* context_ = nullptr;
  ITfKeystrokeMgr* keys_ = nullptr;
  TextStore* store_ = nullptr;
  TfClientId client_ = 0;
  bool activated_ = false, associated_ = false;
  HMODULE test_service_ = nullptr;
};
}  // namespace gesher::input
