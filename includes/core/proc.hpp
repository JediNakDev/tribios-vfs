#pragma once

#include <string>
#include <vector>

namespace tribios {

struct ProcessResult {
  int exit_code = -1;
  std::string output;  // stdout and stderr combined
  bool ok() const { return exit_code == 0; }
};

// Runs a command without a shell and captures its combined output.
ProcessResult run_process_and_capture_output(const std::vector<std::string>& arguments);

}  // namespace tribios
