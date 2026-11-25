// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Host-only Win32/WLAN call boundary. ABI/layout is separately compiled with the real SDK.
#pragma once
#include <cstddef>
#include <cstdint>
#define WINAPI
using DWORD=uint32_t; using ULONG=uint32_t; using LONG=int32_t;
using ULONGLONG=uint64_t; using BOOL=int; using HANDLE=void*; using HMODULE=void*;
using PVOID=void*; using BYTE=unsigned char; using WCHAR=wchar_t; using LPCWSTR=const wchar_t*;
constexpr DWORD WLAN_MAX_NAME_LENGTH=256, WLAN_PROFILE_USER=2;
constexpr DWORD WLAN_REASON_CODE_SUCCESS=0, WLAN_REASON_CODE_AC_CONNECT_BASE=0x28000, WLAN_REASON_CODE_MSM_CONNECT_BASE=0x38000;
constexpr DWORD WLAN_REASON_CODE_MSMSEC_BASE=0x40000, WLAN_REASON_CODE_MSMSEC_END=0x4ffff;
constexpr DWORD WLAN_REASON_CODE_NOT_VISIBLE=WLAN_REASON_CODE_AC_CONNECT_BASE+2, WLAN_REASON_CODE_NETWORK_NOT_AVAILABLE=WLAN_REASON_CODE_AC_CONNECT_BASE+11;
constexpr DWORD WLAN_REASON_CODE_ASSOCIATION_FAILURE=WLAN_REASON_CODE_MSM_CONNECT_BASE+2, WLAN_REASON_CODE_ASSOCIATION_TIMEOUT=WLAN_REASON_CODE_MSM_CONNECT_BASE+3;
constexpr DWORD WLAN_REASON_CODE_START_SECURITY_FAILURE=WLAN_REASON_CODE_MSM_CONNECT_BASE+5, WLAN_REASON_CODE_SECURITY_FAILURE=WLAN_REASON_CODE_MSM_CONNECT_BASE+6;
constexpr DWORD WLAN_REASON_CODE_SECURITY_TIMEOUT=WLAN_REASON_CODE_MSM_CONNECT_BASE+7;
constexpr unsigned char NDIS_OBJECT_TYPE_DEFAULT=0x80, DOT11_BSSID_LIST_REVISION_1=1;
inline void SecureZeroMemory(void* p, size_t n) { volatile unsigned char* b=static_cast<volatile unsigned char*>(p); while(n--) *b++=0; }
struct GUID { uint32_t Data1; uint16_t Data2,Data3; unsigned char Data4[8]; };
struct FILETIME { DWORD dwLowDateTime,dwHighDateTime; };
struct NET_LUID { uint64_t Value; }; using NET_IFINDEX=ULONG;
constexpr int FALSE=0, TRUE=1;
constexpr DWORD ERROR_SUCCESS=0, NO_ERROR=0, ERROR_ACCESS_DENIED=5, ERROR_NOT_FOUND=1168;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258;
constexpr DWORD LOAD_LIBRARY_SEARCH_SYSTEM32=0x800;
constexpr DWORD WLAN_NOTIFICATION_SOURCE_NONE=0, WLAN_NOTIFICATION_SOURCE_ACM=8, WLAN_NOTIFICATION_SOURCE_MSM=0x10;
constexpr DWORD wlan_notification_acm_scan_complete=7, wlan_notification_acm_scan_fail=8;
constexpr DWORD wlan_notification_acm_connection_complete=10, wlan_notification_acm_connection_attempt_fail=11;
constexpr DWORD wlan_notification_acm_interface_arrival=13, wlan_notification_acm_interface_removal=14;
constexpr DWORD wlan_notification_acm_disconnected=21;
constexpr DWORD wlan_notification_msm_connected=4, wlan_notification_msm_radio_state_change=7;
constexpr DWORD wlan_notification_msm_signal_quality_change=8, wlan_notification_msm_disconnected=10;
constexpr DWORD WLAN_MAX_PHY_INDEX=64;
enum WLAN_INTERFACE_STATE { wlan_interface_state_not_ready=0, wlan_interface_state_connected=1, wlan_interface_state_disconnected=4 };
enum WLAN_INTF_OPCODE { wlan_intf_opcode_radio_state=1, wlan_intf_opcode_current_connection=2, wlan_intf_opcode_rssi=3,
    wlan_intf_opcode_supported_infrastructure_auth_cipher_pairs=4 };
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
struct WLAN_CONNECTION_ATTRIBUTES { WLAN_INTERFACE_STATE isState; int wlanConnectionMode; WCHAR strProfileName[256]; WLAN_ASSOCIATION_ATTRIBUTES wlanAssociationAttributes; WLAN_SECURITY_ATTRIBUTES wlanSecurityAttributes; };
enum WLAN_CONNECTION_MODE { wlan_connection_mode_profile=0 };
struct WLAN_CONNECTION_NOTIFICATION_DATA { WLAN_CONNECTION_MODE wlanConnectionMode; WCHAR strProfileName[256]; DOT11_SSID dot11Ssid; int dot11BssType; BOOL bSecurityEnabled; DWORD wlanReasonCode; DWORD dwFlags; WCHAR strProfileXml[1]; };
struct NDIS_OBJECT_HEADER { unsigned char Type, Revision; uint16_t Size; };
struct DOT11_BSSID_LIST { NDIS_OBJECT_HEADER Header; ULONG uNumOfEntries,uTotalNumOfEntries; unsigned char BSSIDs[1][6]; };
struct WLAN_CONNECTION_PARAMETERS { WLAN_CONNECTION_MODE wlanConnectionMode; LPCWSTR strProfile; DOT11_SSID* pDot11Ssid; DOT11_BSSID_LIST* pDesiredBssidList; DOT11_BSS_TYPE dot11BssType; DWORD dwFlags; };
struct WLAN_PROFILE_INFO { WCHAR strProfileName[256]; DWORD dwFlags; };
struct WLAN_PROFILE_INFO_LIST { DWORD dwNumberOfItems,dwIndex; WLAN_PROFILE_INFO ProfileInfo[1]; };
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
struct DOT11_AUTH_CIPHER_PAIR { ULONG AuthAlgoId; ULONG CipherAlgoId; };
struct WLAN_AUTH_CIPHER_PAIR_LIST { DWORD dwNumberOfItems; DOT11_AUTH_CIPHER_PAIR pAuthCipherPairList[1]; };
struct WLAN_INTERFACE_CAPABILITY { ULONG interfaceType; BOOL bDot11DSupported; DWORD dwMaxDesiredSsidListSize,
    dwMaxDesiredBssidListSize,dwNumberOfSupportedPhys; ULONG dot11PhyTypes[64]; };
DWORD WlanOpenHandle(DWORD,PVOID,DWORD*,HANDLE*);
DWORD WlanCloseHandle(HANDLE,PVOID);
DWORD WlanEnumInterfaces(HANDLE,PVOID,WLAN_INTERFACE_INFO_LIST**);
DWORD WlanQueryInterface(HANDLE,const GUID*,WLAN_INTF_OPCODE,PVOID,DWORD*,PVOID*,WLAN_OPCODE_VALUE_TYPE*);
DWORD WlanGetNetworkBssList(HANDLE,const GUID*,const DOT11_SSID*,DOT11_BSS_TYPE,BOOL,PVOID,WLAN_BSS_LIST**);
DWORD WlanRegisterNotification(HANDLE,DWORD,BOOL,WLAN_NOTIFICATION_CALLBACK,PVOID,PVOID,DWORD*);
DWORD WlanScan(HANDLE,const GUID*,const DOT11_SSID*,const WLAN_RAW_DATA*,PVOID);
void WlanFreeMemory(PVOID);
DWORD WlanGetInterfaceCapability(HANDLE,const GUID*,PVOID,WLAN_INTERFACE_CAPABILITY**);
DWORD WlanSetProfile(HANDLE,const GUID*,DWORD,LPCWSTR,LPCWSTR,BOOL,PVOID,DWORD*);
DWORD WlanDeleteProfile(HANDLE,const GUID*,LPCWSTR,PVOID);
DWORD WlanConnect(HANDLE,const GUID*,const WLAN_CONNECTION_PARAMETERS*,PVOID);
DWORD WlanDisconnect(HANDLE,const GUID*,PVOID);
DWORD WlanGetProfileList(HANDLE,const GUID*,PVOID,WLAN_PROFILE_INFO_LIST**);
DWORD WlanSetProfilePosition(HANDLE,const GUID*,LPCWSTR,DWORD,PVOID);
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
