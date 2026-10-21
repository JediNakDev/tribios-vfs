// Covers daemon/protocol.hpp: the newline-delimited, tab-separated, percent-
// escaped framing that carries every control request and reply. The message
// vocabulary itself lives in src/daemon/control_server.cpp; the messages used
// here are the real request and reply shapes that dispatcher produces.

#include "daemon/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using tribios::decode_message;
using tribios::encode_message;

namespace {

std::vector<std::string> round_trip(const std::vector<std::string>& fields) {
  return decode_message(encode_message(fields));
}

}  // namespace

TEST_CASE("every control request shape survives an encode and decode round trip") {
  const std::vector<std::vector<std::string>> requests = {
      {"ping"},
      {"info"},
      {"shutdown"},
      {"ws.create", "feature-x", "main"},
      {"ws.remove", "feature-x"},
      {"ws.list"},
      {"ws.wait-reclaim", "feature-x"},
      {"stats.upper", "feature-x"},
      {"fs.stat", "feature-x", "src/main.cpp"},
      {"fs.ls", "feature-x", ""},
      {"fs.read", "feature-x", "src/main.cpp"},
      {"fs.readlink", "feature-x", "link"},
      {"fs.write", "feature-x", "src/main.cpp", "int main() { return 0; }\n"},
      {"fs.mv", "feature-x", "old/name.txt", "new/name.txt"},
      {"fs.symlink", "feature-x", "../target", "link"},
      {"fs.chmod", "feature-x", "script.sh", "755"},
      {"fs.truncate", "feature-x", "log.txt", "0"},
  };
  for (const auto& request : requests) {
    CHECK(round_trip(request) == request);
  }
}

TEST_CASE("every control reply shape survives an encode and decode round trip") {
  const std::vector<std::vector<std::string>> replies = {
      {"OK"},
      {"OK", "pong"},
      {"OK", "stopping"},
      {"OK", "/projects/demo", "/projects/demo/mnt", "git", "128", "40960", "37", "mounted"},
      {"OK", "feature-x", "tribios/feature-x", "1832", "/projects/demo/mnt/feature-x"},
      {"OK", "feature-x", "441"},
      {"OK", "dir 755"},
      {"OK", "file 644"},
      {"ERR", "empty request"},
      {"ERR", "unknown request: fs.bogus"},
      {"ERR", "errno 2 No such file or directory"},
      {"ERR", "errno 39 Directory not empty"},
  };
  for (const auto& reply : replies) {
    CHECK(round_trip(reply) == reply);
  }
}

TEST_CASE("a field payload containing the delimiters round-trips without splitting the message") {
  const std::vector<std::string> request = {"fs.write", "feature-x", "notes.txt",
                                            "line one\nline\ttwo\r\nline % three"};
  const std::string encoded = encode_message(request);

  // Exactly one newline, at the very end, and one tab per field separator.
  CHECK(encoded.find('\n') == encoded.size() - 1);
  CHECK(std::count(encoded.begin(), encoded.end(), '\t') == 3);
  CHECK(round_trip(request) == request);
}

TEST_CASE("encode_message escapes exactly the delimiters, the escape marker and control bytes") {
  CHECK(encode_message({"a\tb"}) == "a%09b\n");
  CHECK(encode_message({"a\nb"}) == "a%0Ab\n");
  CHECK(encode_message({"a\rb"}) == "a%0Db\n");
  CHECK(encode_message({"100%"}) == "100%25\n");
  CHECK(encode_message({std::string("a\0b", 3)}) == "a%00b\n");
  CHECK(encode_message({"\x1F"}) == "%1F\n");
}

TEST_CASE("encode_message leaves printable bytes, DEL and high bytes unescaped") {
  CHECK(encode_message({"plain/path-1_2.txt"}) == "plain/path-1_2.txt\n");
  CHECK(encode_message({"\x7F"}) == "\x7F\n");
  CHECK(encode_message({"caf\xC3\xA9"}) == "caf\xC3\xA9\n");
  CHECK(round_trip({"caf\xC3\xA9", "\x7F"}) == std::vector<std::string>{"caf\xC3\xA9", "\x7F"});
}

TEST_CASE("a tab separates fields and empty fields keep their positions") {
  CHECK(encode_message({"a", "b"}) == "a\tb\n");
  CHECK(encode_message({"", "", ""}) == "\t\t\n");
  CHECK(round_trip({"fs.ls", "feature-x", ""}) ==
        std::vector<std::string>{"fs.ls", "feature-x", ""});
  CHECK(round_trip({"", "second"}) == std::vector<std::string>{"", "second"});
}

TEST_CASE("decode_message always yields at least one field, so an empty frame is not empty") {
  // The asymmetry that matters to callers: encoding no fields and decoding the
  // result gives one empty field, not zero. ControlServer treats the request as
  // non-empty and answers "unknown request: " rather than "empty request".
  CHECK(encode_message({}) == "\n");
  CHECK(decode_message("\n") == std::vector<std::string>{""});
  CHECK(decode_message("") == std::vector<std::string>{""});
}

TEST_CASE("decode_message stops at the first line terminator and ignores the rest") {
  CHECK(decode_message("ping\nsecond line\n") == std::vector<std::string>{"ping"});
  CHECK(decode_message("ping\r\nsecond line\n") == std::vector<std::string>{"ping"});
  CHECK(decode_message("OK\tpong\nOK\tpong\n") == std::vector<std::string>{"OK", "pong"});
}

TEST_CASE("decode_message reads a frame that was truncated before its newline") {
  CHECK(decode_message("OK\tpong") == std::vector<std::string>{"OK", "pong"});
  CHECK(decode_message("ws.create\tfeature-x\t") ==
        std::vector<std::string>{"ws.create", "feature-x", ""});
}

TEST_CASE("decode_message accepts either hex case, matching the uppercase encoder") {
  CHECK(decode_message("%6A%6F%62") == std::vector<std::string>{"job"});
  CHECK(decode_message("%6a%6f%62") == std::vector<std::string>{"job"});
}

TEST_CASE("decode_message passes a malformed or truncated escape through literally") {
  CHECK(decode_message("100%") == std::vector<std::string>{"100%"});
  CHECK(decode_message("100%2") == std::vector<std::string>{"100%2"});
  CHECK(decode_message("%ZZ") == std::vector<std::string>{"%ZZ"});
  CHECK(decode_message("%2Zx") == std::vector<std::string>{"%2Zx"});
  CHECK(decode_message("a%") == std::vector<std::string>{"a%"});
  // A truncated escape immediately before the terminator is literal too.
  CHECK(decode_message("a%0\n") == std::vector<std::string>{"a%0"});
}

TEST_CASE("a decoded escape is never re-split into fields") {
  // "%09" decodes to a tab inside one field rather than starting a new field.
  CHECK(decode_message("a%09b") == std::vector<std::string>{"a\tb"});
  CHECK(decode_message("a%0Ab") == std::vector<std::string>{"a\nb"});
  CHECK(decode_message("a%25b") == std::vector<std::string>{"a%b"});
}
