/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.tts;

import android.speech.tts.TextToSpeech;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.MissingResourceException;
import java.util.Set;
import java.util.TreeSet;

/**
 * The Windows voices as Android sees them: each voice has a stable Android name (its
 * BCP-47 language tag and a private-use subtag from its Windows name, "he-IL-x-asaf"),
 * its locale, and answers TextToSpeechService's language questions with
 * TextToSpeech.LANG_* for exactly the languages Windows has a voice for. Windows voices
 * have no variants, so LANG_COUNTRY_VAR_AVAILABLE is never answered.
 */
final class VoiceCatalog {
    static final class Entry {
        /** The Android voice name. */
        final String name;
        final Locale locale;
        final String iso3Language;
        final String iso3Country;
        final SpeechBackend.BackendVoice voice;

        Entry(String name, Locale locale, String iso3Language, String iso3Country,
                SpeechBackend.BackendVoice voice) {
            this.name = name;
            this.locale = locale;
            this.iso3Language = iso3Language;
            this.iso3Country = iso3Country;
            this.voice = voice;
        }
    }

    private final List<Entry> mEntries;

    private VoiceCatalog(List<Entry> entries) {
        mEntries = Collections.unmodifiableList(entries);
    }

    static VoiceCatalog of(List<SpeechBackend.BackendVoice> voices) {
        List<SpeechBackend.BackendVoice> sorted = new ArrayList<>(voices);
        // A stable order, whatever order Windows enumerates in: names do not move.
        sorted.sort((a, b) -> {
            int c = a.language.compareToIgnoreCase(b.language);
            if (c == 0) c = a.displayName.compareTo(b.displayName);
            return c != 0 ? c : a.id.compareTo(b.id);
        });
        List<Entry> entries = new ArrayList<>();
        Set<String> names = new HashSet<>();
        Set<String> ids = new HashSet<>();
        for (SpeechBackend.BackendVoice v : sorted) {
            if (!ids.add(v.id)) continue;
            Locale locale = Locale.forLanguageTag(v.language);
            String language = iso3Language(locale.getLanguage());
            if (language.isEmpty()) continue;  // no language Android can name
            String tag = new Locale.Builder().setLocale(locale).clearExtensions().build()
                    .toLanguageTag();
            String slug = slug(v.displayName);
            String name = tag + "-x-" + slug;
            for (int n = 2; !names.add(name.toLowerCase(Locale.ROOT)); n++) {
                String suffix = Integer.toString(n);
                name = tag + "-x-" + slug.substring(0, Math.min(slug.length(), 8 - suffix.length()))
                        + suffix;
            }
            entries.add(new Entry(name, locale, language, iso3Country(locale.getCountry()), v));
        }
        return new VoiceCatalog(entries);
    }

    List<Entry> entries() {
        return mEntries;
    }

    boolean isEmpty() {
        return mEntries.isEmpty();
    }

    Entry byName(String name) {
        if (name == null) return null;
        for (Entry e : mEntries) {
            if (e.name.equalsIgnoreCase(name)) return e;
        }
        return null;
    }

    /** TextToSpeechService.onIsLanguageAvailable's answer. */
    int availability(String lang, String country, String variant) {
        String language = iso3Language(lang);
        String region = iso3Country(country);
        int result = TextToSpeech.LANG_NOT_SUPPORTED;
        for (Entry e : mEntries) {
            if (language.isEmpty() || !e.iso3Language.equals(language)) continue;
            if (!region.isEmpty() && region.equals(e.iso3Country)) {
                return TextToSpeech.LANG_COUNTRY_AVAILABLE;
            }
            result = TextToSpeech.LANG_AVAILABLE;
        }
        return result;
    }

    /**
     * The voice for a locale, or null when Windows has none for its language: a voice of
     * its language and country if there is one, else of its language. Among those,
     * Windows' default voice, then one of {@code preferredCountry} (the device's), then
     * the first in name order.
     */
    Entry defaultFor(String lang, String country, String variant, String preferredCountry) {
        int availability = availability(lang, country, variant);
        if (availability < TextToSpeech.LANG_AVAILABLE) return null;
        String language = iso3Language(lang);
        String region = availability == TextToSpeech.LANG_COUNTRY_AVAILABLE ? iso3Country(country)
                : null;
        String preferred = iso3Country(preferredCountry);
        Entry first = null, ofPreferred = null;
        for (Entry e : mEntries) {
            if (!e.iso3Language.equals(language)) continue;
            if (region != null && !region.equals(e.iso3Country)) continue;
            if (e.voice.isDefault) return e;
            if (first == null) first = e;
            if (ofPreferred == null && !preferred.isEmpty() && preferred.equals(e.iso3Country)) {
                ofPreferred = e;
            }
        }
        return ofPreferred != null ? ofPreferred : first;
    }

    /**
     * TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES: "lang-COUNTRY" in ISO 639-2 / 3166
     * alpha-3 ("heb-ISR"), each once, of all voices or of those {@code requested}
     * (EXTRA_CHECK_VOICE_DATA_FOR, in the same format or as BCP-47 tags) names.
     */
    ArrayList<String> availableVoices(List<String> requested) {
        Set<String> result = new TreeSet<>();
        for (Entry e : mEntries) {
            String voice = e.iso3Country.isEmpty() ? e.iso3Language
                    : e.iso3Language + "-" + e.iso3Country;
            if (requested == null) {
                result.add(voice);
                continue;
            }
            for (String r : requested) {
                if (r == null) continue;
                String[] parts = r.split("[-_]");
                String language = iso3Language(parts[0]);
                String region = parts.length > 1 ? iso3Country(parts[1]) : "";
                if (language.equals(e.iso3Language)
                        && (region.isEmpty() || region.equals(e.iso3Country))) {
                    result.add(voice);
                }
            }
        }
        return new ArrayList<>(result);
    }

    /** ISO 639-2 of a 2- or 3-letter code ("he", "iw" and "heb" are "heb"), or "". */
    static String iso3Language(String code) {
        if (code == null || code.isEmpty()) return "";
        try {
            Locale locale = Locale.forLanguageTag(code.replace('_', '-'));
            return locale.getLanguage().isEmpty() ? ""
                    : locale.getISO3Language().toLowerCase(Locale.ROOT);
        } catch (MissingResourceException e) {
            return "";
        }
    }

    /** ISO 3166 alpha-3 of a 2- or 3-letter region ("IL", "ISR"), or "". */
    static String iso3Country(String code) {
        if (code == null || code.isEmpty()) return "";
        if (code.length() == 3) return code.toUpperCase(Locale.ROOT);
        try {
            Locale locale = Locale.forLanguageTag("und-" + code);
            return locale.getCountry().isEmpty() ? ""
                    : locale.getISO3Country().toUpperCase(Locale.ROOT);
        } catch (MissingResourceException e) {
            return "";
        }
    }

    /**
     * A BCP-47 private-use subtag (1-8 ASCII alphanumerics) from a Windows voice name: its
     * first word after "Microsoft" ("Microsoft Asaf" is "asaf"), or "voice".
     */
    static String slug(String displayName) {
        String name = displayName == null ? "" : displayName.toLowerCase(Locale.ROOT);
        for (String word : name.split("[^a-z0-9]+")) {
            if (word.isEmpty() || word.equals("microsoft")) continue;
            return word.length() > 8 ? word.substring(0, 8) : word;
        }
        return "voice";
    }
}
