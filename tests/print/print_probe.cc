// Gesher (print): a manual probe of compat/print/print_win32.cc on a real Windows
// machine: list printers, show their capabilities, open Windows' print dialog and
// print a generated test page or a PDF to a chosen printer.
//
//   print_probe list
//   print_probe caps "<printer>"
//   print_probe dialog [page count]
//   print_probe testpage "<printer>" [pages] [output file]
//   print_probe print "<printer>" <file.pdf> [first-last] [output file]
//
// Build and run commands: docs/print.md, "Manual test".

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "../../compat/print/print_win32.h"
#include "test_pdf.h"

namespace pr = gesher::print;

namespace {

std::string Narrow(const std::wstring& s) {
  int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size : 0, '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
  return out;
}

int Fail(const char* what, HRESULT hr) {
  std::printf("%s failed: HRESULT 0x%08lx\n", what, static_cast<unsigned long>(hr));
  return 1;
}

void PrintDescription(const pr::Description& d) {
  std::printf("  paper %u \"%s\": %ld x %ld mils (%s)\n", d.paper, Narrow(d.paper_name).c_str(), d.width_mils,
              d.height_mils, d.landscape ? "landscape" : "portrait");
  std::printf("  resolution %ld x %ld dpi, %s%s, duplex %d%s, copies %d\n", d.dpi_x, d.dpi_y,
              d.color ? "color" : "monochrome", d.color_capable ? " (color capable)" : "", d.duplex,
              d.duplex_capable ? " (duplex capable)" : "", d.copies);
  std::printf("  unprintable margins (mils): left %ld top %ld right %ld bottom %ld\n", d.margins_mils[0],
              d.margins_mils[1], d.margins_mils[2], d.margins_mils[3]);
}

int List() {
  std::vector<pr::Printer> printers;
  HRESULT hr = pr::ListPrinters(&printers);
  if (FAILED(hr)) return Fail("EnumPrinters", hr);
  std::printf("%zu printer(s)\n", printers.size());
  for (const pr::Printer& p : printers) {
    std::printf("%s%s\n  port %s, driver %s, status 0x%lx, attributes 0x%lx\n", Narrow(p.name).c_str(),
                p.is_default ? "  [default]" : "", Narrow(p.port).c_str(), Narrow(p.driver).c_str(),
                static_cast<unsigned long>(p.status), static_cast<unsigned long>(p.attributes));
  }
  return 0;
}

int Caps(const std::wstring& printer) {
  pr::Capabilities caps;
  HRESULT hr = pr::QueryCapabilities(printer, &caps);
  if (FAILED(hr)) return Fail("QueryCapabilities", hr);
  std::printf("%s: color %s, duplex %s, landscape %s, up to %lu copies\n", Narrow(printer).c_str(),
              caps.color ? "yes" : "no", caps.duplex ? "yes" : "no", caps.landscape ? "yes" : "no",
              static_cast<unsigned long>(caps.max_copies));
  std::printf("papers (%zu):\n", caps.papers.size());
  for (const pr::Paper& paper : caps.papers) {
    std::printf("  %5u %-40s %5.1f x %5.1f mm\n", paper.id, Narrow(paper.name).c_str(), paper.width / 10.0,
                paper.height / 10.0);
  }
  std::printf("resolutions (%zu):", caps.resolutions.size());
  for (const POINT& r : caps.resolutions) std::printf(" %ldx%ld", r.x, r.y);
  std::printf("\ndefault settings:\n");
  pr::Description d;
  hr = pr::Describe(printer, caps.default_devmode, &d);
  if (FAILED(hr)) return Fail("Describe", hr);
  PrintDescription(d);
  return 0;
}

bool Always(void*) { return true; }

int Dialog(int pages) {
  std::wstring printer;
  pr::DefaultPrinter(&printer);
  std::vector<BYTE> devmode;
  if (!printer.empty()) pr::DefaultSettings(printer, &devmode);
  pr::Choice choice;
  HRESULT hr = pr::ChoosePrinter(printer, devmode, pages, GetCurrentProcessId(), Always, nullptr, &choice);
  if (FAILED(hr)) return Fail("PrintDlgEx", hr);
  if (hr == S_FALSE) {
    std::printf("dialog dismissed\n");
    return 0;
  }
  std::printf("chosen: %s, pages:", Narrow(choice.printer).c_str());
  if (choice.pages.empty()) std::printf(" all");
  for (size_t i = 0; i + 1 < choice.pages.size(); i += 2) std::printf(" %d-%d", choice.pages[i] + 1, choice.pages[i + 1] + 1);
  std::printf("\n");
  pr::Description d;
  hr = pr::Describe(choice.printer, choice.devmode, &d);
  if (FAILED(hr)) return Fail("Describe", hr);
  PrintDescription(d);
  return 0;
}

int Print(const std::wstring& printer, const std::wstring& pdf, const std::vector<int>& pages,
          const std::wstring& output) {
  std::vector<BYTE> devmode;
  HRESULT hr = pr::DefaultSettings(printer, &devmode);
  if (FAILED(hr)) return Fail("DocumentProperties", hr);
  DWORD job = 0;
  int printed = 0;
  hr = pr::PrintPdf(printer, devmode, pdf, pages, L"Gesher print probe", output, nullptr, nullptr, &job, &printed);
  if (FAILED(hr)) return Fail("PrintPdf", hr);
  std::printf("Windows accepted job %lu with %d page(s) on %s%s%s\n", static_cast<unsigned long>(job), printed,
              Narrow(printer).c_str(), output.empty() ? "" : ", output ", Narrow(output).c_str());
  return 0;
}

std::vector<int> Range(const wchar_t* text) {
  wchar_t* end = nullptr;
  long first = std::wcstol(text, &end, 10);
  long last = first;
  if (end && *end == L'-') last = std::wcstol(end + 1, &end, 10);
  if (!end || *end != L'\0' || first < 1 || last < first) return {};
  return {static_cast<int>(first - 1), static_cast<int>(last - 1)};
}

int Usage() {
  std::printf(
      "print_probe list\n"
      "print_probe caps \"<printer>\"\n"
      "print_probe dialog [page count]\n"
      "print_probe testpage \"<printer>\" [pages] [output file]\n"
      "print_probe print \"<printer>\" <file.pdf> [first-last] [output file]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return Usage();
  std::wstring command = argv[1];
  if (command == L"list") return List();
  if (command == L"caps" && argc >= 3) return Caps(argv[2]);
  if (command == L"dialog") return Dialog(argc >= 3 ? _wtoi(argv[2]) : 3);
  if (command == L"testpage" && argc >= 3) {
    int pages = argc >= 4 ? std::max(1, _wtoi(argv[3])) : 1;
    wchar_t directory[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH + 1, directory);
    std::wstring path = std::wstring(directory, length) + L"gesher-print-probe.pdf";
    std::string pdf = gesher::print::test::MakeTestPdf(pages);
    std::ofstream(path.c_str(), std::ios::binary | std::ios::trunc).write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
    int result = Print(argv[2], path, {}, argc >= 5 ? argv[4] : L"");
    DeleteFileW(path.c_str());
    return result;
  }
  if (command == L"print" && argc >= 4) {
    return Print(argv[2], argv[3], argc >= 5 ? Range(argv[4]) : std::vector<int>(), argc >= 6 ? argv[5] : L"");
  }
  return Usage();
}
