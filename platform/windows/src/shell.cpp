#include "../../../src/session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <system_error>

namespace huxerui::terminal::detail {
namespace {

using namespace std::chrono_literals;

class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE handle) : handle_(handle) {}
  Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept {
    Reset();
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }
  ~Handle() { Reset(); }
  HANDLE Get() const { return handle_; }
  void Reset() {
    if (handle_) CloseHandle(std::exchange(handle_, nullptr));
  }
private:
  HANDLE handle_ = nullptr;
};

std::pair<Handle, Handle> Pipe() {
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!CreatePipe(&read, &write, nullptr, 0)) {
    throw std::system_error(GetLastError(), std::system_category(), "CreatePipe");
  }
  return {Handle(read), Handle(write)};
}

std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (!length) throw std::invalid_argument("Invalid UTF-8 in shell configuration");
  std::wstring result(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::wstring Quote(std::wstring_view argument) {
  std::wstring result(1, L'"');
  std::size_t slashes = 0;
  for (wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    result += character;
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

std::wstring Environment(const TerminalShellOptions& options) {
  if (options.environment.empty()) return {};
  const auto less = [](const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  };
  std::map<std::wstring, std::wstring, decltype(less)> values(less);
  if (auto* environment = GetEnvironmentStringsW()) {
    auto release = [](wchar_t* block) { FreeEnvironmentStringsW(block); };
    std::unique_ptr<wchar_t, decltype(release)> block(environment, release);
    for (const wchar_t* item = block.get(); *item; item += std::wcslen(item) + 1) {
      const std::wstring_view entry(item);
      const auto equal = entry.find(L'=', 1);
      if (equal != std::wstring_view::npos) {
        values.emplace(entry.substr(0, equal), entry.substr(equal + 1));
      }
    }
  }
  for (const auto& [name, value] : options.environment) {
    if (name.empty() || name.find('=') != std::string::npos) throw std::invalid_argument("Invalid environment name");
    values.insert_or_assign(Wide(name), Wide(value));
  }
  std::wstring block;
  for (const auto& [name, value] : values) {
    block += name + L'=' + value;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

class Pseudoconsole {
public:
  using Create = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
  using Resize = HRESULT (WINAPI*)(HPCON, COORD);
  using Close = void (WINAPI*)(HPCON);

  Pseudoconsole() {
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    create = reinterpret_cast<Create>(GetProcAddress(kernel, "CreatePseudoConsole"));
    resize = reinterpret_cast<Resize>(GetProcAddress(kernel, "ResizePseudoConsole"));
    close = reinterpret_cast<Close>(GetProcAddress(kernel, "ClosePseudoConsole"));
  }

  Create create;
  Resize resize;
  Close close;
};

struct Process {
  Pseudoconsole api;
  HPCON console = nullptr;
  Handle input;
  Handle output;
  Handle child;
  std::atomic<bool> closing = false;
  std::mutex console_mutex;
  std::mutex output_mutex;

  ~Process() { Shutdown(); }

  void Shutdown() {
    closing = true;
    std::lock_guard lock(console_mutex);
    if (!console) return;
    if (child.Get() && WaitForSingleObject(child.Get(), 0) == WAIT_TIMEOUT) TerminateProcess(child.Get(), 1);
    {
      // Wait for a started peek/read pair before the shutdown drain owns the pipe.
      std::lock_guard lock(output_mutex);
    }
    // Earlier ConPTY versions can block in ClosePseudoConsole until the output pipe is drained.
    std::jthread drain([this] {
      char bytes[4096];
      DWORD count = 0;
      while (ReadFile(output.Get(), bytes, sizeof(bytes), &count, nullptr) && count) {}
    });
    api.close(std::exchange(console, nullptr));
    drain.join();
    if (child.Get()) WaitForSingleObject(child.Get(), INFINITE);
  }
};

COORD Size(TerminalSize size) {
  return {static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows)};
}

class ShellBackend final : public TerminalBackend {
public:
  explicit ShellBackend(std::shared_ptr<Process> process) : process_(std::move(process)) {}

  Task<TerminalResult<TerminalRead>> ReadAsync(std::size_t maximum_bytes) override {
    using Result = TerminalResult<TerminalRead>;
    bool exited_without_output = false;
    while (!process_->closing) {
      {
        std::lock_guard lock(process_->output_mutex);
        if (process_->closing) break;
        DWORD available = 0;
        const bool exited = WaitForSingleObject(process_->child.Get(), 0) == WAIT_OBJECT_0;
        if (!PeekNamedPipe(process_->output.Get(), nullptr, 0, nullptr, &available, nullptr)) {
          if (GetLastError() != ERROR_BROKEN_PIPE) {
            co_return Result::Failure(Error(TerminalErrorCode::IoFailed, "Could not inspect the shell output pipe"));
          }
          if (exited) co_return Exit();
        } else if (available) {
          Bytes bytes(std::min<std::size_t>(maximum_bytes, available));
          DWORD count = 0;
          if (!ReadFile(process_->output.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            co_return Result::Failure(Error(TerminalErrorCode::IoFailed, "Could not read shell output"));
          }
          if (count) {
            bytes.resize(count);
            co_return Result::Success(std::move(bytes));
          }
        } else if (exited && exited_without_output) {
          co_return Exit();
        }
        exited_without_output = exited;
      }
      // The child can exit before ConPTY publishes the final frame.
      co_await Delay(exited_without_output ? 20ms : 8ms);
    }
    co_return Result::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
  }

  Task<TerminalResult<void>> WriteAsync(Bytes bytes) override {
    return RunWorker([process = process_, bytes = std::move(bytes)] {
      using Result = TerminalResult<void>;
      std::size_t offset = 0;
      while (offset < bytes.size() && !process->closing) {
        DWORD count = 0;
        if (!WriteFile(process->input.Get(), bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset),
                       &count, nullptr) || !count) {
          return Result::Failure(Error(TerminalErrorCode::IoFailed, "Could not write shell input"));
        }
        offset += count;
      }
      return offset == bytes.size() ? Result::Success()
                                    : Result::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
    });
  }

  Task<TerminalResult<void>> ResizeAsync(TerminalSize size) override {
    return RunWorker([process = process_, size] {
      std::lock_guard lock(process->console_mutex);
      if (process->closing) return TerminalResult<void>::Failure(Error(TerminalErrorCode::Closed, "Shell is closed"));
      if (FAILED(process->api.resize(process->console, Size(size)))) {
        return TerminalResult<void>::Failure(Error(TerminalErrorCode::IoFailed, "Could not resize the pseudoconsole"));
      }
      return TerminalResult<void>::Success();
    });
  }

  Task<TerminalResult<void>> CloseAsync() override {
    return RunWorker([process = process_] {
      process->Shutdown();
      return TerminalResult<void>::Success();
    });
  }

private:
  TerminalResult<TerminalRead> Exit() const {
    DWORD code = 0;
    if (!GetExitCodeProcess(process_->child.Get(), &code)) {
      return TerminalResult<TerminalRead>::Failure(Error(TerminalErrorCode::IoFailed, "Could not read shell exit code"));
    }
    return TerminalResult<TerminalRead>::Success(TerminalExit{TerminalExitReason::Exited, static_cast<int>(code)});
  }

  std::shared_ptr<Process> process_;
};

TerminalResult<std::unique_ptr<TerminalBackend>> Start(TerminalShellOptions shell, TerminalSize size) {
  using Result = TerminalResult<std::unique_ptr<TerminalBackend>>;
  try {
    auto process = std::make_shared<Process>();
    if (!process->api.create || !process->api.resize || !process->api.close) {
      return Result::Failure(Error(TerminalErrorCode::Unsupported, "ConPTY is unavailable on this Windows version"));
    }
    auto executable = shell.executable.wstring();
    std::wstring command = Quote(executable);
    for (const auto& argument : shell.arguments) command += L' ' + Quote(Wide(argument));
    const auto directory = shell.working_directory ? shell.working_directory->wstring() : std::wstring{};
    auto environment = Environment(shell);
    auto [console_input, input] = Pipe();
    auto [output, console_output] = Pipe();
    process->input = std::move(input);
    process->output = std::move(output);
    const HRESULT opened = process->api.create(Size(size), console_input.Get(), console_output.Get(), 0, &process->console);
    if (FAILED(opened)) throw std::system_error(static_cast<int>(opened), std::system_category(), "CreatePseudoConsole");
    console_input.Reset();
    console_output.Reset();

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    Bytes storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
      throw std::system_error(GetLastError(), std::system_category(), "InitializeProcThreadAttributeList");
    }
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)>
        cleanup(attributes, DeleteProcThreadAttributeList);
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   process->console, sizeof(HPCON), nullptr, nullptr)) {
      throw std::system_error(GetLastError(), std::system_category(), "UpdateProcThreadAttribute");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Leave standard handles null so redirected handles in the host do not bypass the pseudoconsole.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment.empty() ? nullptr : environment.data(),
                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &child)) {
      throw std::system_error(GetLastError(), std::system_category(), "CreateProcessW");
    }
    process->child = Handle(child.hProcess);
    CloseHandle(child.hThread);
    return Result::Success(std::make_unique<ShellBackend>(std::move(process)));
  } catch (const std::invalid_argument& error) {
    return Result::Failure(Error(TerminalErrorCode::InvalidConfiguration, error.what()));
  } catch (const std::system_error& error) {
    return Result::Failure(Error(TerminalErrorCode::StartFailed, error.what()));
  }
}

} // namespace

Task<TerminalResult<std::unique_ptr<TerminalBackend>>> StartShell(TerminalShellOptions shell, TerminalSize size) {
  return RunWorker(Start, std::move(shell), size);
}

} // namespace huxerui::terminal::detail
