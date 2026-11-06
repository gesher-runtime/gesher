/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.le.*;
import android.os.Parcel;
import android.os.ParcelUuid;
import android.os.Parcelable;

/** BLE parsing/filtering/builders are useful even when no adapter is present. */
public final class BluetoothValueContractTest {
    private static int checks;
    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        checks++;
    }
    private static void rejects(Runnable action, String name) {
        try { action.run(); } catch (IllegalArgumentException | IllegalStateException expected) { checks++; return; }
        throw new AssertionError(name);
    }
    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel p = Parcel.obtain();
        try {
            value.writeToParcel(p, 0); p.setDataPosition(0);
            T result = creator.createFromParcel(p);
            check(p.dataAvail() == 0, "complete parcel consumed");
            return result;
        } finally { p.recycle(); }
    }
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        check(BluetoothAdapter.getDefaultAdapter() == null, "adapter remains unavailable");
        ScanSettings defaults = new ScanSettings.Builder().build();
        check(defaults.getScanMode() == ScanSettings.SCAN_MODE_LOW_POWER && defaults.getReportDelayMillis() == 0, "scan defaults");
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setReportDelay(123).setLegacy(false).setPhy(ScanSettings.PHY_LE_ALL_SUPPORTED).build();
        ScanSettings copy = roundTrip(settings, ScanSettings.CREATOR);
        check(copy.getScanMode() == settings.getScanMode() && copy.getReportDelayMillis() == 123
                && !copy.getLegacy() && copy.getPhy() == settings.getPhy(), "scan settings parcel");
        rejects(() -> new ScanSettings.Builder().setScanMode(99), "invalid scan mode");
        rejects(() -> new ScanSettings.Builder().setReportDelay(-1), "negative scan delay");
        rejects(() -> new ScanSettings.Builder().setCallbackType(0), "invalid callback type");
        ParcelUuid heartRate = ParcelUuid.fromString("0000180d-0000-1000-8000-00805f9b34fb");
        byte[] packet = {2,1,6, 3,3,0x0d,0x18, 4,9,'B','L','E', 5,(byte)0xff,0x34,0x12,0x55,0x66};
        ScanRecord record = ScanRecord.parseFromBytes(packet);
        ScanResult scan = new ScanResult(null, record, -40, 123456);
        ScanFilter filter = new ScanFilter.Builder().setDeviceName("BLE").setServiceUuid(heartRate)
                .setManufacturerData(0x1234, new byte[]{0x50,0x60}, new byte[]{(byte)0xf0,(byte)0xf0}).build();
        check(filter.matches(scan), "name UUID and masked manufacturer match");
        check(!filter.matches(null), "null scan mismatch");
        check(!new ScanFilter.Builder().setDeviceName("other").build().matches(scan), "name mismatch");
        check(!new ScanFilter.Builder().setManufacturerData(0x1234, new byte[]{0x54}).build().matches(scan), "manufacturer mismatch");
        check(filter.equals(roundTrip(filter, ScanFilter.CREATOR)), "filter parcel equality");
        rejects(() -> new ScanFilter.Builder().setManufacturerData(1, new byte[]{1}, new byte[]{1,2}), "mask length mismatch");
        rejects(() -> new ScanFilter.Builder().setDeviceAddress("aa:bb:cc:dd:ee:ff"), "lowercase address rejected");
        AdvertiseData data = new AdvertiseData.Builder().addServiceUuid(heartRate)
                .addServiceData(heartRate, new byte[]{1,2}).addManufacturerData(0x1234, new byte[]{3,4})
                .setIncludeDeviceName(true).setIncludeTxPowerLevel(true).build();
        check(data.equals(roundTrip(data, AdvertiseData.CREATOR)), "advertise payload parcel");
        rejects(() -> new AdvertiseData.Builder().addManufacturerData(-1, new byte[]{1}), "negative manufacturer");
        AdvertiseSettings ads = new AdvertiseSettings.Builder().setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH).setConnectable(false).setTimeout(99).build();
        AdvertiseSettings adsCopy = roundTrip(ads, AdvertiseSettings.CREATOR);
        check(adsCopy.getTimeout() == 99 && !adsCopy.isConnectable() && adsCopy.getMode() == ads.getMode(), "advertise settings parcel");
        rejects(() -> new AdvertiseSettings.Builder().setTimeout(180001), "advertise timeout upper bound");
        AdvertisingSetParameters extended = new AdvertisingSetParameters.Builder().setLegacyMode(false)
                .setScannable(true).setInterval(AdvertisingSetParameters.INTERVAL_LOW).build();
        check(roundTrip(extended, AdvertisingSetParameters.CREATOR).isScannable(), "extended advertising parcel");
        rejects(() -> new AdvertisingSetParameters.Builder().setConnectable(true).setScannable(true).build(), "extended connectable-scannable conflict");
        rejects(() -> new AdvertisingSetParameters.Builder().setLegacyMode(true).setAnonymous(true).build(), "legacy anonymous conflict");
        PeriodicAdvertisingParameters periodic = new PeriodicAdvertisingParameters.Builder().setIncludeTxPower(true).setInterval(100).build();
        check(roundTrip(periodic, PeriodicAdvertisingParameters.CREATOR).getInterval() == 100, "periodic parameters parcel");
        rejects(() -> new PeriodicAdvertisingParameters.Builder().setInterval(1), "periodic interval lower bound");
        PeriodicAdvertisingReport report = new PeriodicAdvertisingReport(2, 3, -55, 0, record);
        PeriodicAdvertisingReport reportCopy = roundTrip(report, PeriodicAdvertisingReport.CREATOR);
        check(reportCopy.getSyncHandle() == 2 && reportCopy.getRssi() == -55
                && java.util.Arrays.equals(packet, reportCopy.getData().getBytes()), "periodic report parcel");
        check(BluetoothAdapter.getDefaultAdapter() == null, "builders do not create adapter");
        System.out.println("BluetoothValueContractTest all passed: " + checks);
    }
}
