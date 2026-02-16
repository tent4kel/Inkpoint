#pragma once

#include <string>
#include <vector>

struct CsvRow {
  std::vector<std::string> fields;
};

class CsvParser {
 public:
  // Parse a CSV file from SD card. First row is treated as header.
  static bool parseFile(const std::string& path, std::vector<CsvRow>& rows);

  // Write rows back to CSV. Uses temp file + rename for crash safety.
  static bool writeFile(const std::string& path, const std::vector<CsvRow>& rows);

  // Parse a single CSV line respecting RFC 4180 quoting.
  static CsvRow parseLine(const char* data, size_t len);

  // Serialize a row to a CSV line string (with quoting where needed).
  static std::string serializeLine(const CsvRow& row);
};
