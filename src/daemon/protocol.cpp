#include "daemon/protocol.hpp"

#include <cstdio>

namespace tribios {
namespace {

void append_escaped(std::string& out, const std::string& field) {
  char buffer[8];
  for (unsigned char c : field) {
    if (c == '%' || c == '\t' || c == '\n' || c == '\r' || c < 0x20) {
      std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
      out += buffer;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

std::string encode_message(const std::vector<std::string>& fields) {
  std::string out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) out.push_back('\t');
    append_escaped(out, fields[i]);
  }
  out.push_back('\n');
  return out;
}

std::vector<std::string> decode_message(const std::string& line) {
  std::vector<std::string> fields{std::string{}};
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\n' || c == '\r') break;
    if (c == '\t') {
      fields.emplace_back();
      continue;
    }
    if (c == '%' && i + 2 < line.size()) {
      const int high = hex_value(line[i + 1]);
      const int low = hex_value(line[i + 2]);
      if (high >= 0 && low >= 0) {
        fields.back().push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    fields.back().push_back(c);
  }
  return fields;
}

}  // namespace tribios
