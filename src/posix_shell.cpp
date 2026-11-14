#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)

#include "session.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

extern char** environ;

namespace huxerui::terminal::detail {
namespace {

using namespace std::chrono_literals;

struct Descriptor {
  int value = -1;
  ~Descriptor() { if (value >= 0) close(value); }
  Descriptor() = default;
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
};

winsize Size(TerminalSize size) {
  return {.ws_row = static_cast<unsigned short>(size.rows), .ws_col = static_cast<unsigned short>(size.columns)};
}

TerminalError SystemError(TerminalErrorCode code, std::string operation, int error = errno) {
  return Error(code, std::move(operation) + ": " + std::strerror(error));
}

struct Process {
  Descriptor master;
  pid_t child = -1;
  std::atomic<bool> closing = false;
  std::mutex child_mutex;
  std::optional<TerminalExit> exit;

  ~Process() { Shutdown(); }

  std::optional<TerminalExit> Reap(bool wait) {
    std::lock_guard lock(child_mutex);
    if (child <= 0) return exit;
    int status = 0;
    pid_t result;
    do {
      result = waitpid(child, &status, wait ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) return {};
    child = -1;
    if (result > 0 && WIFEXITED(status)) exit = TerminalExit{TerminalExitReason::Exited, WEXITSTATUS(status)};
    else if (result > 0 && WIFSIGNALED(status)) exit = TerminalExit{TerminalExitReason::Signaled, WTERMSIG(status)};
    else exit = TerminalExit{TerminalExitReason::Disconnected, std::nullopt};
    return exit;
  }

  void Shutdown() {
    if (closing.exchange(true)) return;
    if (master.value >= 0) {
      const auto foreground = tcgetpgrp(master.value);
      if (foreground > 0 && foreground != getpgrp()) kill(-foreground, SIGHUP);
    }
    {
      std::lock_guard lock(child_mutex);
      if (child > 0) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
      }
    }
    Reap(true);
  }
};

class ShellBackend final : public TerminalBackend {
public:
  explicit ShellBackend(std::shared_ptr<Process> process) : process_(std::move(process)) {}

  Task<TerminalResult<TerminalRead>> ReadAsync(std::size_t maximum_bytes) override {
    using Result = TerminalResult<TerminalRead>;
    Bytes bytes(maximum_bytes);
    while (!process_->closing) {
      const auto count = read(process_->master.value, bytes.data(), bytes.size());
      if (count > 0) {
        bytes.resize(static_cast<std::size_t>(count));
        co_return Result::Success(std::move(bytes));
      }
      const int error = errno;
      if (count == 0 || error == EIO || error == EAGAIN || error == EWOULDBLOCK) {
        if (auto exit = process_->Reap(false)) co_return Result::Success(*exit);
      } else if (error != EINTR) {
        co_return Result::Failure(SystemError(TerminalErrorCode::IoFailed, "Could not read shell output", error));
      }
      co_await Delay(8ms);
    }
    co_return Result::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
  }

  Task<TerminalResult<void>> WriteAsync(Bytes bytes) override {
    using Result = TerminalResult<void>;
    std::size_t offset = 0;
    while (offset < bytes.size() && !process_->closing) {
      const auto count = write(process_->master.value, bytes.data() + offset, bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
      } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        co_return Result::Failure(SystemError(TerminalErrorCode::IoFailed, "Could not write shell input"));
      } else {
        co_await Delay(8ms);
      }
    }
    co_return offset == bytes.size() ? Result::Success()
                                     : Result::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
  }

  Task<TerminalResult<void>> ResizeAsync(TerminalSize size) override {
    auto dimensions = Size(size);
    if (process_->closing) co_return TerminalResult<void>::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
    if (ioctl(process_->master.value, TIOCSWINSZ, &dimensions) < 0) {
      co_return TerminalResult<void>::Failure(SystemError(TerminalErrorCode::IoFailed, "Could not resize the pseudoterminal"));
    }
    co_return TerminalResult<void>::Success();
  }

  Task<TerminalResult<void>> CloseAsync() override {
    return RunWorker([process = process_] {
      process->Shutdown();
      return TerminalResult<void>::Success();
    });
  }

private:
  std::shared_ptr<Process> process_;
};

std::vector<std::string> ExecutablePaths(std::string executable, const std::map<std::string, std::string>& environment) {
  if (executable.find('/') != std::string::npos) return {std::move(executable)};
  const auto path = environment.find("PATH");
  std::string_view directories = path == environment.end() ? "/usr/bin:/bin" : path->second;
  std::vector<std::string> candidates;
  for (;;) {
    const auto end = directories.find(':');
    const auto directory = directories.substr(0, end);
    candidates.push_back(directory.empty() ? executable : std::string(directory) + '/' + executable);
    if (end == std::string_view::npos) return candidates;
    directories.remove_prefix(end + 1);
  }
}

TerminalResult<std::unique_ptr<TerminalBackend>> Start(TerminalShellOptions shell, TerminalSize size) {
  using Result = TerminalResult<std::unique_ptr<TerminalBackend>>;
  std::map<std::string, std::string> environment;
  for (char** item = environ; item && *item; ++item) {
    const std::string_view entry(*item);
    if (const auto equal = entry.find('='); equal != std::string_view::npos) {
      environment.emplace(entry.substr(0, equal), entry.substr(equal + 1));
    }
  }
  environment.insert_or_assign("TERM", "xterm-256color");
  for (auto& [name, value] : shell.environment) environment.insert_or_assign(name, std::move(value));
  const auto executable = shell.executable.string();
  const auto paths = ExecutablePaths(executable, environment);
  const auto directory = shell.working_directory ? shell.working_directory->string() : std::string{};
  std::vector<char*> args;
  args.push_back(const_cast<char*>(executable.c_str()));
  for (auto& argument : shell.arguments) args.push_back(argument.data());
  args.push_back(nullptr);
  std::vector<std::string> variables;
  for (const auto& [name, value] : environment) variables.push_back(name + '=' + value);
  std::vector<char*> env;
  for (auto& variable : variables) env.push_back(variable.data());
  env.push_back(nullptr);

  Descriptor error_read;
  Descriptor error_write;
  int errors[2];
#ifdef __APPLE__
  if (pipe(errors) != 0) {
#else
  if (pipe2(errors, O_CLOEXEC) != 0) {
#endif
    return Result::Failure(SystemError(TerminalErrorCode::StartFailed, "Could not create shell status pipe"));
  }
  error_read.value = errors[0];
  error_write.value = errors[1];
#ifdef __APPLE__
  if (fcntl(error_read.value, F_SETFD, FD_CLOEXEC) < 0 || fcntl(error_write.value, F_SETFD, FD_CLOEXEC) < 0) {
    return Result::Failure(SystemError(TerminalErrorCode::StartFailed, "Could not configure shell status pipe"));
  }
#endif

  auto process = std::make_shared<Process>();
  auto dimensions = Size(size);
  const auto child = forkpty(&process->master.value, nullptr, nullptr, &dimensions);
  if (child == 0) {
    close(error_read.value);
    int failure = 0;
    if (!directory.empty() && chdir(directory.c_str()) != 0) {
      failure = errno;
    } else {
      for (const auto& path : paths) {
        execve(path.c_str(), args.data(), env.data());
        if (!failure || errno != ENOENT) failure = errno;
      }
    }
    const auto ignored = write(error_write.value, &failure, sizeof(failure));
    (void)ignored;
    _exit(127);
  }
  if (child < 0) return Result::Failure(SystemError(TerminalErrorCode::StartFailed, "Could not open pseudoterminal"));
  process->child = child;
  close(std::exchange(error_write.value, -1));
  int failure = 0;
  ssize_t count;
  do {
    count = read(error_read.value, &failure, sizeof(failure));
  } while (count < 0 && errno == EINTR);
  if (count != 0) {
    return Result::Failure(SystemError(TerminalErrorCode::StartFailed, "Could not start shell", count > 0 ? failure : errno));
  }
  if (fcntl(process->master.value, F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(process->master.value, F_SETFL, O_NONBLOCK) < 0) {
    return Result::Failure(SystemError(TerminalErrorCode::StartFailed, "Could not configure pseudoterminal"));
  }
  return Result::Success(std::make_unique<ShellBackend>(std::move(process)));
}

} // namespace

Task<TerminalResult<std::unique_ptr<TerminalBackend>>> StartShell(TerminalShellOptions shell, TerminalSize size) {
  return RunWorker(Start, std::move(shell), size);
}

} // namespace huxerui::terminal::detail

#endif
