// Gesher (print, docs/print.md): Windows printers for Android's print framework.
// Plain Win32/WinRT from C++, shared by the JNI layer (print_jni.cc), the CI host
// test (tests/print/print_windows_test.cc) and the manual probe
// (tests/print/print_probe.cc).
//
// - Printers and their capabilities: winspool's EnumPrinters, GetDefaultPrinter,
//   DocumentProperties and DeviceCapabilities, and the printer DC's GetDeviceCaps
//   for the printable area. Nothing is invented: a value the driver does not
//   report is reported as absent.
// - The user's choice: Windows' own print dialog (PrintDlgEx), owned by the app's
//   window, preselecting the printer and settings it is given.
// - Printing: Windows.Data.Pdf renders each page of the app's PDF to a bitmap at
//   the printer's resolution (capped), and GDI prints it (StartDoc/StartPage/
//   StretchDIBits/EndPage/EndDoc). A job counts as accepted only when EndDoc
//   succeeds, which is when the Windows spooler holds the whole job.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace gesher::print {

struct Printer {
  std::wstring name;
  bool is_default = false;
  // PRINTER_INFO_2: when the spooler could not answer, status is 0 and port/driver are empty.
  DWORD status = 0;
  DWORD attributes = 0;
  std::wstring port;
  std::wstring driver;
};

// Local and connected printers (PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS).
HRESULT ListPrinters(std::vector<Printer>* out);
// The user's default printer; S_FALSE and an empty name when there is none.
HRESULT DefaultPrinter(std::wstring* name);

struct Paper {
  WORD id = 0;
  std::wstring name;
  LONG width = 0;   // tenths of a millimeter, portrait
  LONG height = 0;
};

struct Capabilities {
  std::vector<Paper> papers;
  std::vector<POINT> resolutions;  // dots per inch, x and y
  bool color = false;
  bool duplex = false;
  bool landscape = false;
  DWORD max_copies = 1;
  std::vector<BYTE> default_devmode;
};

HRESULT QueryCapabilities(const std::wstring& printer, Capabilities* out);

// The driver's default DEVMODE for the printer (DocumentProperties).
HRESULT DefaultSettings(const std::wstring& printer, std::vector<BYTE>* devmode);

// What an app asked for, applied to a DEVMODE where the driver supports it.
struct Request {
  LONG width = 0;    // tenths of a millimeter, portrait; 0: no media request
  LONG height = 0;
  int landscape = -1;  // -1: none, 0: portrait, 1: landscape
  int color = -1;      // -1: none, DMCOLOR_MONOCHROME or DMCOLOR_COLOR
  int duplex = -1;     // -1: none, DMDUP_SIMPLEX, DMDUP_VERTICAL or DMDUP_HORIZONTAL
};
HRESULT ApplyRequest(const std::wstring& printer, const Request& request,
                     std::vector<BYTE>* devmode);

// What a DEVMODE prints with on its printer, measured on the printer's DC.
struct Description {
  WORD paper = 0;
  std::wstring paper_name;
  LONG width_mils = 0;   // the oriented page, thousandths of an inch
  LONG height_mils = 0;
  bool landscape = false;
  bool color = false;          // what this DEVMODE prints
  bool color_capable = false;  // DC_COLORDEVICE
  int duplex = DMDUP_SIMPLEX;
  bool duplex_capable = false;
  LONG dpi_x = 0;
  LONG dpi_y = 0;
  int copies = 1;
  LONG margins_mils[4] = {0, 0, 0, 0};  // left, top, right, bottom: the unprintable edges
};
HRESULT Describe(const std::wstring& printer, const std::vector<BYTE>& devmode, Description* out);

struct Choice {
  std::wstring printer;
  std::vector<BYTE> devmode;
  std::vector<int> pages;  // zero-based inclusive pairs; empty: all pages
};

// Windows' print dialog. S_OK with a choice when the user chose Print, S_FALSE when
// the dialog was dismissed or active() became false. page_count < 0: unknown, so
// no page selection is offered. active is polled on this thread while it shows.
HRESULT ChoosePrinter(const std::wstring& printer, const std::vector<BYTE>& devmode,
                      int page_count, DWORD owner_pid, bool (*active)(void*), void* context,
                      Choice* out);

// Prints pages (zero-based inclusive pairs, end clipped to the document; empty: all)
// of the PDF to the printer. output: StartDoc's lpszOutput (a file for a printer that
// writes one, such as Microsoft Print to PDF), empty for the printer's own port.
// S_OK only after EndDoc succeeded; E_ABORT when active() became false (the job
// is aborted); otherwise the failure.
HRESULT PrintPdf(const std::wstring& printer, const std::vector<BYTE>& devmode,
                 const std::wstring& pdf, const std::vector<int>& pages,
                 const std::wstring& document_name, const std::wstring& output,
                 bool (*active)(void*), void* context, DWORD* job_id, int* printed_pages);

}  // namespace gesher::print
