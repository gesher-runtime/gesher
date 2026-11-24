# Printing

Android's print framework (`android.print`, `PrintManager.print(jobName,
PrintDocumentAdapter, attributes)`, used by WebView printing, PDF viewers,
receipt apps and androidx `PrintHelper`) prints to the Windows printers.

## Before

Gesher had no `print` service and did not declare `android.software.print`.
`SystemServiceRegistry` (android-16.0.0_r4) then creates `PrintManager` with a
null service, and `PrintManager.print()` logs "Feature android.software.print
not available" and returns null. That is the Android contract for a device
without printing, and it is still what a runtime without the print natives
reports.

## The contract

| Android (android-16.0.0_r4) | Gesher |
| --- | --- |
| `FEATURE_PRINTING` | Declared when the runtime has the print natives (`WindowsPrint.available()`), not per installed printer: on Android it describes the framework. With no printer, Windows' dialog says so and the job is CANCELED. |
| `IPrintManager.print` (UserState.print) | `PrintManagerService.print`: a CREATED `PrintJobInfo` (new `PrintJobId`, appId, label, copies 1, creation time, the app's attributes) and a one-shot immutable activity `IntentSender` for `ACTION_PRINT_DIALOG` with `printjob:<id>`, created for the app. Job tracking ends when the adapter's binder dies (PrintJobForAppCache). |
| PrintSpooler's `PrintActivity` | The IntentSender start reaches `ActivityTaskManagerService.startPrintDialog`: the printing activity is paused, as an activity covering it would pause it, and resumed when the job leaves its dialog. Only the creating app's pending job can be started, once. |
| `RemotePrintDocument` call order | `start()`, `layout(empty, defaults, EXTRA_PRINT_PREVIEW=true)`, the user's choice, `layout(defaults, chosen, EXTRA_PRINT_PREVIEW=false)`, `write(chosen pages, fd)`, `finish()`. Sequence numbers, `ICancellationSignal` and stale callbacks are handled as RemotePrintDocument does; written pages are checked as `PageRangeUtils.computeWhichPagesInFileToPrint`. |
| Spooler UI (printer, copies, pages, media, color, duplex) | Windows' own print dialog (`PrintDlgEx`), owned by the app's window, preselecting the default printer with the app's requested media, orientation, color and duplex applied where the driver supports them. Its preferences pages set media, orientation, color, duplex and quality. |
| `PrintAttributes` | Measured from the chosen DEVMODE on the printer DC: the oriented page size (Android's standard `MediaSize` when within 1 mm, otherwise Windows' paper name and size), the DC resolution, the unprintable edges as minimum margins, color and duplex. |
| Print service (QUEUED, STARTED, COMPLETED, FAILED) | QUEUED once the document is written, STARTED while Windows renders and spools it, COMPLETED only after `EndDoc` succeeded (the Windows spooler holds the whole job), FAILED with Windows' error otherwise; `restartPrintJob` re-queues a FAILED job with its document. |
| CANCELED | A dismissed dialog, an activity destroyed before the job was queued, an adapter `onLayoutFailed`/`onWriteFailed`/cancel, written pages missing a chosen page (as PrintActivity.onPause/onPrintDocumentError), a final layout that no longer contains any chosen page (nothing is written; PrintActivity offers no print button for such a range, and a partly overlapping choice keeps only its pages in the document), and `cancelPrintJob` on a QUEUED, STARTED or FAILED job (a STARTED one aborts the Windows job). As in UserState, `cancelPrintJob` leaves a job still in its dialog to the user. |
| `getPrintJobs`, `getPrintJob`, state listeners | The caller's own jobs (appId from its Binder identity), with printer name, `PrinterId(android/com.gesher.server.PrintManagerService, <printer>)`, attributes, pages, copies and document info. |

Printing renders each chosen PDF page with Windows.Data.Pdf at the printer's
resolution (capped at 300 dpi and 64 Mpixels) onto the whole sheet and prints it with GDI
(`StartDoc`/`StartPage`/`StretchDIBits`/`EndPage`/`EndDoc`). Copies are the
driver's (`dmCopies`, with `PD_USEDEVMODECOPIESANDCOLLATE`). "Microsoft Print
to PDF" is a real printer: Windows asks where to save its file.

## Not implemented

- Third-party `PrintService` apps (`android.printservice`): not bound;
  `getPrintServices` and recommendations are empty, printer discovery sessions
  (a system API of the spooler) are not offered.
- A preview: the spooler's preview writes are not requested (only the final
  write), so `onWrite` is called once per job.
- `STATE_BLOCKED` and progress: the Windows job is not followed after the
  spooler accepted it (offline/paper-out states are not reported back).
- Errors from the app's adapter are recorded in the job status and log, not
  shown in a Windows message.
- Vector printing: pages are rasterized.

## Tests

- `tools/test_print_spooler_contracts.py`: the production `PrintSpooler`,
  unchanged, with a scripted oneway adapter and Windows host (states, call
  order, page ranges, cancellation and time-outs at each stage, failures,
  restart).
- `tests/print/print_windows_test.cc` (CI `windows-host-contracts`): real
  `EnumPrinters`, capabilities, DEVMODE requests measured on the DC, and a
  generated 3-page PDF of which pages 2-3 are printed to Microsoft Print to PDF
  into a file, checked for exactly two pages; cancellation, malformed PDF and
  page ranges outside the document.
- `tests/print/print_probe.cc`: list printers, show capabilities, open the
  dialog and print a test page on a real machine (`tests/print/HUMAN_TEST.md`).

## Manual test

On Windows with the pinned llvm-mingw (or `cl` from a Developer Command Prompt):

```
x86_64-w64-mingw32-clang++ -std=c++17 -O2 -static -municode -Icompat/devices ^
  compat/print/print_win32.cc compat/devices/winrt.cc tests/print/print_probe.cc ^
  -o print_probe.exe -lwinspool -lcomdlg32 -lgdi32 -luser32 -lole32
print_probe list
print_probe caps "Microsoft Print to PDF"
print_probe dialog 5
print_probe testpage "<a real printer>" 2
print_probe testpage "Microsoft Print to PDF" 2 %TEMP%\gesher-print.pdf
```
