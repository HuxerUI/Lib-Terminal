#include "session.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if !defined(_WIN32) && !(defined(__linux__) && !defined(__ANDROID__)) && !(defined(__APPLE__) && TARGET_OS_OSX)
namespace huxerui::terminal::detail {

Task<TerminalResult<std::unique_ptr<TerminalBackend>>> StartShell(TerminalShellOptions, TerminalSize) {
  co_return TerminalResult<std::unique_ptr<TerminalBackend>>::Failure(
      Error(TerminalErrorCode::Unsupported, "Local shell sessions are unavailable on this platform"));
}

} // namespace huxerui::terminal::detail
#endif
