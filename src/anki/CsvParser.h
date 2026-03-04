#pragma once

#include <string>
#include <vector>

struct CsvRow {
  std::vector<std::string> fields;
};

class CsvParser {
 public:
  // Returns the delimiter implied by the file extension (.tsv → tab, otherwise comma).
  static char delimiterForPath(const std::string& path);

  // Parse a file from SD card. Delimiter is sniffed from the first line.
  static bool parseFile(const std::string& path, std::vector<CsvRow>& rows);

  // Write rows back to file using the given delimiter. Uses temp file + rename for crash safety.
  static bool writeFile(const std::string& path, const std::vector<CsvRow>& rows,
                        char delim = ',');

  // Parse a single line with the given delimiter. Respects RFC 4180 quoting.
  static CsvRow parseLine(const char* data, size_t len, char delim = ',');

  // Serialize a row to a line string with the given delimiter (with quoting where needed).
  static std::string serializeLine(const CsvRow& row, char delim = ',');
};
