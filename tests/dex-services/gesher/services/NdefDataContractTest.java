/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContextWrapper;
import android.net.Uri;
import android.nfc.FormatException;
import android.nfc.NdefMessage;
import android.nfc.NdefRecord;
import android.nfc.NfcAdapter;
import android.nfc.NfcEvent;
import android.nfc.Tag;
import android.nfc.cardemulation.CardEmulation;
import android.os.Parcel;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Public NDEF data contracts; no NFC adapter, tag or NFC service is required. */
public final class NdefDataContractTest {
    private static int checks;
    private interface Operation { void run() throws Exception; }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Throwable> type, Operation operation,
            String message) throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(message, failure);
            checks++;
            return;
        }
        throw new AssertionError(message);
    }

    private static byte[] ascii(String value) { return value.getBytes(StandardCharsets.US_ASCII); }

    private static byte[] hex(String text) {
        String value = text.replace(" ", "");
        byte[] bytes = new byte[value.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte)Integer.parseInt(value.substring(i * 2, i * 2 + 2), 16);
        }
        return bytes;
    }

    private static void sameRecord(NdefRecord expected, NdefRecord actual) {
        check(expected.getTnf() == actual.getTnf(), "Record TNF round trip");
        check(Arrays.equals(expected.getType(), actual.getType()), "Record type round trip");
        check(Arrays.equals(expected.getId(), actual.getId()), "Record ID round trip");
        check(Arrays.equals(expected.getPayload(), actual.getPayload()), "Record payload round trip");
        check(expected.equals(actual) && expected.hashCode() == actual.hashCode(),
                "Record equality/hash contract");
    }

    private static NdefMessage roundTrip(NdefMessage message) throws Exception {
        byte[] wire = message.toByteArray();
        check(wire.length == message.getByteArrayLength(), "Encoded byte length");
        NdefMessage parsed = new NdefMessage(wire);
        check(parsed.getRecords().length == message.getRecords().length, "Record count round trip");
        for (int i = 0; i < message.getRecords().length; i++) {
            sameRecord(message.getRecords()[i], parsed.getRecords()[i]);
        }
        check(message.equals(parsed) && message.hashCode() == parsed.hashCode(),
                "Message equality/hash contract");
        check(Arrays.equals(wire, parsed.toByteArray()), "Canonical byte round trip");
        return parsed;
    }

    private static void dataContracts() throws Exception {
        byte[] type = ascii("application/octet-stream"), id = {9, 8}, payload = {1, 2, 3, 4};
        NdefRecord binary = new NdefRecord(NdefRecord.TNF_MIME_MEDIA, type, id, payload);
        // These calls existed in the previous placeholders but discarded their arguments.
        check(binary.getTnf() == NdefRecord.TNF_MIME_MEDIA, "Record constructor retains TNF");
        check(Arrays.equals(type, binary.getType()), "Record constructor retains type");
        check(Arrays.equals(id, binary.getId()), "Record constructor retains ID");
        check(Arrays.equals(payload, binary.getPayload()), "Record constructor retains payload");
        binary.getType()[0] = 0;
        binary.getId()[0] = 0;
        binary.getPayload()[0] = 0;
        check(Arrays.equals(type, binary.getType()) && Arrays.equals(id, binary.getId())
                && Arrays.equals(payload, binary.getPayload()), "Record getters return copies");
        NdefMessage binaryMessage = new NdefMessage(new NdefRecord[]{binary});
        roundTrip(binaryMessage);
        sameRecord(binary, new NdefRecord(binary.toByteArray()));
        check(binary.describeContents() == 0 && binaryMessage.describeContents() == 0,
                "NDEF has no file descriptors");
        check(NdefRecord.CREATOR.newArray(3).length == 3
                && NdefMessage.CREATOR.newArray(2).length == 2, "Parcelable array factories");

        // Independently specified URI bytes: MB|ME|SR|TNF_WELL_KNOWN, type U,
        // URI prefix 03 (http://), followed by ASCII abc.
        byte[] knownUri = hex("D1 01 04 55 03 61 62 63");
        NdefRecord uri = NdefRecord.createUri("HTTP://abc");
        check(Uri.parse("http://abc").equals(uri.toUri()), "URI scheme normalization");
        check(Arrays.equals(knownUri, new NdefMessage(uri).toByteArray()), "Known URI wire vector");
        sameRecord(uri, new NdefMessage(knownUri).getRecords()[0]);
        sameRecord(uri, NdefRecord.createUri(Uri.parse("http://abc")));
        NdefMessage independent = new NdefMessage(knownUri);
        knownUri[knownUri.length - 1] = 'z';
        check("http://abc".equals(independent.getRecords()[0].toUri().toString()),
                "Parsed record owns its input bytes");

        NdefRecord text = NdefRecord.createTextRecord("he", "שלום");
        byte[] textPayload = text.getPayload();
        check(textPayload[0] == 2 && textPayload[1] == 'h' && textPayload[2] == 'e',
                "Text language prefix");
        check("שלום".equals(new String(textPayload, 3, textPayload.length - 3,
                StandardCharsets.UTF_8)), "Text payload retains UTF-8");
        check("text/plain".equals(text.toMimeType()) && text.toUri() == null,
                "Text MIME/URI projection");
        NdefRecord mime = NdefRecord.createMime(" Text/Plain; charset=utf-8 ", ascii("payload"));
        check("text/plain".equals(mime.toMimeType()), "MIME normalization");
        NdefRecord external = NdefRecord.createExternal(" EXAMPLE.TEST ", " DATA ", payload);
        check(Arrays.equals(ascii("example.test:data"), external.getType()), "External type normalization");
        check("vnd.android.nfc://ext/example.test:data".equals(external.toUri().toString()),
                "External URI projection");
        NdefRecord application = NdefRecord.createApplicationRecord("com.example.app");
        check(Arrays.equals(ascii("android.com:pkg"), application.getType())
                && Arrays.equals(ascii("com.example.app"), application.getPayload()),
                "Application record preserves package");
        roundTrip(new NdefMessage(uri, text, mime, external, application, binary));
        NdefRecord poster = new NdefRecord(NdefRecord.TNF_WELL_KNOWN,
                NdefRecord.RTD_SMART_POSTER, null, new NdefMessage(uri).toByteArray());
        check(uri.toUri().equals(poster.toUri()), "Smart poster nested URI");
        NdefRecord empty = new NdefRecord(NdefRecord.TNF_EMPTY, null, null, null);
        roundTrip(new NdefMessage(empty));
        check(empty.toUri() == null && empty.toMimeType() == null, "Empty record projections");

        for (int size : new int[]{0, 1, 255, 256, 1024}) {
            byte[] bytes = new byte[size];
            for (int i = 0; i < size; i++) bytes[i] = (byte)i;
            NdefMessage message = new NdefMessage(new NdefRecord(NdefRecord.TNF_UNKNOWN,
                    null, null, bytes));
            check(((message.toByteArray()[0] & 0x10) != 0) == (size < 256),
                    "Short-record payload boundary " + size);
            roundTrip(message);
        }
        byte[] maximumField = new byte[255];
        Arrays.fill(maximumField, (byte)'x');
        roundTrip(new NdefMessage(new NdefRecord(NdefRecord.TNF_MIME_MEDIA,
                maximumField, maximumField, payload)));
        NdefMessage chunked = new NdefMessage(hex("B5 00 02 01 02 56 00 02 03 04"));
        check(chunked.getRecords().length == 1, "Chunks combine into one logical record");
        check(Arrays.equals(payload, chunked.getRecords()[0].getPayload()), "Chunk payload concatenation");
        check(Arrays.equals(hex("D5 00 04 01 02 03 04"), chunked.toByteArray()),
                "Chunked input serializes as a canonical logical record");

        expect(NullPointerException.class, () -> new NdefMessage((byte[])null), "Null wire input");
        expect(IllegalArgumentException.class, () -> new NdefMessage(new NdefRecord[0]), "Empty record array");
        expect(NullPointerException.class, () -> new NdefMessage(new NdefRecord[]{null}), "Null record entry");
        expect(IllegalArgumentException.class,
                () -> new NdefRecord(NdefRecord.TNF_EMPTY, null, null, payload), "Nonempty TNF_EMPTY");
        expect(IllegalArgumentException.class,
                () -> new NdefRecord(NdefRecord.TNF_UNCHANGED, null, null, payload), "Logical TNF_UNCHANGED");
        expect(IllegalArgumentException.class, () -> NdefRecord.createUri(""), "Empty URI");
        expect(IllegalArgumentException.class, () -> NdefRecord.createMime("/plain", payload), "Invalid MIME major type");
        expect(IllegalArgumentException.class, () -> NdefRecord.createExternal("", "data", payload), "Empty external domain");
        expect(IllegalArgumentException.class, () -> NdefRecord.createTextRecord("x".repeat(64), "text"),
                "Text language length boundary");
        for (String malformed : new String[]{"", "D1", "51 01 04 55 03 61 62 63",
                "D1 01 04 55 03 61", "D1 01 04 55 03 61 62 63 00", "D0 00 01 07",
                "D6 00 00", "F5 00 00", "B5 00 01 01 55 00 01 02", "C5 00 00 A0 00 01"}) {
            expect(FormatException.class, () -> new NdefMessage(hex(malformed)),
                    "Malformed wire must throw FormatException: " + malformed);
        }
    }

    private static void hardwareContracts() throws Exception {
        check(NfcAdapter.getDefaultAdapter(new ContextWrapper(null)) == null,
                "Data support must not advertise NFC hardware");
        expect(IllegalArgumentException.class, () -> NfcAdapter.getDefaultAdapter(null), "Null NFC context");
        expect(NullPointerException.class, () -> CardEmulation.getInstance(null), "Card emulation requires an adapter");
        // The fixture alone constructs the otherwise unreachable no-hardware
        // adapter to verify that a non-null value cannot advertise card emulation.
        java.lang.reflect.Constructor<NfcAdapter> adapterConstructor = NfcAdapter.class.getDeclaredConstructor();
        adapterConstructor.setAccessible(true);
        NfcAdapter unavailable = adapterConstructor.newInstance();
        expect(UnsupportedOperationException.class, () -> CardEmulation.getInstance(unavailable),
                "Non-null unsupported adapter must not fabricate card emulation");
        check(Tag.class.getConstructors().length == 0, "No public constructor fabricates an empty tag");
        expect(UnsupportedOperationException.class, () -> Tag.CREATOR.createFromParcel(null),
                "Unsupported tag parcel must not return a fabricated empty tag");
        check(Tag.CREATOR.newArray(2).length == 2, "Tag callback/parcel array linkage remains available");
        java.lang.reflect.Constructor<NfcEvent> eventConstructor =
                NfcEvent.class.getDeclaredConstructor(NfcAdapter.class, byte.class);
        eventConstructor.setAccessible(true);
        for (int version : new int[]{0x00, 0x12, 0xab, 0xff}) {
            NfcEvent event = eventConstructor.newInstance(null, (byte)version);
            check(event.nfcAdapter == null && event.peerLlcpMajorVersion == ((version >> 4) & 15)
                    && event.peerLlcpMinorVersion == (version & 15), "Public LLCP version fields " + version);
        }
    }

    private static void parcelContracts() throws Exception {
        NdefRecord record = new NdefRecord(NdefRecord.TNF_MIME_MEDIA, ascii("text/plain"),
                new byte[]{4, 5}, new byte[]{1, 2, 3});
        Parcel parcel = Parcel.obtain();
        try {
            record.writeToParcel(parcel, 0);
            parcel.writeInt(0x1976);
            parcel.setDataPosition(0);
            sameRecord(record, NdefRecord.CREATOR.createFromParcel(parcel));
            check(parcel.readInt() == 0x1976, "Record creator consumes exactly its parcel data");
            check(parcel.dataPosition() == parcel.dataSize(), "Record parcel fully consumed");
        } finally { parcel.recycle(); }
        NdefMessage message = new NdefMessage(record, NdefRecord.createUri("https://example.test"));
        parcel = Parcel.obtain();
        try {
            message.writeToParcel(parcel, 0);
            parcel.writeInt(0x1977);
            parcel.setDataPosition(0);
            NdefMessage restored = NdefMessage.CREATOR.createFromParcel(parcel);
            check(message.equals(restored), "Message parcel preserves every record");
            check(Arrays.equals(message.toByteArray(), restored.toByteArray()), "Message parcel preserves wire bytes");
            check(parcel.readInt() == 0x1977, "Message creator consumes exactly its parcel data");
            check(parcel.dataPosition() == parcel.dataSize(), "Message parcel fully consumed");
        } finally { parcel.recycle(); }
    }

    public static void main(String[] args) throws Exception {
        boolean nativeParcel = args.length == 0;
        if (nativeParcel) System.loadLibrary("android_runtime");
        else System.load(args[0]);
        dataContracts();
        hardwareContracts();
        if (nativeParcel) parcelContracts();
        System.out.println("NdefDataContractTest all passed: " + checks + " checks; parcel=" + nativeParcel);
    }
}
