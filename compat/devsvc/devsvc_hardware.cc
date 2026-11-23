// Gesher: the Windows hardware behind PackageManager's system features
// (docs/system-features.md): a touch digitizer's contacts, audio render endpoints
// (speakers and output jacks) and wired Ethernet interfaces. See devsvc_win32.h.

#include "devsvc_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <mmdeviceapi.h>

namespace gesher {
namespace devsvc {
namespace {

// The GUIDs, spelled out (not all are in every libuuid), as the audio sink does.
const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C,
                                        {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35,
                                     {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

}  // namespace

int TouchContacts() {
  const int digitizer = GetSystemMetrics(SM_DIGITIZER);
  if ((digitizer & NID_READY) == 0) return 0;
  if ((digitizer & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) == 0) return 0;
  const int contacts = GetSystemMetrics(SM_MAXIMUMTOUCHES);
  return contacts > 0 ? contacts : 0;
}

int AudioRenderEndpoints() {
  // The calling thread may already be in an apartment (RPC_E_CHANGED_MODE): it can
  // still call. Only a successful initialization here is balanced.
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(init) && init != RPC_E_CHANGED_MODE) return -1;
  int count = -1;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                 kIidIMMDeviceEnumerator,
                                 reinterpret_cast<void**>(&enumerator)))) {
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &devices))) {
      UINT n = 0;
      if (SUCCEEDED(devices->GetCount(&n))) count = static_cast<int>(n);
      devices->Release();
    }
    enumerator->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return count;
}

int EthernetInterfaces() {
  MIB_IF_TABLE2* table = nullptr;
  if (GetIfTable2(&table) != NO_ERROR || table == nullptr) return -1;
  int count = 0;
  for (ULONG i = 0; i < table->NumEntries; i++) {
    const MIB_IF_ROW2& row = table->Table[i];
    if (row.Type == IF_TYPE_ETHERNET_CSMACD &&
        row.PhysicalMediumType == NdisPhysicalMedium802_3 &&
        row.InterfaceAndOperStatusFlags.HardwareInterface) {
      count++;
    }
  }
  FreeMibTable(table);
  return count;
}

}  // namespace devsvc
}  // namespace gesher
