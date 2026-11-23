/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.database.Cursor;
import android.provider.ContactsContract.CommonDataKinds.Email;
import android.provider.ContactsContract.CommonDataKinds.Event;
import android.provider.ContactsContract.CommonDataKinds.Im;
import android.provider.ContactsContract.CommonDataKinds.Nickname;
import android.provider.ContactsContract.CommonDataKinds.Note;
import android.provider.ContactsContract.CommonDataKinds.Organization;
import android.provider.ContactsContract.CommonDataKinds.Phone;
import android.provider.ContactsContract.CommonDataKinds.Photo;
import android.provider.ContactsContract.CommonDataKinds.Relation;
import android.provider.ContactsContract.CommonDataKinds.SipAddress;
import android.provider.ContactsContract.CommonDataKinds.StructuredName;
import android.provider.ContactsContract.CommonDataKinds.StructuredPostal;
import android.provider.ContactsContract.CommonDataKinds.Website;
import android.util.Base64;

import java.nio.charset.StandardCharsets;

/** Emits a UTF-8 vCard 3.0 snapshot of the device Profile. */
final class ProfileVCard {
    private static final String BREAK = "\r\n";
    private final StringBuilder mCard = new StringBuilder();

    private ProfileVCard() { }

    /** Rows contain mimetype, data1 through data10, and data15, in that order. */
    static byte[] encode(String displayName, Cursor rows, boolean includePhoto) {
        ProfileVCard card = new ProfileVCard();
        card.line("BEGIN", "VCARD");
        card.line("VERSION", "3.0");
        card.line("FN", escape(displayName));
        String structuredName = ";;;;";
        boolean hasName = false;
        boolean hasPhoto = false;
        while (rows.moveToNext()) {
            String value = rows.getType(1) == Cursor.FIELD_TYPE_BLOB ? null : rows.getString(1);
            switch (rows.getString(0)) {
                case StructuredName.CONTENT_ITEM_TYPE:
                    if (!hasName) {
                        structuredName = fields(rows, 3, 2, 5, 4, 6);
                        hasName = true;
                    }
                    break;
                case Phone.CONTENT_ITEM_TYPE:
                    card.textIfPresent("TEL" + phoneType(rows.getInt(2)), value);
                    break;
                case Email.CONTENT_ITEM_TYPE:
                    card.textIfPresent("EMAIL;TYPE=INTERNET" + locationType(rows.getInt(2), ","),
                            value);
                    break;
                case StructuredPostal.CONTENT_ITEM_TYPE:
                    String address = fields(rows, 5, 6, 4, 7, 8, 9, 10);
                    if (";;;;;;".equals(address)) address = ";;" + escape(value) + ";;;;";
                    card.line("ADR" + locationType(rows.getInt(2), ";TYPE="), address);
                    break;
                case Organization.CONTENT_ITEM_TYPE:
                    card.line("ORG", fields(rows, 1, 5));
                    card.textIfPresent("TITLE", rows.getString(4));
                    break;
                case Nickname.CONTENT_ITEM_TYPE:
                    card.textIfPresent("NICKNAME", value);
                    break;
                case Note.CONTENT_ITEM_TYPE:
                    card.textIfPresent("NOTE", value);
                    break;
                case Website.CONTENT_ITEM_TYPE:
                    card.textIfPresent("URL", value);
                    break;
                case Event.CONTENT_ITEM_TYPE:
                    if (rows.getInt(2) == Event.TYPE_BIRTHDAY) card.textIfPresent("BDAY", value);
                    else card.androidProperty(Event.CONTENT_ITEM_TYPE, rows);
                    break;
                case Relation.CONTENT_ITEM_TYPE:
                    card.androidProperty(Relation.CONTENT_ITEM_TYPE, rows);
                    break;
                case SipAddress.CONTENT_ITEM_TYPE:
                    if (value != null && !value.isEmpty()) {
                        card.textIfPresent("IMPP", value.startsWith("sip:") ? value : "sip:" + value);
                    }
                    break;
                case Im.CONTENT_ITEM_TYPE:
                    card.im(rows, value);
                    break;
                case Photo.CONTENT_ITEM_TYPE:
                    if (includePhoto && !hasPhoto) hasPhoto = card.photo(rows.getBlob(11));
                    break;
                default:
                    break;
            }
        }
        card.line("N", structuredName);
        card.mCard.append("END:VCARD").append(BREAK);
        return card.mCard.toString().getBytes(StandardCharsets.UTF_8);
    }

    private void im(Cursor rows, String value) {
        if (rows.isNull(5)) return;
        String property = switch (rows.getInt(5)) {
            case Im.PROTOCOL_AIM -> "X-AIM";
            case Im.PROTOCOL_MSN -> "X-MSN";
            case Im.PROTOCOL_YAHOO -> "X-YAHOO";
            case Im.PROTOCOL_SKYPE -> "X-SKYPE-USERNAME";
            case Im.PROTOCOL_QQ -> "X-QQ";
            case Im.PROTOCOL_GOOGLE_TALK -> "X-GOOGLE-TALK";
            case Im.PROTOCOL_ICQ -> "X-ICQ";
            case Im.PROTOCOL_JABBER -> "X-JABBER";
            default -> null;
        };
        if (property != null) textIfPresent(property + locationType(rows.getInt(2), ";TYPE="), value);
    }

    private void androidProperty(String mimetype, Cursor rows) {
        // These kinds use only data1 through data3. Leave the remaining Android fields empty.
        line("X-ANDROID-CUSTOM", mimetype + ";" + fields(rows, 1, 2, 3) + ";".repeat(12));
    }

    private static String phoneType(int type) {
        return switch (type) {
            case Phone.TYPE_HOME -> ";TYPE=HOME,VOICE";
            case Phone.TYPE_MOBILE -> ";TYPE=CELL";
            case Phone.TYPE_WORK -> ";TYPE=WORK,VOICE";
            case Phone.TYPE_FAX_WORK -> ";TYPE=WORK,FAX";
            case Phone.TYPE_FAX_HOME -> ";TYPE=HOME,FAX";
            case Phone.TYPE_PAGER -> ";TYPE=PAGER";
            default -> "";
        };
    }

    private static String locationType(int type, String prefix) {
        return switch (type) {
            case 1 -> prefix + "HOME";
            case 2 -> prefix + "WORK";
            default -> "";
        };
    }

    private boolean photo(byte[] data) {
        if (data == null || data.length < 3) return false;
        String type;
        if ((data[0] & 255) == 255 && (data[1] & 255) == 216) {
            type = "JPEG";
        } else if ((data[0] & 255) == 137 && data[1] == 'P' && data[2] == 'N') {
            type = "PNG";
        } else if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
            type = "GIF";
        } else {
            return false;
        }
        line("PHOTO;ENCODING=b;TYPE=" + type, Base64.encodeToString(data, Base64.NO_WRAP));
        return true;
    }

    private static String fields(Cursor rows, int... columns) {
        StringBuilder values = new StringBuilder();
        for (int i = 0; i < columns.length; i++) {
            if (i != 0) values.append(';');
            values.append(escape(rows.getString(columns[i])));
        }
        return values.toString();
    }

    private static String escape(String value) {
        if (value == null) return "";
        return value.replace("\\", "\\\\").replace("\r\n", "\n").replace("\r", "\n")
                .replace("\n", "\\n").replace(";", "\\;").replace(",", "\\,");
    }

    private void textIfPresent(String property, String value) {
        if (value != null && !value.isEmpty()) line(property, escape(value));
    }

    private void line(String name, String value) {
        String line = name + ':' + value;
        int bytes = 0;
        for (int offset = 0; offset < line.length();) {
            int codePoint = line.codePointAt(offset);
            int width = codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3 : 4;
            if (bytes + width > 75) {
                mCard.append(BREAK).append(' ');
                bytes = 1;
            }
            mCard.appendCodePoint(codePoint);
            bytes += width;
            offset += Character.charCount(codePoint);
        }
        mCard.append(BREAK);
    }
}
