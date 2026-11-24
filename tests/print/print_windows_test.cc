// Gesher (print): Windows host contracts of compat/print/print_win32.cc, with no
// UI. Needs the Print Spooler service and the "Microsoft Print to PDF" printer
// (present on GitHub's Windows runners). Prints a generated PDF to a file
// through that printer and checks Windows wrote it.
//
// cl /nologo /std:c++17 /EHsc /W4 /DNOGDI /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Icompat\devices
//    compat\print\print_win32.cc compat\devices\winrt.cc tests\print\print_windows_test.cc
//    /Fe:print-windows-test.exe winspool.lib comdlg32.lib gdi32.lib user32.lib ole32.lib

// Exercise the production header's GDI opt-in before any Windows header loads.
#include "../../compat/print/print_win32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "test_pdf.h"

namespace pr = gesher::print;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* description, HRESULT hr = S_OK) {
  ++checks;
  if (condition) {
    std::printf("PASS %s\n", description);
  } else {
    ++failures;
    std::printf("FAIL %s (HRESULT 0x%08lx)\n", description, static_cast<unsigned long>(hr));
  }
  std::fflush(stdout);
}

std::string Narrow(const std::wstring& s) {
  int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size : 0, '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring TempPath(const wchar_t* name) {
  wchar_t directory[MAX_PATH + 1];
  DWORD length = GetTempPathW(MAX_PATH + 1, directory);
  return std::wstring(directory, length) + name;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

// Windows writes a file printer's output after EndDoc, as the spooler despools it.
std::string WaitForOutput(const std::wstring& path, DWORD timeout_ms) {
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  std::string last;
  while (GetTickCount64() < deadline) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size = {};
      GetFileSizeEx(file, &size);
      std::string data(static_cast<size_t>(size.QuadPart), '\0');
      DWORD read = 0;
      BOOL ok = data.empty() || ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
      CloseHandle(file);
      if (ok && read == data.size() && data.size() > 8 && data.find("%%EOF") != std::string::npos) {
        return data;
      }
    }
    Sleep(250);
  }
  return last;
}

struct Countdown {
  int remaining;
};
bool ActiveWhileCounting(void* context) {
  auto* countdown = static_cast<Countdown*>(context);
  return countdown->remaining-- > 0;
}

}  // namespace

int main() {
  const std::wstring pdf_printer = L"Microsoft Print to PDF";

  std::vector<pr::Printer> printers;
  HRESULT hr = pr::ListPrinters(&printers);
  Check(SUCCEEDED(hr), "EnumPrinters lists the printers", hr);
  bool found = false;
  for (const pr::Printer& printer : printers) {
    std::printf("  printer: %s%s port=%s driver=%s status=0x%lx\n", Narrow(printer.name).c_str(),
                printer.is_default ? " (default)" : "", Narrow(printer.port).c_str(),
                Narrow(printer.driver).c_str(), static_cast<unsigned long>(printer.status));
    if (printer.name == pdf_printer) found = true;
  }
  Check(found, "Microsoft Print to PDF is a real local printer");
  std::wstring default_printer;
  hr = pr::DefaultPrinter(&default_printer);
  Check(SUCCEEDED(hr), "GetDefaultPrinter answers (S_FALSE when there is none)", hr);
  std::printf("  default: %s\n", Narrow(default_printer).c_str());
  if (!found) {
    std::printf("%d checks, %d failures\n", checks, failures + 1);
    return 1;
  }

  pr::Capabilities caps;
  hr = pr::QueryCapabilities(pdf_printer, &caps);
  Check(SUCCEEDED(hr) && !caps.default_devmode.empty(), "DeviceCapabilities and the default DEVMODE", hr);
  bool a4 = false, letter = false;
  for (const pr::Paper& paper : caps.papers) {
    if (paper.width == 2100 && paper.height == 2970) a4 = true;
    if (std::abs(paper.width - 2159) <= 1 && std::abs(paper.height - 2794) <= 1) letter = true;
  }
  std::printf("  papers=%zu resolutions=%zu color=%d duplex=%d landscape=%d copies=%lu\n", caps.papers.size(),
              caps.resolutions.size(), caps.color, caps.duplex, caps.landscape,
              static_cast<unsigned long>(caps.max_copies));
  Check(a4 && letter, "the driver's paper list includes A4 and Letter with their sizes");
  Check(!caps.resolutions.empty(), "the driver reports its resolutions");

  pr::Description described;
  hr = pr::Describe(pdf_printer, caps.default_devmode, &described);
  Check(SUCCEEDED(hr) && described.width_mils > 1000 && described.height_mils > 1000 && described.dpi_x > 0,
        "the default settings measure a real page on the printer DC", hr);
  std::printf("  default page %ldx%ld mils at %ldx%ld dpi, margins %ld %ld %ld %ld, paper %u %s\n",
              described.width_mils, described.height_mils, described.dpi_x, described.dpi_y,
              described.margins_mils[0], described.margins_mils[1], described.margins_mils[2],
              described.margins_mils[3], described.paper, Narrow(described.paper_name).c_str());

  std::vector<BYTE> landscape = caps.default_devmode;
  pr::Request request;
  request.width = 2100;
  request.height = 2970;
  request.landscape = 1;
  hr = pr::ApplyRequest(pdf_printer, request, &landscape);
  Check(SUCCEEDED(hr), "an A4 landscape request is applied by the driver", hr);
  pr::Description a4_landscape;
  hr = pr::Describe(pdf_printer, landscape, &a4_landscape);
  Check(SUCCEEDED(hr) && a4_landscape.landscape && std::abs(a4_landscape.width_mils - 11693) <= 30 &&
            std::abs(a4_landscape.height_mils - 8268) <= 30 && a4_landscape.paper == DMPAPER_A4,
        "and the DC measures A4 landscape", hr);

  const std::wstring input = TempPath(L"gesher-print-test-input.pdf");
  const std::wstring output = TempPath(L"gesher-print-test-output.pdf");
  DeleteFileW(output.c_str());
  Check(WriteFileBytes(input, gesher::print::test::MakeTestPdf(3)), "a 3-page test PDF is written");

  DWORD job = 0;
  int printed = 0;
  hr = pr::PrintPdf(pdf_printer, caps.default_devmode, input, {1, 2}, L"Gesher print test", output, nullptr,
                    nullptr, &job, &printed);
  Check(hr == S_OK && job > 0 && printed == 2, "pages 2-3 are rendered and Windows accepts the job (EndDoc)", hr);
  std::printf("  job %lu, %d pages\n", static_cast<unsigned long>(job), printed);
  std::string written = WaitForOutput(output, 60000);
  Check(written.rfind("%PDF", 0) == 0, "Microsoft Print to PDF wrote the output file");
  std::regex page_object("/Type\\s*/Page(?![s\\w])");
  auto pages = std::distance(std::sregex_iterator(written.begin(), written.end(), page_object), std::sregex_iterator());
  std::printf("  output %zu bytes, %ld page objects\n", written.size(), static_cast<long>(pages));
  Check(pages == 2, "the output has exactly the two printed pages");

  const std::wstring aborted = TempPath(L"gesher-print-test-aborted.pdf");
  DeleteFileW(aborted.c_str());
  Countdown countdown{2};
  hr = pr::PrintPdf(pdf_printer, caps.default_devmode, input, {}, L"Gesher print cancel", aborted,
                    ActiveWhileCounting, &countdown, &job, &printed);
  Check(hr == E_ABORT && printed < 3, "a canceled print stops before the last page and aborts the job", hr);

  const std::wstring garbage = TempPath(L"gesher-print-test-garbage.pdf");
  WriteFileBytes(garbage, "this is not a PDF");
  hr = pr::PrintPdf(pdf_printer, caps.default_devmode, garbage, {}, L"Gesher print garbage", aborted, nullptr,
                    nullptr, &job, &printed);
  Check(FAILED(hr) && printed == 0, "a malformed PDF fails without a page", hr);

  hr = pr::PrintPdf(pdf_printer, caps.default_devmode, input, {7, 9}, L"Gesher print range", aborted, nullptr,
                    nullptr, &job, &printed);
  Check(FAILED(hr) && printed == 0, "pages outside the document fail instead of printing nothing", hr);

  std::vector<BYTE> none;
  hr = pr::DefaultSettings(L"Gesher no such printer", &none);
  Check(FAILED(hr), "an unknown printer has no settings", hr);

  DeleteFileW(input.c_str());
  DeleteFileW(output.c_str());
  DeleteFileW(aborted.c_str());
  DeleteFileW(garbage.c_str());
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
