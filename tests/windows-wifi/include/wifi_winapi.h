// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Host-only Win32/WLAN call boundary. ABI/layout is separately compiled with the real SDK.
#pragma once
#include <cstddef>
#include <cstdint>
#define WINAPI
using DWORD=uint32_t; using ULONG=uint32_t; using LONG=int32_t;
using ULONGLONG=uint64_t; using BOOL=int; using HANDLE=void*; using HMODULE=void*;
using PVOID=void*; using BYTE=unsigned char;
struct GUID { uint32_t Data1; uint16_t Data2,Data3; unsigned char Data4[8]; };
struct FILETIME { DWORD dwLowDateTime,dwHighDateTime; };
struct NET_LUID { uint64_t Value; }; using NET_IFINDEX=ULONG;
constexpr int FALSE=0, TRUE=1;
constexpr DWORD ERROR_SUCCESS=0, NO_ERROR=0, ERROR_ACCESS_DENIED=5;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258;
constexpr DWORD LOAD_LIBRARY_SEARCH_SYSTEM32=0x800;
constexpr DWORD WLAN_NOTIFICATION_SOURCE_NONE=0, WLAN_NOTIFICATION_SOURCE_ACM=8;
constexpr DWORD wlan_notification_acm_scan_complete=7, wlan_notification_acm_scan_fail=8;
constexpr DWORD WLAN_MAX_PHY_INDEX=64;
enum WLAN_INTERFACE_STATE { wlan_interface_state_not_ready=0, wlan_interface_state_connected=1, wlan_interface_state_disconnected=4 };
enum WLAN_INTF_OPCODE { wlan_intf_opcode_radio_state=1, wlan_intf_opcode_current_connection=2, wlan_intf_opcode_rssi=3 };
enum WLAN_OPCODE_VALUE_TYPE { wlan_opcode_value_type_invalid };
enum DOT11_BSS_TYPE { dot11_BSS_type_infrastructure=1, dot11_BSS_type_any=3 };
enum DOT11_RADIO_STATE { dot11_radio_state_unknown=0, dot11_radio_state_on=1, dot11_radio_state_off=2 };
struct DOT11_SSID { ULONG uSSIDLength; unsigned char ucSSID[32]; };
struct WLAN_INTERFACE_INFO { GUID InterfaceGuid; wchar_t strInterfaceDescription[256]; WLAN_INTERFACE_STATE isState; };
struct WLAN_INTERFACE_INFO_LIST { DWORD dwNumberOfItems,dwIndex; WLAN_INTERFACE_INFO InterfaceInfo[1]; };
struct WLAN_PHY_RADIO_STATE { DWORD dwPhyIndex; DOT11_RADIO_STATE dot11SoftwareRadioState,dot11HardwareRadioState; };
struct WLAN_RADIO_STATE { DWORD dwNumberOfPhys; WLAN_PHY_RADIO_STATE PhyRadioState[64]; };
struct WLAN_ASSOCIATION_ATTRIBUTES { DOT11_SSID dot11Ssid; unsigned char dot11Bssid[6]; ULONG dot11PhyType,ulRxRate,ulTxRate; };
struct WLAN_SECURITY_ATTRIBUTES { BOOL bSecurityEnabled; ULONG dot11AuthAlgorithm; };
struct WLAN_CONNECTION_ATTRIBUTES { WLAN_INTERFACE_STATE isState; WLAN_ASSOCIATION_ATTRIBUTES wlanAssociationAttributes; WLAN_SECURITY_ATTRIBUTES wlanSecurityAttributes; };
struct WLAN_BSS_ENTRY {
    DOT11_SSID dot11Ssid;
    unsigned char dot11Bssid[6];
    ULONG dot11BssPhyType;
    LONG lRssi;
    ULONGLONG ullHostTimestamp;
    uint16_t usCapabilityInformation;
    ULONG ulChCenterFrequency,ulIeOffset,ulIeSize;
};
struct WLAN_BSS_LIST { DWORD dwTotalSize,dwNumberOfItems; WLAN_BSS_ENTRY wlanBssEntries[1]; };
struct WLAN_NOTIFICATION_DATA { DWORD NotificationSource,NotificationCode; GUID InterfaceGuid; DWORD dwDataSize; PVOID pData; };
using WLAN_NOTIFICATION_CALLBACK=void (*)(WLAN_NOTIFICATION_DATA*,PVOID);
struct WLAN_RAW_DATA { DWORD dwDataSize; BYTE DataBlob[1]; };
DWORD WlanOpenHandle(DWORD,PVOID,DWORD*,HANDLE*);
DWORD WlanCloseHandle(HANDLE,PVOID);
DWORD WlanEnumInterfaces(HANDLE,PVOID,WLAN_INTERFACE_INFO_LIST**);
DWORD WlanQueryInterface(HANDLE,const GUID*,WLAN_INTF_OPCODE,PVOID,DWORD*,PVOID*,WLAN_OPCODE_VALUE_TYPE*);
DWORD WlanGetNetworkBssList(HANDLE,const GUID*,const DOT11_SSID*,DOT11_BSS_TYPE,BOOL,PVOID,WLAN_BSS_LIST**);
DWORD WlanRegisterNotification(HANDLE,DWORD,BOOL,WLAN_NOTIFICATION_CALLBACK,PVOID,PVOID,DWORD*);
DWORD WlanScan(HANDLE,const GUID*,const DOT11_SSID*,const WLAN_RAW_DATA*,PVOID);
void WlanFreeMemory(PVOID);
HMODULE LoadLibraryExW(const wchar_t*,HANDLE,DWORD);
void* GetProcAddress(HMODULE,const char*);
HANDLE CreateEventW(PVOID,BOOL,BOOL,const wchar_t*);
BOOL CloseHandle(HANDLE);
BOOL SetEvent(HANDLE);
ULONGLONG GetTickCount64();
DWORD WaitForSingleObject(HANDLE,DWORD);
void GetSystemTimeAsFileTime(FILETIME*);
void QueryInterruptTimePrecise(ULONGLONG*);
DWORD ConvertInterfaceGuidToLuid(const GUID*,NET_LUID*);
DWORD ConvertInterfaceLuidToIndex(const NET_LUID*,NET_IFINDEX*);
