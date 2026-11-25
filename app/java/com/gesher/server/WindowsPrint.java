/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.print.PrintAttributes;

import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.function.BooleanSupplier;

/**
 * Windows printers for {@link PrintManagerService} (compat/print, docs/print.md):
 * the default printer and its settings, Windows' print dialog and printing a PDF.
 *
 * <p>Attributes describe what Windows reports for the chosen DEVMODE, measured on the
 * printer's DC: the oriented page, its resolution, the unprintable edges as minimum
 * margins, color and duplex. A Windows paper that is one of Android's standard media
 * sizes (within a millimeter) is reported as that size; any other keeps Windows' own
 * name and dimensions. Nothing is reported that the driver did not.
 */
final class WindowsPrint implements PrintSpooler.Host {
    private static volatile Boolean sAvailable;

    /** Whether the runtime has the print natives (a Windows build of libandroid_runtime). */
    static boolean available() {
        Boolean available = sAvailable;
        if (available == null) {
            try {
                nativeDefaultPrinter();
                available = true;
            } catch (UnsatisfiedLinkError e) {
                available = false;
            }
            sAvailable = available;
        }
        return available;
    }

    @Override
    public PrintSpooler.Choice defaults(Object requested) throws IOException {
        String printer = nativeDefaultPrinter();
        if (printer == null) return null;
        byte[] devmode = nativeDefaultSettings(printer);
        if (requested instanceof PrintAttributes attributes) {
            devmode = nativeApply(printer, devmode, request(attributes));
        }
        return choice(printer, devmode, null);
    }

    @Override
    public PrintSpooler.Choice choose(String label, int owner, PrintSpooler.Choice initial, int pageCount,
            BooleanSupplier active) throws IOException {
        Object[] chosen = nativeChoose(initial == null ? null : initial.printer,
                initial == null ? null : (byte[]) initial.settings, pageCount, owner, active);
        if (chosen == null) return null;
        int[] pages = (int[]) chosen[2];
        return choice((String) chosen[0], (byte[]) chosen[1], pages.length == 0 ? null : pages);
    }

    @Override
    public void print(String label, PrintSpooler.Choice choice, File pdf, int[] filePages,
            BooleanSupplier active) throws IOException {
        int[] pages = PrintSpooler.isAll(filePages) ? new int[0] : filePages;
        int job = nativePrint(choice.printer, (byte[]) choice.settings, pdf.getAbsolutePath(), pages,
                label, null, active);
        SystemServer.log("print: Windows accepted job " + job + " on " + choice.printer);
    }

    private static PrintSpooler.Choice choice(String printer, byte[] devmode, int[] pages)
            throws IOException {
        Object[] described = nativeDescribe(printer, devmode);
        int[] v = (int[]) described[0];
        return new PrintSpooler.Choice(printer, attributes(v, (String) described[1]), pages,
                v[COPIES], devmode);
    }

    // nativeDescribe's values.
    static final int PAPER = 0, WIDTH = 1, HEIGHT = 2, LANDSCAPE = 3, COLOR = 4, COLOR_CAPABLE = 5,
            DUPLEX = 6, DUPLEX_CAPABLE = 7, DPI_X = 8, DPI_Y = 9, COPIES = 10, MARGIN_LEFT = 11,
            MARGIN_TOP = 12, MARGIN_RIGHT = 13, MARGIN_BOTTOM = 14, VALUES = 15;
    // wingdi.h DMDUP_*.
    static final int DMDUP_SIMPLEX = 1, DMDUP_VERTICAL = 2, DMDUP_HORIZONTAL = 3;

    private static final PrintAttributes.MediaSize[] STANDARD = {
        PrintAttributes.MediaSize.ISO_A0, PrintAttributes.MediaSize.ISO_A1,
        PrintAttributes.MediaSize.ISO_A2, PrintAttributes.MediaSize.ISO_A3,
        PrintAttributes.MediaSize.ISO_A4, PrintAttributes.MediaSize.ISO_A5,
        PrintAttributes.MediaSize.ISO_A6, PrintAttributes.MediaSize.ISO_B4,
        PrintAttributes.MediaSize.ISO_B5, PrintAttributes.MediaSize.ISO_C5,
        PrintAttributes.MediaSize.JIS_B4, PrintAttributes.MediaSize.JIS_B5,
        PrintAttributes.MediaSize.NA_LETTER, PrintAttributes.MediaSize.NA_LEGAL,
        PrintAttributes.MediaSize.NA_GOVT_LETTER, PrintAttributes.MediaSize.NA_TABLOID,
        PrintAttributes.MediaSize.NA_INDEX_4X6, PrintAttributes.MediaSize.NA_INDEX_5X8,
        PrintAttributes.MediaSize.NA_MONARCH, PrintAttributes.MediaSize.NA_QUARTO,
    };
    /** A millimeter, in thousandths of an inch. */
    private static final int TOLERANCE_MILS = 40;

    /** The PrintAttributes Windows' settings print with. */
    static PrintAttributes attributes(int[] v, String paperName) {
        int width = Math.min(v[WIDTH], v[HEIGHT]), height = Math.max(v[WIDTH], v[HEIGHT]);
        PrintAttributes.MediaSize media = null;
        for (PrintAttributes.MediaSize standard : STANDARD) {
            if (Math.abs(standard.getWidthMils() - width) <= TOLERANCE_MILS
                    && Math.abs(standard.getHeightMils() - height) <= TOLERANCE_MILS) {
                media = standard;
                break;
            }
        }
        if (media == null) {
            String label = paperName == null || paperName.isEmpty()
                    ? width + " x " + height + " mils" : paperName;
            media = new PrintAttributes.MediaSize("WINDOWS_PAPER_" + v[PAPER] + "_" + width + "x" + height,
                    label, width, height);
        }
        if (v[LANDSCAPE] != 0) media = media.asLandscape();
        int duplex = v[DUPLEX] == DMDUP_VERTICAL ? PrintAttributes.DUPLEX_MODE_LONG_EDGE
                : v[DUPLEX] == DMDUP_HORIZONTAL ? PrintAttributes.DUPLEX_MODE_SHORT_EDGE
                : PrintAttributes.DUPLEX_MODE_NONE;
        return new PrintAttributes.Builder()
                .setMediaSize(media)
                .setResolution(new PrintAttributes.Resolution("WINDOWS_" + v[DPI_X] + "x" + v[DPI_Y],
                        v[DPI_X] + " x " + v[DPI_Y] + " dpi", v[DPI_X], v[DPI_Y]))
                .setMinMargins(new PrintAttributes.Margins(v[MARGIN_LEFT], v[MARGIN_TOP],
                        v[MARGIN_RIGHT], v[MARGIN_BOTTOM]))
                .setColorMode(v[COLOR] != 0 ? PrintAttributes.COLOR_MODE_COLOR
                        : PrintAttributes.COLOR_MODE_MONOCHROME)
                .setDuplexMode(duplex)
                .build();
    }

    /** An app's requested attributes as print_win32.h's Request (-1 or 0: none). */
    static int[] request(PrintAttributes attributes) {
        int[] request = {0, 0, -1, -1, -1};
        PrintAttributes.MediaSize media = attributes.getMediaSize();
        if (media != null) {
            // Thousandths of an inch to tenths of a millimeter.
            request[0] = Math.round(Math.min(media.getWidthMils(), media.getHeightMils()) * 0.254f);
            request[1] = Math.round(Math.max(media.getWidthMils(), media.getHeightMils()) * 0.254f);
            request[2] = media.isPortrait() ? 0 : 1;
        }
        if (attributes.getColorMode() == PrintAttributes.COLOR_MODE_COLOR) request[3] = 2;
        else if (attributes.getColorMode() == PrintAttributes.COLOR_MODE_MONOCHROME) request[3] = 1;
        switch (attributes.getDuplexMode()) {
            case PrintAttributes.DUPLEX_MODE_NONE: request[4] = DMDUP_SIMPLEX; break;
            case PrintAttributes.DUPLEX_MODE_LONG_EDGE: request[4] = DMDUP_VERTICAL; break;
            case PrintAttributes.DUPLEX_MODE_SHORT_EDGE: request[4] = DMDUP_HORIZONTAL; break;
            default: break;
        }
        return request;
    }

    /** GetDefaultPrinter; null when Windows has none. */
    static native String nativeDefaultPrinter();
    /** The printer's default DEVMODE. */
    static native byte[] nativeDefaultSettings(String printer) throws IOException;
    /** The DEVMODE with request (width, height, landscape, color, duplex) applied where supported. */
    static native byte[] nativeApply(String printer, byte[] devmode, int[] request) throws IOException;
    /** {int[VALUES], paper name} for the DEVMODE on its printer. */
    static native Object[] nativeDescribe(String printer, byte[] devmode) throws IOException;
    /** {printer, DEVMODE, int[] zero-based page pairs (empty: all)}, or null when dismissed. */
    static native Object[] nativeChoose(String printer, byte[] devmode, int pageCount, int ownerPid,
            BooleanSupplier active) throws IOException;
    /**
     * Prints and returns the Windows job id once Windows accepted the job;
     * {@link InterruptedIOException} when {@code active} became false.
     */
    static native int nativePrint(String printer, byte[] devmode, String pdf, int[] pages, String name,
            String output, BooleanSupplier active) throws IOException;
}
