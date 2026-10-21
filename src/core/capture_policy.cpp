#include "core/capture_policy.hpp"

#include <sys/stat.h>

#include <fstream>
#include <iterator>
#include <optional>

#include "core/paths.hpp"

namespace tribios {

const char* const kCapturePolicyWarning =
    "warning: .tribiosignore reduces what Workspaces receive; it is not a security "
    "boundary";

namespace {

constexpr std::size_t kNoClassEnd = std::string_view::npos;

// Returns the index just past a character class's closing ']', or kNoClassEnd
// when the class is unterminated. A ']' straight after the opening bracket or
// its negation is a literal member, as in Git.
std::size_t find_class_end(std::string_view glob, std::size_t open) {
  std::size_t index = open + 1;
  if (index < glob.size() && (glob[index] == '!' || glob[index] == '^')) ++index;
  if (index < glob.size() && glob[index] == ']') ++index;
  while (index < glob.size() && glob[index] != ']') {
    if (glob[index] == '\\') ++index;
    ++index;
  }
  return index < glob.size() ? index + 1 : kNoClassEnd;
}

bool class_matches(std::string_view glob, std::size_t open, std::size_t end, unsigned char byte) {
  const std::size_t close = end - 1;
  std::size_t index = open + 1;
  bool negated = false;
  if (glob[index] == '!' || glob[index] == '^') {
    negated = true;
    ++index;
  }
  bool matched = false;
  while (index < close) {
    if (glob[index] == '\\') ++index;
    const auto low = static_cast<unsigned char>(glob[index]);
    auto high = low;
    ++index;
    if (index + 1 < close && glob[index] == '-') {
      ++index;
      if (glob[index] == '\\' && index + 1 < close) ++index;
      high = static_cast<unsigned char>(glob[index]);
      ++index;
    }
    if (low <= byte && byte <= high) matched = true;
  }
  return matched != negated;
}

// Rejects overlong forms, surrogates and code points above U+10FFFF.
bool is_valid_utf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    unsigned char second_low = 0x80;
    unsigned char second_high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      continuation = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      continuation = 2;
      if (lead == 0xE0) second_low = 0xA0;
      if (lead == 0xED) second_high = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      continuation = 3;
      if (lead == 0xF0) second_low = 0x90;
      if (lead == 0xF4) second_high = 0x8F;
    } else {
      return false;
    }
    if (index + continuation >= text.size()) return false;
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if (second < second_low || second > second_high) return false;
    for (std::size_t offset = 2; offset <= continuation; ++offset) {
      if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) return false;
    }
    index += continuation + 1;
  }
  return true;
}

// Trailing spaces and tabs are dropped unless a backslash escapes them.
void trim_unescaped_trailing_whitespace(std::string& line) {
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
    std::size_t backslashes = 0;
    for (std::size_t index = line.size() - 1; index > 0 && line[index - 1] == '\\'; --index) {
      ++backslashes;
    }
    if (backslashes % 2 == 1) break;
    line.pop_back();
  }
}

OutcomeVoid validate_glob(std::string_view glob) {
  for (std::size_t index = 0; index < glob.size(); ++index) {
    if (glob[index] == '\\') {
      if (index + 1 == glob.size()) return error("pattern ends with an unfinished escape");
      ++index;
    } else if (glob[index] == '[') {
      const std::size_t end = find_class_end(glob, index);
      if (end == kNoClassEnd) return error("unterminated character class");
      index = end - 1;
    }
  }
  return {};
}

Outcome<std::optional<CapturePattern>> parse_pattern_line(std::string line, int number) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.empty() || line.front() == '#') return std::nullopt;
  trim_unescaped_trailing_whitespace(line);
  if (line.empty()) return std::nullopt;

  CapturePattern pattern;
  pattern.text = line;
  pattern.line = number;
  std::string glob = line;
  if (glob.front() == '!') {
    pattern.negated = true;
    glob.erase(0, 1);
  }
  if (!glob.empty() && glob.back() == '/') {
    pattern.directory_only = true;
    glob.pop_back();
  }
  if (!glob.empty() && glob.front() == '/') {
    pattern.anchored = true;
    glob.erase(0, 1);
  }
  if (glob.find('/') != std::string::npos) pattern.anchored = true;
  if (glob.empty()) return error("pattern names no path");
  if (glob.front() == '/' || glob.back() == '/' || glob.find("//") != std::string::npos) {
    return error("pattern contains an empty path segment");
  }
  if (auto valid = validate_glob(glob); !valid) return std::unexpected(valid.error());
  pattern.glob = std::move(glob);
  return pattern;
}

std::string_view final_segment(std::string_view relative) {
  const auto slash = relative.rfind('/');
  return slash == std::string_view::npos ? relative : relative.substr(slash + 1);
}

}  // namespace

bool match_capture_glob(std::string_view glob, std::string_view text) {
  std::size_t glob_index = 0;
  std::size_t text_index = 0;
  while (glob_index < glob.size()) {
    if (glob[glob_index] == '*') {
      std::size_t after_stars = glob_index;
      while (after_stars < glob.size() && glob[after_stars] == '*') ++after_stars;
      const bool starts_segment = glob_index == 0 || glob[glob_index - 1] == '/';
      const bool ends_segment = after_stars == glob.size() || glob[after_stars] == '/';
      if (after_stars - glob_index >= 2 && starts_segment && ends_segment) {
        if (after_stars == glob.size()) return true;
        // "**/" matches zero or more whole directories.
        const std::string_view rest = glob.substr(after_stars + 1);
        for (std::size_t start = text_index;;) {
          if (match_capture_glob(rest, text.substr(start))) return true;
          const std::size_t slash = text.find('/', start);
          if (slash == std::string_view::npos) return false;
          start = slash + 1;
        }
      }
      const std::string_view rest = glob.substr(after_stars);
      for (std::size_t start = text_index; start <= text.size(); ++start) {
        if (match_capture_glob(rest, text.substr(start))) return true;
        if (start < text.size() && text[start] == '/') return false;
      }
      return false;
    }

    if (text_index >= text.size()) return false;
    const char current = text[text_index];
    if (glob[glob_index] == '?') {
      if (current == '/') return false;
      ++glob_index;
    } else if (glob[glob_index] == '[') {
      const std::size_t end = find_class_end(glob, glob_index);
      if (end == kNoClassEnd || current == '/' ||
          !class_matches(glob, glob_index, end, static_cast<unsigned char>(current))) {
        return false;
      }
      glob_index = end;
    } else {
      if (glob[glob_index] == '\\') ++glob_index;
      if (glob_index >= glob.size() || glob[glob_index] != current) return false;
      ++glob_index;
    }
    ++text_index;
  }
  return text_index == text.size();
}

Outcome<CapturePolicy> CapturePolicy::compile(std::string source) {
  if (source.find('\0') != std::string::npos) {
    return error(std::string(kCapturePolicyFileName) + ": contains a NUL byte");
  }
  if (!is_valid_utf8(source)) {
    return error(std::string(kCapturePolicyFileName) + ": is not valid UTF-8");
  }
  CapturePolicy policy;
  policy.present_ = true;
  std::size_t line_start = 0;
  int line_number = 0;
  while (line_start < source.size()) {
    std::size_t line_end = source.find('\n', line_start);
    if (line_end == std::string::npos) line_end = source.size();
    ++line_number;
    auto parsed =
        parse_pattern_line(source.substr(line_start, line_end - line_start), line_number);
    if (!parsed) {
      return error(std::string(kCapturePolicyFileName) + ":" + std::to_string(line_number) +
                   ": " + parsed.error());
    }
    if (*parsed) policy.patterns_.push_back(std::move(**parsed));
    line_start = line_end + 1;
  }
  policy.source_ = std::move(source);
  return policy;
}

Outcome<CapturePolicy> CapturePolicy::load_from_project(
    const std::filesystem::path& project_root) {
  const std::filesystem::path path = project_root / kCapturePolicyFileName;
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) return CapturePolicy{};
    return error("cannot inspect " + path.string());
  }
  if (!S_ISREG(st.st_mode)) return error(path.string() + " must be a regular file");
  std::ifstream in(path, std::ios::binary);
  if (!in) return error("cannot read " + path.string());
  std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return error("cannot read " + path.string());
  auto policy = compile(std::move(source));
  if (!policy) return std::unexpected(policy.error());
  policy->source_mode_ = st.st_mode & 07777;
  return policy;
}

CaptureDecision CapturePolicy::decide(std::string_view relative, bool is_directory) const {
  CaptureDecision decision;
  const std::string_view name = final_segment(relative);
  for (std::size_t index = 0; index < patterns_.size(); ++index) {
    const CapturePattern& pattern = patterns_[index];
    if (pattern.directory_only && !is_directory) continue;
    if (!match_capture_glob(pattern.glob, pattern.anchored ? relative : name)) continue;
    decision.excluded = !pattern.negated;
    decision.pattern_index = static_cast<int>(index);
  }
  return decision;
}

std::string CapturePolicy::describe_pattern(const CaptureDecision& decision) const {
  if (decision.pattern_index < 0) return "no pattern matches";
  const CapturePattern& pattern = patterns_[static_cast<std::size_t>(decision.pattern_index)];
  return std::string(kCapturePolicyFileName) + ":" + std::to_string(pattern.line) + ": " +
         pattern.text;
}

Outcome<CaptureExplanation> explain_capture_decision(const std::filesystem::path& project_root,
                                                     const CapturePolicy& policy,
                                                     std::string_view relative_path) {
  const std::string relative = normalize_relative(relative_path);
  if (relative.empty()) return error("the Project root itself is always captured");

  const std::string first_segment = relative.substr(0, relative.find('/'));
  if (first_segment == kGitDirName || first_segment == kTribiosDirName) {
    return CaptureExplanation{true, "Tribios rule: Git and Tribios metadata are never captured"};
  }
  if (relative == kCapturePolicyFileName) {
    return CaptureExplanation{false, "Tribios rule: the capture policy file is always captured"};
  }

  struct stat root_stat {};
  if (::lstat(project_root.c_str(), &root_stat) != 0) {
    return error("cannot inspect " + project_root.string());
  }
  // A capture never descends into an excluded directory, so the first excluded
  // ancestor decides for everything beneath it.
  for (std::size_t slash = relative.find('/'); slash != std::string::npos;
       slash = relative.find('/', slash + 1)) {
    const std::string ancestor = relative.substr(0, slash);
    struct stat ancestor_stat {};
    if (::lstat((project_root / ancestor).c_str(), &ancestor_stat) == 0) {
      if (S_ISLNK(ancestor_stat.st_mode)) {
        return CaptureExplanation{
            true, "Tribios rule: " + ancestor + " is a symlink and capture never follows it"};
      }
      if (ancestor_stat.st_dev != root_stat.st_dev) {
        return CaptureExplanation{true, "Tribios rule: " + ancestor + " is a nested mount"};
      }
    }
    const CaptureDecision decision = policy.decide(ancestor, true);
    if (decision.excluded) {
      return CaptureExplanation{
          true, policy.describe_pattern(decision) + ", matched parent directory " + ancestor};
    }
  }

  bool is_directory = relative_path.ends_with('/');
  struct stat st {};
  if (::lstat((project_root / relative).c_str(), &st) == 0) {
    if (st.st_dev != root_stat.st_dev) {
      return CaptureExplanation{true, "Tribios rule: nested mounts are never captured"};
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      return CaptureExplanation{true, "Tribios rule: special files are never captured"};
    }
    is_directory = S_ISDIR(st.st_mode);
  }
  const CaptureDecision decision = policy.decide(relative, is_directory);
  return CaptureExplanation{decision.excluded, policy.describe_pattern(decision)};
}

}  // namespace tribios
