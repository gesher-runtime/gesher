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
    adapters=1;ie_size=2;clock=0;filetime=1'000'000'000'000ULL;callback=nullptr;context=nullptr;last_context=nullptr;armed.clear();wlan_broken=false;
}
void notify(int index,DWORD code=wlan_notification_acm_scan_complete) {
    if(!callback)return;
    WLAN_NOTIFICATION_DATA notification{}; notification.NotificationSource=WLAN_NOTIFICATION_SOURCE_ACM;
    notification.NotificationCode=code; notification.InterfaceGuid.Data1=index;
    callback(&notification,context);
}
bool guard(void*) { ++guards;if(notify_in_guard)notify(6);return eligible; }
Api api() {
    Api api;
    api.open=WlanOpenHandle;api.close=WlanCloseHandle;api.interfaces=WlanEnumInterfaces;
    api.query=WlanQueryInterface;api.bss=WlanGetNetworkBssList;api.notify=WlanRegisterNotification;
    api.scan=WlanScan;api.free=WlanFreeMemory;
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
    std::cout<<"WindowsWifi native boundary all passed: "<<checks<<"\n";
}
