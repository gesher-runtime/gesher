/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A strict RFC 8259 reader and a writer for the JSON documents of the passkey bridge
 * (docs/passkeys.md): an application's WebAuthn request options, a site's asset links, and
 * the WebAuthn responses Gesher serializes. The input comes from applications and from the
 * network, so nothing lenient is accepted: no comments, no unquoted or single-quoted strings,
 * no duplicate member names, no trailing text, and a bounded nesting depth.
 *
 * <p>Values are {@code Map<String, Object>} (in document order), {@code List<Object>},
 * {@code String}, {@code Double}, {@code Boolean} and {@link #NULL}.
 */
final class PasskeyJson {
    /** JSON's null, distinct from an absent member. */
    static final Object NULL = new Object() {
        @Override
        public String toString() {
            return "null";
        }
    };

    private static final int MAX_DEPTH = 32;

    private final String mText;
    private int mAt;

    private PasskeyJson(String text) {
        mText = text;
    }

    /** The document's value. Throws IllegalArgumentException for anything but strict JSON. */
    static Object parse(String text) {
        if (text == null) throw new IllegalArgumentException("no JSON text");
        PasskeyJson reader = new PasskeyJson(text);
        Object value = reader.value(0);
        reader.skipSpace();
        if (reader.mAt != text.length()) throw reader.bad("text after the value");
        return value;
    }

    /** The document's object. Throws IllegalArgumentException when it is not an object. */
    static Map<String, Object> parseObject(String text) {
        Object value = parse(text);
        Map<String, Object> object = asObject(value);
        if (object == null) throw new IllegalArgumentException("JSON value is not an object");
        return object;
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> asObject(Object value) {
        return value instanceof Map ? (Map<String, Object>) value : null;
    }

    @SuppressWarnings("unchecked")
    static List<Object> asArray(Object value) {
        return value instanceof List ? (List<Object>) value : null;
    }

    private IllegalArgumentException bad(String what) {
        return new IllegalArgumentException("invalid JSON at " + mAt + ": " + what);
    }

    private void skipSpace() {
        while (mAt < mText.length()) {
            char c = mText.charAt(mAt);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            mAt++;
        }
    }

    private Object value(int depth) {
        if (depth > MAX_DEPTH) throw bad("nested too deeply");
        skipSpace();
        if (mAt >= mText.length()) throw bad("no value");
        char c = mText.charAt(mAt);
        switch (c) {
            case '{':
                return object(depth);
            case '[':
                return array(depth);
            case '"':
                return string();
            case 't':
                literal("true");
                return Boolean.TRUE;
            case 'f':
                literal("false");
                return Boolean.FALSE;
            case 'n':
                literal("null");
                return NULL;
            default:
                return number();
        }
    }

    private void literal(String word) {
        if (!mText.startsWith(word, mAt)) throw bad("unknown literal");
        mAt += word.length();
    }

    private Map<String, Object> object(int depth) {
        Map<String, Object> members = new LinkedHashMap<>();
        mAt++;
        skipSpace();
        if (mAt < mText.length() && mText.charAt(mAt) == '}') {
            mAt++;
            return members;
        }
        while (true) {
            skipSpace();
            if (mAt >= mText.length() || mText.charAt(mAt) != '"') throw bad("member name");
            String name = string();
            skipSpace();
            if (mAt >= mText.length() || mText.charAt(mAt) != ':') throw bad("':'");
            mAt++;
            Object value = value(depth + 1);
            if (members.put(name, value) != null) throw bad("duplicate member " + name);
            skipSpace();
            if (mAt >= mText.length()) throw bad("unterminated object");
            char c = mText.charAt(mAt++);
            if (c == '}') return members;
            if (c != ',') throw bad("',' or '}'");
        }
    }

    private List<Object> array(int depth) {
        List<Object> elements = new ArrayList<>();
        mAt++;
        skipSpace();
        if (mAt < mText.length() && mText.charAt(mAt) == ']') {
            mAt++;
            return elements;
        }
        while (true) {
            elements.add(value(depth + 1));
            skipSpace();
            if (mAt >= mText.length()) throw bad("unterminated array");
            char c = mText.charAt(mAt++);
            if (c == ']') return elements;
            if (c != ',') throw bad("',' or ']'");
        }
    }

    private String string() {
        StringBuilder out = new StringBuilder();
        mAt++;
        while (true) {
            if (mAt >= mText.length()) throw bad("unterminated string");
            char c = mText.charAt(mAt++);
            if (c == '"') return out.toString();
            if (c < 0x20) throw bad("control character in a string");
            if (c != '\\') {
                out.append(c);
                continue;
            }
            if (mAt >= mText.length()) throw bad("unterminated escape");
            char e = mText.charAt(mAt++);
            switch (e) {
                case '"': out.append('"'); break;
                case '\\': out.append('\\'); break;
                case '/': out.append('/'); break;
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'u': {
                    if (mAt + 4 > mText.length()) throw bad("short \\u escape");
                    int unit = 0;
                    for (int i = 0; i < 4; i++) {
                        int digit = Character.digit(mText.charAt(mAt++), 16);
                        if (digit < 0) throw bad("\\u escape");
                        unit = unit << 4 | digit;
                    }
                    out.append((char) unit);
                    break;
                }
                default:
                    throw bad("escape");
            }
        }
    }

    private Double number() {
        int start = mAt;
        if (mAt < mText.length() && mText.charAt(mAt) == '-') mAt++;
        int digits = digits();
        if (digits == 0) throw bad("value");
        if (digits > 1 && mText.charAt(mAt - digits) == '0') throw bad("leading zero");
        if (mAt < mText.length() && mText.charAt(mAt) == '.') {
            mAt++;
            if (digits() == 0) throw bad("fraction");
        }
        if (mAt < mText.length() && (mText.charAt(mAt) == 'e' || mText.charAt(mAt) == 'E')) {
            mAt++;
            if (mAt < mText.length() && (mText.charAt(mAt) == '+' || mText.charAt(mAt) == '-')) {
                mAt++;
            }
            if (digits() == 0) throw bad("exponent");
        }
        return Double.valueOf(mText.substring(start, mAt));
    }

    private int digits() {
        int start = mAt;
        while (mAt < mText.length() && mText.charAt(mAt) >= '0' && mText.charAt(mAt) <= '9') {
            mAt++;
        }
        return mAt - start;
    }

    /**
     * Appends {@code text} as a JSON string. Escapes exactly as WebAuthn's CCDToString does
     * (https://www.w3.org/TR/webauthn-3/#ccdtostring): the quote, the backslash and
     * everything below U+0020, the latter as lower-case \\u00xx; nothing else.
     */
    static StringBuilder quote(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '"') {
                out.append("\\\"");
            } else if (c == '\\') {
                out.append("\\\\");
            } else if (c < 0x20) {
                out.append("\\u00").append(Character.forDigit(c >> 4, 16))
                        .append(Character.forDigit(c & 0xf, 16));
            } else {
                out.append(c);
            }
        }
        return out.append('"');
    }
}
