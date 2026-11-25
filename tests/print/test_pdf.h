// Gesher (print): a small, valid PDF for the print tests and probe: US Letter
// pages, each with a border, a filled bar and "Gesher print test page N of M"
// in Helvetica, written with exact xref offsets.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace gesher::print::test {

inline std::string MakeTestPdf(int pages) {
  if (pages < 1) pages = 1;
  std::vector<size_t> offsets;
  std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  auto object = [&](const std::string& body) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
  };
  // 1: catalog, 2: pages, 3: font, then a page and its content per page.
  object("<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (int i = 0; i < pages; ++i) kids += std::to_string(4 + 2 * i) + " 0 R ";
  object("<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(pages) + " >>");
  object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  for (int i = 0; i < pages; ++i) {
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> "
           "/Contents " + std::to_string(5 + 2 * i) + " 0 R >>");
    std::string content =
        "0 0 0 RG 4 w 36 36 540 720 re S\n"
        "0.1 0.3 0.8 rg 72 600 468 60 re f\n"
        "BT /F1 28 Tf 0 0 0 rg 72 520 Td (Gesher print test) Tj ET\n"
        "BT /F1 18 Tf 72 480 Td (Page " + std::to_string(i + 1) + " of " + std::to_string(pages) + ") Tj ET\n";
    object("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
  }
  size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
  char entry[32];
  for (size_t offset : offsets) {
    std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    pdf += entry;
  }
  pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

}  // namespace gesher::print::test
