#include "core/proc.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>

extern char** environ;

namespace tribios {

ProcessResult run_process_and_capture_output(const std::vector<std::string>& arguments) {
  ProcessResult result;
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    result.output = "pipe failed";
    return result;
  }

  std::vector<char*> raw;
  raw.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) {
    raw.push_back(const_cast<char*>(argument.c_str()));
  }
  raw.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);

  pid_t pid = 0;
  int spawned = posix_spawnp(&pid, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(pipefd[1]);
  if (spawned != 0) {
    ::close(pipefd[0]);
    result.output = "failed to spawn " + arguments.front();
    return result;
  }

  std::array<char, 4096> buffer{};
  ssize_t n = 0;
  while ((n = ::read(pipefd[0], buffer.data(), buffer.size())) > 0) {
    result.output.append(buffer.data(), static_cast<size_t>(n));
  }
  ::close(pipefd[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

}  // namespace tribios
