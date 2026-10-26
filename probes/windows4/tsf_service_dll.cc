// Explicit fixture DLL: called on an app window's TSF thread, not registered
// as an installed IME. It uses the actual focused TSF context/edit interfaces.
#include "text_store.h"
#include <cstdio>
#include <string>
namespace {
thread_local int failures = 0;
void Check(bool okay, const char* message) {
  std::fprintf(stderr, "%s  windows4 TSF service: %s\n", okay ? "PASS" : "FAIL", message);
  std::fflush(stderr);
  if (!okay) ++failures;
}
}
#define GESHER_TSF_SERVICE_DLL 1
#include "tsf_service_fixture.h"
extern "C" __declspec(dllexport) HRESULT WINAPI gesher_test_reconversion(ITfThreadMgr* manager, TfClientId client) {
  failures = 0;
  auto* service = new ReconversionService();
  HRESULT status = service->Activate(manager, client);
  if (SUCCEEDED(status)) status = service->Reconvert();
  service->Release();
  return failures == 0 ? status : E_FAIL;
}
