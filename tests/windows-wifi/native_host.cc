// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// The production adapter runs unchanged against this explicit Windows-call boundary.
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#ifndef GESHER_WIFI_SOURCE
#define GESHER_WIFI_SOURCE "../../compat/net/windows_wifi.cc"
#endif
#include GESHER_WIFI_SOURCE

namespace fixture {
int checks=0, opens=0, closes=0, frees=0, scans=0, events=0, guards=0;
bool open_failure=false, query_failure=false, bss_failure=false, malformed=false;
bool scan_failure=false, completion=true, precompletion=false, close_failure=false, radio_on=true;
bool eligible=true, revoke_on_open=false, notify_in_guard=false;
bool capability_failure=false, pairs_overflow=false, ihv_phy=false;
bool enum_failure=false; // the WLAN service behind an open handle went away
// Connection boundary: what Windows was asked, and the scripted notification it sends.
std::vector<std::wstring> stored_profiles, deleted_profiles, connected_profiles;
int disconnects=0; bool pinned=false, set_profile_failure=false, cancel_on_wait=false;
std::wstring positioned; DWORD position=0; bool position_failure=false;
DWORD delete_result=ERROR_SUCCESS; bool profiles_failure=false;
DWORD connect_reason=0; int connect_notifications=1; std::wstring notify_profile; bool foreign_first=false;
bool index_failure=false, delay_after_bss=false, delay_close=false;
int adapters=1; size_t ie_size=2;
uint64_t clock=0, filetime=1'000'000'000'000ULL;
WLAN_NOTIFICATION_CALLBACK callback=nullptr;
void* context=nullptr;
void* last_context=nullptr;
std::vector<int> armed;
void check(bool value,const char* reason) { if(!value) { std::cerr<<reason<<"\n"; std::abort(); } ++checks; }
void reset() {
    opens=closes=frees=scans=events=guards=0;
    open_failure=query_failure=bss_failure=malformed=scan_failure=precompletion=close_failure=revoke_on_open=false;
    completion=radio_on=eligible=true;notify_in_guard=false;
    index_failure=delay_after_bss=delay_close=false;
    capability_failure=pairs_overflow=ihv_phy=false;
    enum_failure=false;
    stored_profiles.clear();deleted_profiles.clear();connected_profiles.clear();disconnects=0;pinned=false;
    set_profile_failure=cancel_on_wait=false;delete_result=ERROR_SUCCESS;profiles_failure=false;positioned.clear();position=0;position_failure=false;connect_reason=0;connect_notifications=1;notify_profile.clear();foreign_first=false;
    adapters=1;ie_size=2;clock=0;filetime=1'000'000'000'000ULL;callback=nullptr;context=nullptr;last_context=nullptr;armed.clear();wlan_broken=false;
}
void notify(int index,DWORD code=wlan_notification_acm_scan_complete,DWORD source=WLAN_NOTIFICATION_SOURCE_ACM) {
    if(!callback)return;
    WLAN_NOTIFICATION_DATA notification{}; notification.NotificationSource=source;
    notification.NotificationCode=code; notification.InterfaceGuid.Data1=index;
    callback(&notification,context);
}
bool guard(void*) { ++guards;if(notify_in_guard)notify(6);return eligible; }
Api api() {
    Api api;
    api.open=WlanOpenHandle;api.close=WlanCloseHandle;api.interfaces=WlanEnumInterfaces;
    api.query=WlanQueryInterface;api.bss=WlanGetNetworkBssList;api.notify=WlanRegisterNotification;
    api.scan=WlanScan;api.free=WlanFreeMemory;api.capability=WlanGetInterfaceCapability;
    api.set_profile=WlanSetProfile;api.delete_profile=WlanDeleteProfile;api.connect=WlanConnect;
    api.disconnect=WlanDisconnect;api.profiles=WlanGetProfileList;api.position=WlanSetProfilePosition;
    return api;
}
}
HMODULE LoadLibraryExW(const wchar_t*,HANDLE,DWORD) {return nullptr;}
void* GetProcAddress(HMODULE,const char*) {return nullptr;}
HANDLE CreateEventW(PVOID,BOOL,BOOL,const wchar_t*) {++fixture::events;return new int(0);}
BOOL CloseHandle(HANDLE handle) {--fixture::events;delete static_cast<int*>(handle);return TRUE;}
BOOL SetEvent(HANDLE handle) {*static_cast<int*>(handle)=1;return TRUE;}
ULONGLONG GetTickCount64() {return fixture::clock;}
DWORD WaitForSingleObject(HANDLE handle,DWORD millis) {
    int* event=static_cast<int*>(handle);
    if(*event) {*event=0;return WAIT_OBJECT_0;}
    fixture::clock+=millis;return WAIT_TIMEOUT;
}
void GetSystemTimeAsFileTime(FILETIME* out) {out->dwHighDateTime=fixture::filetime>>32;out->dwLowDateTime=fixture::filetime;}
void QueryInterruptTimePrecise(ULONGLONG* out) {*out=(10'000+fixture::clock)*10'000;}
DWORD ConvertInterfaceGuidToLuid(const GUID* guid,NET_LUID* luid) {if(fixture::index_failure)return ERROR_ACCESS_DENIED;luid->Value=guid->Data1;return NO_ERROR;}
DWORD ConvertInterfaceLuidToIndex(const NET_LUID* luid,NET_IFINDEX* index) {*index=luid->Value;return NO_ERROR;}
DWORD WlanOpenHandle(DWORD,PVOID,DWORD* version,HANDLE* handle) {
    ++fixture::opens;
    if(fixture::revoke_on_open)fixture::eligible=false;
    if(fixture::open_failure)return ERROR_ACCESS_DENIED;
    *version=2;*handle=reinterpret_cast<HANDLE>(1);return ERROR_SUCCESS;
}
DWORD WlanCloseHandle(HANDLE,PVOID) {
    ++fixture::closes;
    if(fixture::delay_close) {fixture::clock+=2000;fixture::filetime+=20'000'000;}
    if(fixture::close_failure)return ERROR_ACCESS_DENIED;
    fixture::callback=nullptr;fixture::context=nullptr;return ERROR_SUCCESS;
}
DWORD WlanEnumInterfaces(HANDLE,PVOID,WLAN_INTERFACE_INFO_LIST** output) {
    if(fixture::enum_failure)return ERROR_ACCESS_DENIED;
    size_t size=offsetof(WLAN_INTERFACE_INFO_LIST,InterfaceInfo)+fixture::adapters*sizeof(WLAN_INTERFACE_INFO);
    auto* list=static_cast<WLAN_INTERFACE_INFO_LIST*>(calloc(1,size));
    list->dwNumberOfItems=fixture::adapters;
    for(int i=0;i<fixture::adapters;i++) {list->InterfaceInfo[i].InterfaceGuid.Data1=i+6;list->InterfaceInfo[i].isState=wlan_interface_state_disconnected;}
    *output=list;return ERROR_SUCCESS;
}
DWORD WlanQueryInterface(HANDLE,const GUID*,WLAN_INTF_OPCODE operation,PVOID,DWORD* size,PVOID* result,WLAN_OPCODE_VALUE_TYPE*) {
    if(fixture::query_failure)return ERROR_ACCESS_DENIED;
    if(operation==wlan_intf_opcode_radio_state) {
        *size=sizeof(WLAN_RADIO_STATE);auto* radio=static_cast<WLAN_RADIO_STATE*>(calloc(1,*size));
        radio->dwNumberOfPhys=1;
        radio->PhyRadioState[0].dot11SoftwareRadioState=fixture::radio_on?dot11_radio_state_on:dot11_radio_state_off;
        radio->PhyRadioState[0].dot11HardwareRadioState=dot11_radio_state_on;
        *result=radio;
    } else if(operation==wlan_intf_opcode_current_connection) {
        if(fixture::delay_after_bss) {fixture::clock+=2000;fixture::filetime+=20'000'000;}
        *size=sizeof(WLAN_CONNECTION_ATTRIBUTES);auto* connection=static_cast<WLAN_CONNECTION_ATTRIBUTES*>(calloc(1,*size));
        connection->isState=wlan_interface_state_connected;
        auto& association=connection->wlanAssociationAttributes;
        association.dot11Ssid.uSSIDLength=4;memcpy(association.dot11Ssid.ucSSID,"Net\0",4);
        unsigned char mac[]={0xaa,0xbb,0xcc,0x11,0x22,0x33};memcpy(association.dot11Bssid,mac,6);
        association.dot11PhyType=10;association.ulRxRate=866000;association.ulTxRate=720000;
        connection->wlanSecurityAttributes.bSecurityEnabled=TRUE;connection->wlanSecurityAttributes.dot11AuthAlgorithm=11;
        *result=connection;
    } else if(operation==wlan_intf_opcode_supported_infrastructure_auth_cipher_pairs) {
        // WPA2-PSK/CCMP, SAE/CCMP, OWE/CCMP and open/WEP (DOT11_CIPHER_ALGO_WEP).
        const DOT11_AUTH_CIPHER_PAIR pairs[]={{7,4},{9,4},{10,4},{1,0x101}};
        *size=offsetof(WLAN_AUTH_CIPHER_PAIR_LIST,pAuthCipherPairList)+sizeof(pairs);
        auto* list=static_cast<WLAN_AUTH_CIPHER_PAIR_LIST*>(calloc(1,*size));
        list->dwNumberOfItems=fixture::pairs_overflow?1000:4;memcpy(list->pAuthCipherPairList,pairs,sizeof(pairs));
        *result=list;
    } else {*size=sizeof(LONG);auto* rssi=static_cast<LONG*>(calloc(1,*size));*rssi=-58;*result=rssi;}
    return ERROR_SUCCESS;
}
DWORD WlanGetNetworkBssList(HANDLE,const GUID*,const DOT11_SSID*,DOT11_BSS_TYPE,BOOL,PVOID,WLAN_BSS_LIST** output) {
    if(fixture::bss_failure)return ERROR_ACCESS_DENIED;
    size_t base=offsetof(WLAN_BSS_LIST,wlanBssEntries), total=base+sizeof(WLAN_BSS_ENTRY)+fixture::ie_size;
    auto* list=static_cast<WLAN_BSS_LIST*>(calloc(1,total));list->dwTotalSize=total;list->dwNumberOfItems=1;
    auto& bss=list->wlanBssEntries[0];bss.dot11Ssid.uSSIDLength=3;memcpy(bss.dot11Ssid.ucSSID,"Net",3);
    unsigned char mac[]={0xaa,0xbb,0xcc,0x11,0x22,0x33};memcpy(bss.dot11Bssid,mac,6);
    bss.dot11BssPhyType=10;bss.lRssi=-60;bss.ullHostTimestamp=fixture::filetime-20'000'000;
    bss.usCapabilityInformation=1;bss.ulChCenterFrequency=5'180'000;
    bss.ulIeOffset=fixture::malformed?0xffffffff:sizeof(WLAN_BSS_ENTRY);bss.ulIeSize=fixture::ie_size;
    auto* bytes=reinterpret_cast<unsigned char*>(list)+base+sizeof(WLAN_BSS_ENTRY);bytes[0]=0;bytes[1]=0;
    *output=list;return ERROR_SUCCESS;
}
DWORD WlanRegisterNotification(HANDLE,DWORD source,BOOL,WLAN_NOTIFICATION_CALLBACK callback,PVOID context,PVOID,DWORD*) {
    if(source==WLAN_NOTIFICATION_SOURCE_NONE) {
        if(fixture::close_failure)return ERROR_ACCESS_DENIED;
        fixture::callback=nullptr;fixture::context=nullptr;
    } else {
        fixture::callback=callback;fixture::context=context;fixture::last_context=context;
        if(fixture::precompletion)for(int i=0;i<fixture::adapters;i++)fixture::notify(i+6);
    }
    return ERROR_SUCCESS;
}
DWORD WlanScan(HANDLE,const GUID* guid,const DOT11_SSID*,const WLAN_RAW_DATA*,PVOID) {
    ++fixture::scans;fixture::armed.push_back(guid->Data1);
    if(fixture::scan_failure)return ERROR_ACCESS_DENIED;
    if(fixture::completion)fixture::notify(guid->Data1);
    return ERROR_SUCCESS;
}
void WlanFreeMemory(PVOID value) {++fixture::frees;free(value);}
DWORD WlanGetInterfaceCapability(HANDLE,const GUID*,PVOID,WLAN_INTERFACE_CAPABILITY** output) {
    if(fixture::capability_failure)return ERROR_ACCESS_DENIED;
    auto* capability=static_cast<WLAN_INTERFACE_CAPABILITY*>(calloc(1,sizeof(WLAN_INTERFACE_CAPABILITY)));
    const ULONG phys[]={5,6,4,7,8,10}; // HR-DSSS, ERP, OFDM, HT, VHT, HE
    capability->dwNumberOfSupportedPhys=6;memcpy(capability->dot11PhyTypes,phys,sizeof(phys));
    if(fixture::ihv_phy) {capability->dot11PhyTypes[6]=0x80000001u;capability->dwNumberOfSupportedPhys=7;}
    *output=capability;return ERROR_SUCCESS;
}
DWORD WlanSetProfile(HANDLE,const GUID*,DWORD flags,LPCWSTR xml,LPCWSTR,BOOL overwrite,PVOID,DWORD* reason) {
    *reason=0;
    if(fixture::set_profile_failure || flags!=WLAN_PROFILE_USER || !overwrite)return ERROR_ACCESS_DENIED;
    std::wstring text(xml);size_t a=text.find(L"<name>"),b=text.find(L"</name>");
    fixture::stored_profiles.push_back(text.substr(a+6,b-a-6));return ERROR_SUCCESS;
}
DWORD WlanSetProfilePosition(HANDLE,const GUID*,LPCWSTR name,DWORD position,PVOID) {
    if(fixture::position_failure)return ERROR_ACCESS_DENIED;
    fixture::positioned=name;fixture::position=position;return ERROR_SUCCESS;
}
DWORD WlanDeleteProfile(HANDLE,const GUID*,LPCWSTR name,PVOID) {
    if(fixture::delete_result==ERROR_SUCCESS)fixture::deleted_profiles.push_back(name);
    return fixture::delete_result;
}
void connect_notify(const std::wstring& profile,DWORD code,DWORD reason) {
    if(!fixture::callback)return;
    WLAN_CONNECTION_NOTIFICATION_DATA data{};wcsncpy(data.strProfileName,profile.c_str(),255);data.wlanReasonCode=reason;
    WLAN_NOTIFICATION_DATA notification{};notification.NotificationSource=WLAN_NOTIFICATION_SOURCE_ACM;
    notification.NotificationCode=code;notification.InterfaceGuid.Data1=6;notification.dwDataSize=sizeof(data);notification.pData=&data;
    fixture::callback(&notification,fixture::context);
}
DWORD WlanConnect(HANDLE,const GUID* guid,const WLAN_CONNECTION_PARAMETERS* parameters,PVOID) {
    if(guid->Data1!=6)return ERROR_ACCESS_DENIED;
    fixture::connected_profiles.push_back(parameters->strProfile);
    fixture::pinned=parameters->pDesiredBssidList!=nullptr && parameters->pDesiredBssidList->uNumOfEntries==1
        && parameters->pDesiredBssidList->BSSIDs[0][0]==0xaa;
    std::wstring target=fixture::notify_profile.empty()?parameters->strProfile:fixture::notify_profile;
    if(fixture::foreign_first)connect_notify(L"Home",wlan_notification_acm_connection_complete,0);
    for(int i=0;i<fixture::connect_notifications;i++)
        connect_notify(target,fixture::connect_reason?wlan_notification_acm_connection_attempt_fail:wlan_notification_acm_connection_complete,fixture::connect_reason);
    return ERROR_SUCCESS;
}
DWORD WlanDisconnect(HANDLE,const GUID*,PVOID) {++fixture::disconnects;return ERROR_SUCCESS;}
DWORD WlanGetProfileList(HANDLE,const GUID*,PVOID,WLAN_PROFILE_INFO_LIST** output) {
    if(fixture::profiles_failure)return ERROR_ACCESS_DENIED;
    size_t size=offsetof(WLAN_PROFILE_INFO_LIST,ProfileInfo)+3*sizeof(WLAN_PROFILE_INFO);
    auto* list=static_cast<WLAN_PROFILE_INFO_LIST*>(calloc(1,size));list->dwNumberOfItems=3;
    wcscpy(list->ProfileInfo[0].strProfileName,L"Gesher-r-1");list->ProfileInfo[0].dwFlags=WLAN_PROFILE_USER;
    wcscpy(list->ProfileInfo[1].strProfileName,L"Gesher-s-2");list->ProfileInfo[1].dwFlags=0; // all-user: never ours
    wcscpy(list->ProfileInfo[2].strProfileName,L"Home");list->ProfileInfo[2].dwFlags=WLAN_PROFILE_USER;
    *output=list;return ERROR_SUCCESS;
}
namespace fixture {
bool connect_active(void*) {return !cancel_on_wait;}
std::vector<uint32_t> deliveries;
int stop_after=3;
void (*between)(int)=nullptr;
bool sink(void*,uint32_t bits) {
    deliveries.push_back(bits);
    if(between)between(static_cast<int>(deliveries.size()));
    return static_cast<int>(deliveries.size())<stop_after;
}
void watch_events(int delivered) {
    if(delivered==1) {notify(6,wlan_notification_msm_radio_state_change,WLAN_NOTIFICATION_SOURCE_MSM);
        notify(6,wlan_notification_acm_scan_complete);}
    if(delivered==2) notify(6,wlan_notification_msm_signal_quality_change,WLAN_NOTIFICATION_SOURCE_MSM);
}
}

int main() {
    using namespace fixture;
    reset();Api backend=api();
    Api unavailable;
    check(Snapshot(unavailable,false,guard,nullptr).empty() && opens==0,
            "Missing WLAN library or exports stays unknown instead of disabled");
    std::string output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t1\n")==0 && scans==1 && closes==1 && events==0,"Actual scan completion publishes once and releases resources");
    check(output.find("if\t6\t1\t1\t4e657400\taa:bb:cc:11:22:33\t5180\t-58\t866000\t720000\t10\t11\t1")!=std::string::npos,"Native SSID bytes, exact interface, rates, frequency and enterprise auth retained");
    check(output.find("\t8000000\t10\t1\t0000\n")!=std::string::npos,"Host FILETIME timestamp converts to elapsed observation time");
    reset();delay_after_bss=true;delay_close=true;
    output=Snapshot(backend,false,guard,nullptr);
    check(fixture::clock==4000 && output.find("\t8000000\t10\t1\t0000\n")!=std::string::npos,"Slow post-BSS RPC and close cannot make observations newer");
    reset();index_failure=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t1\n")==0 && output.find("bss\t4e6574")!=std::string::npos
            && output.find("radio\t1\n")!=std::string::npos && output.find("if\t")==std::string::npos,
            "IP index mapping failure retains real WLAN scan and radio observations");
    reset();adapters=2;precompletion=true;completion=false;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t0\n")==0 && scans==2,"Pre-request notifications cannot complete a subsequent scan");
    check(fixture::clock==5000 && events==0,"Missing completion waits bounded deadline then cleans event");
    reset();completion=false;notify_in_guard=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t0\n")==0 && fixture::clock==5000,"Completion during permission lookup cannot precede scan initiation");
    reset();scan_failure=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t2\n")==0 && output.find("bss\t")==std::string::npos,"Rejected scan never republishes stale results as updated");
    reset();bss_failure=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t2\n")==0 && output.find("if\t6")!=std::string::npos,"Location-denied BSS list preserves only available interface state");
    reset();malformed=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(output.find("ok\t0\n")==0 && events==0,"Malformed native IE offset fails closed without buffer read");
    reset();adapters=64;ie_size=65535;
    output=Snapshot(backend,false,guard,nullptr);
    check(output.find("ok\t0\n")==0 && output.size()<=4*1024*1024,"Aggregate multi-adapter output bound rejects incomplete scan set before large concatenation");
    reset();radio_on=false;
    output=Snapshot(backend,true,guard,nullptr);
    check(scans==0 && output.find("ok\t0\n")==0,"No enabled PHY means no scan accepted");
    reset();revoke_on_open=true;
    output=Snapshot(backend,true,guard,nullptr);
    check(scans==0 && guards==1 && output.find("ok\t0\n")==0,"Permission revoked during native initialization prevents WlanScan");
    reset();adapters=0;
    output=Snapshot(backend,false,guard,nullptr);
    check(output=="ok\t1\n" && closes==1 && scans==0,"No hardware is real empty data, no fake scan");
    reset();open_failure=true;
    check(Snapshot(backend,false,guard,nullptr).empty() && closes==0,"Open failure distinct from no hardware and no bogus close");
    reset();query_failure=true;
    output=Snapshot(backend,false,guard,nullptr);
    check(output.find("if\t6\t-1\t0\t\t\t-1\t-127")!=std::string::npos,"Unreadable radio/connection remains unknown");
    reset();close_failure=true;
    Snapshot(backend,true,guard,nullptr);
    check(wlan_broken && events==1,"Close failure keeps callback context alive and bounds retention");
    int before=opens;
    check(Snapshot(backend,true,guard,nullptr).empty() && opens==before,"Teardown failure blocks further native allocations");
    notify(6); // Sanitizers would detect a stack/freed callback target here.
    check(events==1,"Late callback after failed close still has a live target");
    callback=nullptr;context=nullptr;delete static_cast<ScanWait*>(last_context);last_context=nullptr;
    check(events==0,"Fixture releases intentionally retained failed-close context after callbacks stopped");
    // Adapter census: PHY, auth and cipher unions; failures never invent support.
    reset();
    output=Hardware(backend);
    check(output=="hw\t1\ncap\t"+std::to_string((1u<<5)|(1u<<6)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<10))
            +"\t"+std::to_string((1u<<7)|(1u<<9)|(1u<<10)|(1u<<1))+"\t"+std::to_string((1u<<4)|(1u<<31))+"\n"
            && closes==1,"Census reports real PHY types and auth/cipher pairs");
    reset();capability_failure=true;pairs_overflow=true;ihv_phy=false;
    output=Hardware(backend);
    check(output=="hw\t1\ncap\t0\t0\t0\n","Unreadable capability and oversized pair count report no support");
    reset();ihv_phy=true;
    output=Hardware(backend);
    check(output.find("cap\t"+std::to_string((1u<<5)|(1u<<6)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<10))+"\t")!=std::string::npos,
            "IHV-defined PHY types are not standards");
    reset();adapters=0;
    check(Hardware(backend)=="hw\t0\n","No adapter is a real zero count");
    reset();open_failure=true;
    check(Hardware(backend).empty() && closes==0,"Stopped WLAN service is not hardware");
    check(Hardware(unavailable).empty() && opens==1,"Missing wlanapi is not hardware");
    // Notification watcher: callback bits are drained by the calling thread only.
    reset();deliveries.clear();stop_after=3;between=watch_events;
    check(Watch(backend,sink,nullptr,1000)==0,"Watcher stops cleanly when the sink declines");
    check(deliveries.size()==3 && deliveries[0]==15,"First delivery after registration requests a full refresh");
    check(deliveries[1]==(1|2|4),"Radio change and scan completion delivered as state, connection and scan");
    check(deliveries[2]==0,"Signal-quality noise is not an event");
    check(callback==nullptr && closes==1 && events==0,"Watcher unregisters, closes and frees its event");
    reset();deliveries.clear();open_failure=true;
    check(Watch(backend,sink,nullptr,1000)==-1 && deliveries.empty() && closes==0 && events==0,
            "Unavailable WLAN service is reported for retry without deliveries");
    reset();deliveries.clear();stop_after=1;between=nullptr;close_failure=true;
    check(Watch(backend,sink,nullptr,1000)==-1 && wlan_broken && events==1,
            "Failed close retains the watcher target and disables WLAN use");
    callback=nullptr;context=nullptr;delete static_cast<WatchState*>(last_context);last_context=nullptr;
    check(events==0,"Fixture releases the deliberately retained watcher state");
    reset();deliveries.clear();stop_after=100;between=[](int delivered){if(delivered==2)fixture::enum_failure=true;};
    check(Watch(backend,sink,nullptr,1000)==-1 && deliveries.size()==2 && callback==nullptr && closes==1 && events==0
            && !wlan_broken,"A handle whose WLAN service went away ends the watch for a reopen, releasing everything");
    between=nullptr;
    // User-approved connections: only this attempt's completion is an outcome.
    const unsigned char ap[6]={0xaa,0xbb,0xcc,0x11,0x22,0x33};
    reset();
    check(Connect(backend,6,L"<WLANProfile><name>Gesher-r-a</name></WLANProfile>",L"Gesher-r-a",ap,connect_active,nullptr,30000)==kConnected
            && stored_profiles.size()==1 && stored_profiles[0]==L"Gesher-r-a" && pinned && callback==nullptr && closes==1 && events==0,
            "Per-user profile, pinned BSSID, completion of this profile, notifications released");
    reset();foreign_first=true;connect_notifications=0;
    check(Connect(backend,6,L"<name>Gesher-r-b</name>",L"Gesher-r-b",nullptr,connect_active,nullptr,3000)==kFailNoResponse
            && fixture::clock>=3000 && !pinned,"Another profile's completion is not this attempt's success");
    reset();connect_reason=WLAN_REASON_CODE_SECURITY_FAILURE;
    check(Connect(backend,6,L"<name>Gesher-r-c</name>",L"Gesher-r-c",nullptr,connect_active,nullptr,3000)==kFailAuthentication,
            "Security failure is AUTHENTICATION");
    reset();connect_reason=WLAN_REASON_CODE_MSMSEC_BASE+5;
    check(Connect(backend,6,L"<name>Gesher-r-d</name>",L"Gesher-r-d",nullptr,connect_active,nullptr,3000)==kFailAuthentication,
            "MSM security reason range is AUTHENTICATION");
    reset();connect_reason=WLAN_REASON_CODE_ASSOCIATION_TIMEOUT;
    check(Connect(backend,6,L"<name>Gesher-r-e</name>",L"Gesher-r-e",nullptr,connect_active,nullptr,3000)==kFailAssociation,
            "Association timeout is ASSOCIATION");
    reset();connect_reason=WLAN_REASON_CODE_NETWORK_NOT_AVAILABLE;
    check(Connect(backend,6,L"<name>Gesher-r-f</name>",L"Gesher-r-f",nullptr,connect_active,nullptr,3000)==kFailNotFound,
            "Network not available is NOT_FOUND");
    reset();connect_reason=0x12345;
    check(Connect(backend,6,L"<name>Gesher-r-g</name>",L"Gesher-r-g",nullptr,connect_active,nullptr,3000)==kFailUnknown,
            "Other reasons are UNKNOWN");
    reset();connect_notifications=0;cancel_on_wait=true;
    check(Connect(backend,6,L"<name>Gesher-r-h</name>",L"Gesher-r-h",nullptr,connect_active,nullptr,3000)==kFailUnknown
            && connected_profiles.empty() && stored_profiles.empty(),"A requester gone before setup never changes Windows");
    reset();set_profile_failure=true;
    check(Connect(backend,6,L"<name>Gesher-r-i</name>",L"Gesher-r-i",nullptr,connect_active,nullptr,3000)==kFailUnknown
            && connected_profiles.empty(),"A rejected profile is never connected");
    reset();
    check(Connect(backend,99,L"<name>Gesher-r-j</name>",L"Gesher-r-j",nullptr,connect_active,nullptr,3000)==kFailUnknown
            && stored_profiles.empty(),"An unknown interface is not substituted");
    check(Connect(unavailable,6,L"<name>Gesher-r-k</name>",L"Gesher-r-k",nullptr,connect_active,nullptr,3000)==kFailUnknown,
            "No WLAN API cannot connect");
    reset();
    check(Disconnect(backend,6,L"Gesher-r-a",L"Home") && disconnects==1 && deleted_profiles.size()==1
            && deleted_profiles[0]==L"Gesher-r-a" && connected_profiles.size()==1 && connected_profiles[0]==L"Home",
            "Teardown disconnects, deletes the temporary profile and restores the previous one");
    reset();
    check(Disconnect(backend,6,L"",L"") && deleted_profiles.empty() && connected_profiles.empty(),"Plain disconnect");
    reset();
    check(StoreProfile(backend,6,L"<name>Gesher-s-1</name>",L"Gesher-s-1") && stored_profiles.size()==1
            && positioned==L"Gesher-s-1" && position==2
            && StoreProfile(backend,6,nullptr,L"Gesher-s-1") && deleted_profiles.size()==1,
            "Suggestion profile is stored below the user's profiles, and deleted");
    reset();position_failure=true;
    check(!StoreProfile(backend,6,L"<name>Gesher-s-1</name>",L"Gesher-s-1") && deleted_profiles.size()==1
            && deleted_profiles[0]==L"Gesher-s-1","A suggestion Windows cannot rank below the user's networks is removed");
    reset();delete_result=ERROR_NOT_FOUND;
    check(StoreProfile(backend,6,nullptr,L"Gesher-s-1"),"A profile already gone from the interface counts as deleted");
    reset();delete_result=ERROR_ACCESS_DENIED;
    check(!StoreProfile(backend,6,nullptr,L"Gesher-s-1"),"A refused deletion is a failure");
    reset();
    check(!StoreProfile(backend,99,nullptr,L"Gesher-s-1") && deleted_profiles.empty(),
            "No deletion on an absent interface");
    reset();
    std::wstring names=L"stale";
    check(Profiles(backend,6,L"Gesher-",&names) && names==L"Gesher-r-1\n","Only per-user Gesher profiles are listed for cleanup");
    check(Profiles(backend,6,L"Gesher-s-9",&names) && names.empty(),"A read list without the profile is empty");
    // A list that cannot be read is not an empty one: it never shows a profile as absent.
    check(!Profiles(backend,99,L"Gesher-",&names) && names.empty(),"An absent interface has no readable list");
    profiles_failure=true;
    check(!Profiles(backend,6,L"Gesher-",&names),"A failed profile list is a failure");
    reset();enum_failure=true;
    check(!Profiles(backend,6,L"Gesher-",&names),"A WLAN service that went away has no readable list");
    reset();
    check(!Profiles(unavailable,6,L"Gesher-",&names),"No WLAN API has no readable list");
    reset();
    check(CurrentProfile(backend,6).empty() || CurrentProfile(backend,6)==L"","Connected fixture interface without a profile name");
    std::cout<<"WindowsWifi native boundary all passed: "<<checks<<"\n";
}
