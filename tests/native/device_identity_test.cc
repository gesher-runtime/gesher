// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// The production SMBIOS parsing and identity rules (gesher/device_identity.h)
// on firmware-shaped tables: real vendors, placeholder strings, a baseboard
// fallback, Gesher's fallback, virtual machines, malformed tables. Run by
// tools/test_device_identity.py; the Windows read itself is checked against
// Win32_ComputerSystem in tests/windows/tracks/43-exec2.ps1.

#include <gesher/device_identity.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace id = gesher::device_identity;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

void Same(const std::string& actual, const std::string& expected, const std::string& what) {
  Check(actual == expected, what + ": \"" + actual + "\" (expected \"" + expected + "\")");
}

// One SMBIOS structure: type, formatted area (string indexes at the given
// offsets), then its strings.
struct Structure {
  uint8_t type;
  uint8_t length;
  std::vector<std::pair<uint8_t, uint8_t>> indexes;  // offset, string number
  std::vector<std::string> strings;
};

std::vector<uint8_t> Table(const std::vector<Structure>& structures) {
  std::vector<uint8_t> table;
  uint16_t handle = 0;
  for (const Structure& s : structures) {
    size_t at = table.size();
    table.resize(at + s.length, 0);
    table[at] = s.type;
    table[at + 1] = s.length;
    table[at + 2] = static_cast<uint8_t>(handle);
    table[at + 3] = static_cast<uint8_t>(handle++ >> 8);
    // SMBIOS has no empty strings: an absent one is string number 0.
    std::vector<uint8_t> renumbered(256, 0);
    uint8_t count = 0;
    for (size_t i = 0; i < s.strings.size(); ++i) {
      if (s.strings[i].empty()) continue;
      renumbered[i + 1] = ++count;
      table.insert(table.end(), s.strings[i].begin(), s.strings[i].end());
      table.push_back(0);
    }
    for (auto [offset, number] : s.indexes) {
      table[at + offset] = number <= s.strings.size() ? renumbered[number] : number;
    }
    if (count == 0) table.push_back(0);
    table.push_back(0);
  }
  return table;
}

// RawSMBIOSData, as GetSystemFirmwareTable('RSMB') returns it.
std::vector<uint8_t> Raw(const std::vector<uint8_t>& table, uint32_t declared) {
  std::vector<uint8_t> raw = {0, 3, 4, 0};
  for (int i = 0; i < 4; ++i) raw.push_back(static_cast<uint8_t>(declared >> (8 * i)));
  raw.insert(raw.end(), table.begin(), table.end());
  return raw;
}

Structure Bios() { return {0, 0x18, {{4, 1}, {5, 2}, {8, 3}}, {"American Megatrends Inc.", "1.21.0", "03/15/2024"}}; }
Structure System(const std::string& manufacturer, const std::string& product, const std::string& version,
                 const std::string& family) {
  return {1, 0x1b, {{4, 1}, {5, 2}, {6, 3}, {7, 4}, {0x19, 5}, {0x1a, 6}},
          {manufacturer, product, version, "SERIAL123", "SKU1", family}};
}
Structure Board(const std::string& manufacturer, const std::string& product) {
  return {2, 0x0f, {{4, 1}, {5, 2}, {6, 3}}, {manufacturer, product, "Rev 1.xx"}};
}
Structure End() { return {127, 4, {}, {}}; }

id::Identity FromTable(const std::vector<Structure>& structures) {
  std::vector<uint8_t> table = Table(structures);
  std::vector<uint8_t> raw = Raw(table, static_cast<uint32_t>(table.size()));
  return id::Resolve(id::ParseRawSmbios(raw.data(), raw.size()));
}

void Vendors() {
  id::Identity dell = FromTable({Bios(), System("Dell Inc.", "XPS 13 9310", "", "XPS"), Board("Dell Inc.", "0DXP1F"), End()});
  Same(dell.manufacturer, "Dell Inc.", "Dell manufacturer");
  Same(dell.model, "XPS 13 9310", "Dell model");

  id::Identity hp = FromTable({System("HP", "HP EliteBook 840 G8 Notebook PC", "", "103C_5336AN HP EliteBook"), End()});
  Same(hp.manufacturer, "HP", "HP manufacturer");
  Same(hp.model, "HP EliteBook 840 G8 Notebook PC", "HP model");

  id::Identity lenovo = FromTable({System("LENOVO", "20HRCTO1WW", "ThinkPad X1 Carbon 5th", "ThinkPad X1 Carbon 5th"), End()});
  Same(lenovo.manufacturer, "LENOVO", "Lenovo manufacturer");
  Same(lenovo.model, "ThinkPad X1 Carbon 5th", "Lenovo model is its system version");
  id::Identity lenovo_bare = FromTable({System("LENOVO", "81DE", "LENOVO", "IDEAPAD"), End()});
  Same(lenovo_bare.model, "81DE", "Lenovo without a model name keeps its product name");

  id::Identity surface = FromTable({System("Microsoft Corporation", "Surface Laptop 5", "124I:00036T:000M:0300000D:11000000:00", "Surface"), End()});
  Same(surface.model, "Surface Laptop 5", "Surface model");
}

void VirtualMachines() {
  id::Identity hyperv = FromTable({System("Microsoft Corporation", "Virtual Machine", "Hyper-V UEFI Release v4.1", "Virtual Machine"),
                                   Board("Microsoft Corporation", "Virtual Machine"), End()});
  Same(hyperv.manufacturer, "Microsoft Corporation", "Hyper-V manufacturer is real");
  Same(hyperv.model, "Virtual Machine", "Hyper-V model is real");
  id::Identity vmware = FromTable({System("VMware, Inc.", "VMware7,1", "None", ""), End()});
  Same(vmware.manufacturer, "VMware, Inc.", "VMware manufacturer");
  Same(vmware.model, "VMware7,1", "VMware model");
  id::Identity vbox = FromTable({System("innotek GmbH", "VirtualBox", "1.2", "Virtual Machine"), End()});
  Same(vbox.manufacturer + "/" + vbox.model, "innotek GmbH/VirtualBox", "VirtualBox identity");
  id::Identity qemu = FromTable({System("QEMU", "Standard PC (Q35 + ICH9, 2009)", "pc-q35-8.2", ""), End()});
  Same(qemu.manufacturer + "/" + qemu.model, "QEMU/Standard PC (Q35 + ICH9, 2009)", "QEMU identity");
}

void Placeholders() {
  // An ASUS desktop board: the system strings are unfilled, the board's are real.
  id::Identity asus = FromTable({Bios(), System("System manufacturer", "System Product Name", "System Version", "To be filled by O.E.M."),
                                 Board("ASUSTeK COMPUTER INC.", "PRIME B450M-A"), End()});
  Same(asus.manufacturer, "ASUSTeK COMPUTER INC.", "ASUS board manufacturer");
  Same(asus.model, "PRIME B450M-A", "ASUS board product");
  // ASRock/AMI templates everywhere: Gesher's fallback, never another vendor.
  id::Identity unfilled = FromTable({System("To Be Filled By O.E.M.", "To Be Filled By O.E.M.", "To Be Filled By O.E.M.", "Default string"),
                                     Board("Default string", "Default string"), End()});
  Same(unfilled.manufacturer, id::kFallbackManufacturer, "Unfilled manufacturer falls back to Gesher");
  Same(unfilled.model, id::kFallbackModel, "Unfilled model falls back to PC");
  id::Identity blank = FromTable({System("   ", "\t", "", ""), End()});
  Same(blank.manufacturer + "/" + blank.model, "Gesher/PC", "Whitespace-only strings fall back");
  id::Identity none = FromTable({Bios(), End()});
  Same(none.manufacturer + "/" + none.model, "Gesher/PC", "No system or board structure falls back");
  id::Identity family = FromTable({System("Framework", "Default string", "", "Laptop 13"), End()});
  Same(family.manufacturer + "/" + family.model, "Framework/Laptop 13", "Family when the product is a placeholder");
  id::Identity insyde = FromTable({System("Type1 - Manufacturer", "Type1ProductConfigId", "Type1Version", "Type1Family"),
                                   Board("Type2 - Board Manufacturer", "Type2 - Board Product Name1"), End()});
  Same(insyde.manufacturer + "/" + insyde.model, "Gesher/PC", "Insyde templates fall back");

  for (const char* junk : {"", " ", "To Be Filled By O.E.M.", "to be filled by o.e.m.", "System manufacturer",
                           "System Product Name", "Default string", "DEFAULT STRING", "Not Applicable", "Not Specified",
                           "N/A", "None", "Unknown", "O.E.M.", "OEM", "0", "0000000000", "xxxxxxxx", "........", "---",
                           "Type1ProductConfigId", "Type2 - Board Vendor Name1", "123456789", "x.x"}) {
    Check(id::Placeholder(id::Clean(junk)), std::string("placeholder: ") + junk);
  }
  for (const char* real : {"Dell Inc.", "HP", "LENOVO", "ASUSTeK COMPUTER INC.", "Micro-Star International Co., Ltd.",
                           "Microsoft Corporation", "Virtual Machine", "QEMU", "Acer", "X1", "ThinkPad X1 Carbon 5th"}) {
    Check(!id::Placeholder(id::Clean(real)), std::string("real name: ") + real);
  }
}

void Cleaning() {
  Same(id::Clean("  Dell\tInc. \r\n"), "Dell Inc.", "Whitespace collapsed and trimmed");
  Same(id::Clean(std::string("Acer\x01\x7f", 6)), "Acer", "Control characters dropped");
  Same(id::Clean("Gigabyte \xff\xfe Technology"), "Gigabyte Technology", "Bytes that are not UTF-8 dropped");
  Same(id::Clean("Caf\xc3\xa9 PC"), "Caf\xc3\xa9 PC", "UTF-8 kept");
  Same(id::Clean("\xc0\xaf\xed\xa0\x80X"), "X", "Overlong and surrogate forms dropped");
  std::string longer(80, 'a');
  longer += " \xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac";
  std::string cleaned = id::Clean(longer);
  Check(cleaned.size() <= id::kMaxValueBytes, "Value fits PROP_VALUE_MAX");
  Same(cleaned, std::string(80, 'a') + " \xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", "Cut at a whole character");
  auto properties = id::Properties({"Dell Inc.", "XPS 13 9310"});
  Check(properties.size() == 2 && properties[0].first == "ro.product.manufacturer" &&
            properties[0].second == "Dell Inc." && properties[1].first == "ro.product.model" &&
            properties[1].second == "XPS 13 9310",
        "Properties are Build.MANUFACTURER's and Build.MODEL's");
}

void Malformed() {
  std::vector<uint8_t> table = Table({System("Dell Inc.", "XPS 13 9310", "", ""), End()});
  // A declared length past the buffer reads only the buffer.
  std::vector<uint8_t> raw = Raw(table, 0xffffffffu);
  id::Identity whole = id::Resolve(id::ParseRawSmbios(raw.data(), raw.size()));
  Same(whole.model, "XPS 13 9310", "Declared length past the buffer");
  // Every truncation ends the walk without reading past the data.
  for (size_t n = 0; n <= raw.size(); ++n) {
    std::vector<uint8_t> cut(raw.begin(), raw.begin() + static_cast<long>(n));
    id::Identity partial = id::Resolve(id::ParseRawSmbios(cut.empty() ? nullptr : cut.data(), cut.size()));
    Check(!partial.manufacturer.empty() && !partial.model.empty(), "Truncated at " + std::to_string(n));
  }
  // A formatted length below the header, and string numbers past the strings.
  std::vector<uint8_t> bad = {1, 2, 0, 0, 0, 0};
  Same(id::Resolve(id::ParseSmbiosTable(bad.data(), bad.size())).model, "PC", "Formatted length below 4");
  Structure dangling = {1, 0x1b, {{4, 9}, {5, 200}}, {"Only one"}};
  std::vector<uint8_t> dangling_table = Table({dangling, End()});
  id::Identity d = id::Resolve(id::ParseSmbiosTable(dangling_table.data(), dangling_table.size()));
  Same(d.manufacturer + "/" + d.model, "Gesher/PC", "String numbers past the strings");
  // A short type 1 (SMBIOS 2.0, no family) still gives its product.
  Structure old_system = {1, 0x08, {{4, 1}, {5, 2}}, {"Compaq", "Deskpro"}};
  std::vector<uint8_t> old_table = Table({old_system, End()});
  id::Identity old_identity = id::Resolve(id::ParseSmbiosTable(old_table.data(), old_table.size()));
  Same(old_identity.manufacturer + "/" + old_identity.model, "Compaq/Deskpro", "SMBIOS 2.0 system structure");
  // Pseudo-random bytes never crash or produce an empty identity.
  uint32_t seed = 0x6765736b;
  for (int round = 0; round < 20000; ++round) {
    std::vector<uint8_t> noise(static_cast<size_t>(round % 97));
    for (uint8_t& byte : noise) {
      seed = seed * 1664525u + 1013904223u;
      byte = static_cast<uint8_t>(seed >> 24);
      if (byte > 200) byte = static_cast<uint8_t>(byte % 3);  // small types and lengths
    }
    id::Identity noisy = id::Resolve(id::ParseRawSmbios(noise.data(), noise.size()));
    if (noisy.manufacturer.empty() || noisy.model.empty() || noisy.manufacturer.size() > id::kMaxValueBytes ||
        noisy.model.size() > id::kMaxValueBytes) {
      Check(false, "Noise round " + std::to_string(round));
      break;
    }
  }
}

}  // namespace

int main() {
  Vendors();
  VirtualMachines();
  Placeholders();
  Cleaning();
  Malformed();
  if (failures) {
    std::fprintf(stderr, "device identity: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("device identity contracts passed: %d checks\n", checks);
  return 0;
}
