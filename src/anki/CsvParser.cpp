#include "CsvParser.h"

#include <HalStorage.h>
#include <Logging.h>

namespace {

char sniffDelimiter(const char* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] == '\n' || buf[i] == '\r') break;
    if (buf[i] == '\t') return '\t';
  }
  return ',';
}

bool needsQuoting(const std::string& field, char delim) {
  for (char c : field) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') return true;
  }
  return false;
}

std::string quoteField(const std::string& field, char delim) {
  if (!needsQuoting(field, delim)) return field;
  std::string result = "\"";
  for (char c : field) {
    if (c == '"') result += "\"\"";
    else result += c;
  }
  result += '"';
  return result;
}

}  // namespace

char CsvParser::delimiterForPath(const std::string& path) {
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".tsv") return '\t';
  return ',';
}

CsvRow CsvParser::parseLine(const char* data, size_t len, char delim) {
  CsvRow row;
  std::string field;
  bool inQuotes = false;
  size_t i = 0;

  while (i < len) {
    char c = data[i];

    if (inQuotes) {
      if (c == '"') {
        if (i + 1 < len && data[i + 1] == '"') {
          field += '"';
          i += 2;
        } else {
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
      } else if (c == delim) {
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

std::string CsvParser::serializeLine(const CsvRow& row, char delim) {
  std::string line;
  for (size_t i = 0; i < row.fields.size(); i++) {
    if (i > 0) line += delim;
    line += quoteField(row.fields[i], delim);
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

  auto* buf = static_cast<char*>(malloc(fileSize + 1));
  if (!buf) {
    LOG_ERR("CSV", "Failed to allocate %zu bytes", fileSize);
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buf, fileSize);
  file.close();
  buf[bytesRead] = '\0';

  const char delim = sniffDelimiter(buf, bytesRead);

  size_t pos = 0;
  while (pos < bytesRead) {
    if (buf[pos] == '\r' || buf[pos] == '\n') { pos++; continue; }

    size_t lineStart = pos;
    bool inQuotes = false;
    while (pos < bytesRead) {
      if (buf[pos] == '"') inQuotes = !inQuotes;
      else if (!inQuotes && buf[pos] == '\n') break;
      pos++;
    }

    size_t lineLen = pos - lineStart;
    if (lineLen > 0 && buf[lineStart + lineLen - 1] == '\r') lineLen--;

    if (lineLen > 0) {
      rows.push_back(parseLine(buf + lineStart, lineLen, delim));
    }

    if (pos < bytesRead) pos++;
  }

  free(buf);
  LOG_DBG("CSV", "Parsed %zu rows from %s", rows.size(), path.c_str());
  return !rows.empty();
}

bool CsvParser::writeFile(const std::string& path, const std::vector<CsvRow>& rows, char delim) {
  std::string tmpPath = path + ".tmp";

  FsFile file;
  if (!Storage.openFileForWrite("CSV", tmpPath, file)) {
    LOG_ERR("CSV", "Failed to open tmp file for write");
    return false;
  }

  for (const auto& row : rows) {
    std::string line = serializeLine(row, delim) + "\n";
    file.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.size());
  }
  file.flush();
  file.close();

  Storage.remove(path.c_str());

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
