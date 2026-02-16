#include "CsvParser.h"

#include <HalStorage.h>
#include <Logging.h>

namespace {

bool needsQuoting(const std::string& field) {
  for (char c : field) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') return true;
  }
  return false;
}

std::string quoteField(const std::string& field) {
  if (!needsQuoting(field)) return field;
  std::string result = "\"";
  for (char c : field) {
    if (c == '"') result += "\"\"";
    else result += c;
  }
  result += '"';
  return result;
}

}  // namespace

CsvRow CsvParser::parseLine(const char* data, size_t len) {
  CsvRow row;
  std::string field;
  bool inQuotes = false;
  size_t i = 0;

  while (i < len) {
    char c = data[i];

    if (inQuotes) {
      if (c == '"') {
        if (i + 1 < len && data[i + 1] == '"') {
          // Escaped quote
          field += '"';
          i += 2;
        } else {
          // End of quoted field
          inQuotes = false;
          i++;
        }
      } else {
        field += c;
        i++;
      }
    } else {
      if (c == '"' && field.empty()) {
        inQuotes = true;
        i++;
      } else if (c == ',') {
        row.fields.push_back(field);
        field.clear();
        i++;
      } else if (c == '\r' || c == '\n') {
        break;
      } else {
        field += c;
        i++;
      }
    }
  }

  row.fields.push_back(field);
  return row;
}

std::string CsvParser::serializeLine(const CsvRow& row) {
  std::string line;
  for (size_t i = 0; i < row.fields.size(); i++) {
    if (i > 0) line += ',';
    line += quoteField(row.fields[i]);
  }
  return line;
}

bool CsvParser::parseFile(const std::string& path, std::vector<CsvRow>& rows) {
  rows.clear();

  FsFile file;
  if (!Storage.openFileForRead("CSV", path, file)) {
    LOG_ERR("CSV", "Failed to open: %s", path.c_str());
    return false;
  }

  const size_t fileSize = file.size();
  if (fileSize == 0) {
    file.close();
    return false;
  }

  // Read entire file into memory (CSVs for flashcards should be small)
  auto* buf = static_cast<char*>(malloc(fileSize + 1));
  if (!buf) {
    LOG_ERR("CSV", "Failed to allocate %zu bytes", fileSize);
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buf, fileSize);
  file.close();
  buf[bytesRead] = '\0';

  // Parse line by line
  size_t pos = 0;
  while (pos < bytesRead) {
    // Skip empty lines
    if (buf[pos] == '\r' || buf[pos] == '\n') {
      pos++;
      continue;
    }

    // Find end of this logical line (respecting quoted fields)
    size_t lineStart = pos;
    bool inQuotes = false;
    while (pos < bytesRead) {
      if (buf[pos] == '"') {
        inQuotes = !inQuotes;
      } else if (!inQuotes && (buf[pos] == '\n')) {
        break;
      }
      pos++;
    }

    size_t lineLen = pos - lineStart;
    // Strip trailing CR
    if (lineLen > 0 && buf[lineStart + lineLen - 1] == '\r') lineLen--;

    if (lineLen > 0) {
      rows.push_back(parseLine(buf + lineStart, lineLen));
    }

    if (pos < bytesRead) pos++;  // skip the \n
  }

  free(buf);
  LOG_DBG("CSV", "Parsed %zu rows from %s", rows.size(), path.c_str());
  return !rows.empty();
}

bool CsvParser::writeFile(const std::string& path, const std::vector<CsvRow>& rows) {
  // Write to temp file first, then replace
  std::string tmpPath = path + ".tmp";

  FsFile file;
  if (!Storage.openFileForWrite("CSV", tmpPath, file)) {
    LOG_ERR("CSV", "Failed to open tmp file for write");
    return false;
  }

  for (const auto& row : rows) {
    std::string line = serializeLine(row) + "\n";
    file.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.size());
  }
  file.flush();
  file.close();

  // Remove original and rename tmp
  Storage.remove(path.c_str());

  // SdFat rename: open tmp and rename it
  FsFile tmpFile = Storage.open(tmpPath.c_str(), O_RDWR);
  if (!tmpFile) {
    LOG_ERR("CSV", "Failed to reopen tmp file for rename");
    return false;
  }
  bool ok = tmpFile.rename(path.c_str());
  tmpFile.close();

  if (!ok) {
    LOG_ERR("CSV", "Rename failed");
    return false;
  }

  LOG_DBG("CSV", "Wrote %zu rows to %s", rows.size(), path.c_str());
  return true;
}
