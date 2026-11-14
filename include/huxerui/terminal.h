#pragma once

/// @file
/// @brief Embedded terminal sessions and the declarative HuxerUI terminal view.
///
/// Include <huxerui/terminal.h> and link HuxerUI::Terminal. Create a session
/// from an application hook or event handler and pass it to Terminal().

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

namespace huxerui::terminal {

namespace detail {
class Session;
struct Access;
} // namespace detail

/// Portable categories for terminal operation failures.
enum class TerminalErrorCode {
  Unsupported,          ///< The requested capability is unavailable on this platform.
  InvalidConfiguration, ///< A session option or command registration is invalid.
  StartFailed,          ///< A process or transport could not be started.
  IoFailed,             ///< An established session could not complete an I/O operation.
  Closed,               ///< The session or command output writer no longer accepts the operation.
};

/// An operation failure with a human-readable diagnostic.
struct TerminalError {
  TerminalErrorCode code; ///< Stable category for programmatic handling.
  std::string message;    ///< Diagnostic text for display or logging.
  /// Compares the category and diagnostic text.
  bool operator==(const TerminalError&) const = default;
};

/// An owned result of a terminal operation.
template <class T>
using TerminalResult = Result<T, TerminalError>;

/// Terminal grid dimensions in character cells.
struct TerminalSize {
  int columns = 80; ///< Number of cells per row.
  int rows = 24;    ///< Number of visible rows.
  /// Compares both grid dimensions.
  bool operator==(const TerminalSize&) const = default;
};

/// Limits and initial geometry shared by all session types.
struct TerminalSessionOptions {
  TerminalSize initial_size;                     ///< Grid size before an interactive view measures its viewport.
  std::size_t scrollback_lines = 10'000;          ///< Maximum retained history rows. Zero disables history.
  std::size_t pending_output_bytes = 256 * 1024;  ///< Maximum bytes in one pending output batch.
};

/// Lifecycle of the session as a whole.
enum class TerminalStatus {
  Open,    ///< The session accepts input.
  Closing, ///< Explicit cleanup is in progress.
  Exited,  ///< The peer ended and all accepted output has been rendered.
  Closed,  ///< Explicit cleanup completed.
  Failed,  ///< A fatal error ended the session.
};

/// How a byte-stream session ended.
enum class TerminalExitReason {
  Exited,       ///< The peer exited normally.
  Signaled,     ///< The system reported termination by a signal.
  Disconnected, ///< The transport ended without a process result.
};

/// Result retained after a byte-stream session ends.
struct TerminalExit {
  TerminalExitReason reason; ///< How the peer ended.
  std::optional<int> code;   ///< Exit code or signal number when the transport reports it.
  /// Compares both reason and optional code.
  bool operator==(const TerminalExit&) const = default;
};

/// An application command that currently owns its session's input.
struct TerminalCommandState {
  std::string name;        ///< Registered command name.
  bool cancelling = false; ///< Whether cooperative cancellation was requested.
  /// Compares the command name and cancellation state.
  bool operator==(const TerminalCommandState&) const = default;
};

/// Observable session metadata. Terminal screen updates are internal to the view.
struct TerminalSnapshot {
  TerminalStatus status = TerminalStatus::Open;        ///< Current session lifecycle.
  TerminalSize size;                                   ///< Current terminal grid dimensions.
  std::optional<TerminalCommandState> active_command;   ///< Present while an application command is running.
  std::optional<TerminalExit> exit;                     ///< Peer result, published after the last output.
  std::optional<TerminalError> error;                   ///< Retained fatal session failure, if any.
  /// Compares all published metadata.
  bool operator==(const TerminalSnapshot&) const = default;
};

/// Copyable handle to an application-owned session.
///
/// Copies share one session and may be used on its owning application thread.
/// The last owner releases the session. Use CloseAsync() to await cleanup.
class TerminalSession {
public:
  /// Reads session metadata and subscribes the current scope when called during composition.
  /// @return An owned copy of the latest published metadata.
  /// @throws std::logic_error If called from another thread.
  TerminalSnapshot Snapshot() const;

  /// Requests cooperative command cancellation or sends the terminal interrupt character.
  /// @return Whether the session accepted the request.
  /// @throws std::logic_error If called from another thread.
  bool Interrupt() const;

  /// Requests closure and awaits resource cleanup. Multiple callers may await the same close.
  /// @return The result of the shutdown, including any backend cleanup failure.
  /// @pre Await on the session's application thread. Command handlers must return after a stop request.
  Task<TerminalResult<void>> CloseAsync() const;

private:
  explicit TerminalSession(std::shared_ptr<detail::Session> session);
  std::shared_ptr<detail::Session> session_;
  friend struct detail::Access;
};

/// A bounded output writer valid for the duration of one application command.
class TerminalCommandOutput {
public:
  /// Appends UTF-8 text in publication order. Newlines advance the terminal prompt area.
  /// @param text Owned plain text. Terminal escape sequences have no control effect.
  /// @return Success after publishing the text, or an error if the writer has closed.
  Task<TerminalResult<void>> WriteAsync(std::string text) const;

private:
  explicit TerminalCommandOutput(std::weak_ptr<detail::Session> session, std::size_t command_id);
  std::weak_ptr<detail::Session> session_;
  std::size_t command_id_;
  friend struct detail::Access;
};

/// Inputs retained by an application command handler.
struct TerminalCommandContext {
  std::vector<std::string> arguments; ///< Parsed UTF-8 arguments excluding the command name.
  TerminalCommandOutput output;      ///< Plain-text stream for this invocation.
  std::stop_token stop_token;         ///< Cooperative cancellation request.
};

/// Application command registration used by dispatch, help, and completion.
struct TerminalCommand {
  std::string name; ///< Case-sensitive command name without whitespace.
  std::string help; ///< Short description for the built-in help command.
  /// Runs on the application task scope. Return zero for success or a nonzero command exit code.
  std::function<Task<int>(TerminalCommandContext)> handler;
};

/// Settings for a session that dispatches commands registered by the application.
struct TerminalCommandOptions {
  std::string prompt = "\xE2\x9D\xAF ";  ///< UTF-8 prompt rendered in the terminal grid.
  std::vector<TerminalCommand> commands;   ///< Command registrations. The names help and clear are reserved.
  std::size_t history_limit = 200;         ///< Maximum stored commands. Zero disables input history.
};

/// Launch configuration for a desktop shell or console program.
struct TerminalShellOptions {
  std::filesystem::path executable;                       ///< Executable path or name.
  std::vector<std::string> arguments;                      ///< UTF-8 arguments excluding the executable name.
  std::optional<std::filesystem::path> working_directory;  ///< Child working directory, if specified.
  std::map<std::string, std::string> environment;          ///< UTF-8 overrides for the inherited environment.
};

/// One nonempty chunk of owned output bytes or the result after the last chunk.
using TerminalRead = std::variant<Bytes, TerminalExit>;

/// Asynchronous bidirectional byte transport owned by one session.
///
/// The session may read concurrently with a write or resize, with one operation
/// outstanding on each side. CloseAsync() must settle pending operations.
class TerminalBackend {
public:
  /// Releases the backend. Implementations must also support cleanup at application shutdown.
  virtual ~TerminalBackend() = default;

  /// Reads up to maximum_bytes or returns the peer result once output is drained.
  /// @param maximum_bytes Positive upper bound for the returned nonempty chunk.
  virtual Task<TerminalResult<TerminalRead>> ReadAsync(std::size_t maximum_bytes) = 0;

  /// Sends all bytes in order and owns the input through completion.
  /// @param bytes Byte payload to send to the peer.
  virtual Task<TerminalResult<void>> WriteAsync(Bytes bytes) = 0;

  /// Updates the peer's grid size. Calls are serialized with writes.
  /// @param size Positive terminal grid dimensions.
  virtual Task<TerminalResult<void>> ResizeAsync(TerminalSize size) = 0;

  /// Closes the transport and releases resources even with pending I/O.
  virtual Task<TerminalResult<void>> CloseAsync() = 0;
};

/// Appearance of the mounted terminal view.
/// Unset color entries follow the surrounding HuxerUI theme.
struct TerminalStyle {
  Font font = Font::Monospace();   ///< Monospaced font request in logical units.
  float line_height_scale = 1.2F;  ///< Positive multiplier for measured line height.
  std::optional<Color> foreground; ///< Default cell foreground.
  std::optional<Color> background; ///< Default cell background.
  std::optional<Color> cursor;     ///< Terminal cursor color.
  std::optional<Color> selection;  ///< Fill of the selected cell range.
  /// Replaces all 16 ANSI colors in index order when set.
  std::optional<std::array<Color, 16>> ansi;
};

/// Typed events emitted by the mounted terminal.
struct TerminalEvents {
  /// Reports the visible HTTP(S) link activated with Control-click or Command-click.
  /// The application owns navigation. Copy the destination inside the handler to retain it.
  struct LinkActivated : Event<void(const std::string&)> {};
};

/// Creates a command session with its first prompt ready to display.
/// @param commands Prompt, registrations, and history settings.
/// @param options Initial geometry and output limits.
/// @return The session or a configuration error.
/// @pre Call on the application thread outside composition.
TerminalResult<TerminalSession> CreateCommandSession(
    TerminalCommandOptions commands, TerminalSessionOptions options = {});

/// Opens the platform pseudoterminal and starts a desktop shell or console program.
/// @param shell Executable, arguments, working directory, and environment overrides.
/// @param options Initial geometry and output limits.
/// @return The open session or a portable failure. Mobile and Web return Unsupported.
Task<TerminalResult<TerminalSession>> OpenShellAsync(
    TerminalShellOptions shell, TerminalSessionOptions options = {});

/// Attaches an application-provided byte transport and applies the initial terminal size.
/// @param backend Open transport whose ownership is transferred to the session.
/// @param options Initial geometry and output limits.
/// @return The attached session or the configuration or initial resize failure.
Task<TerminalResult<TerminalSession>> OpenTransportAsync(
    std::unique_ptr<TerminalBackend> backend, TerminalSessionOptions options = {});

/// Declares one focusable terminal grid with inline keyboard and input-method support.
/// @param session Retained session. At most one mounted Terminal may interact with it at a time.
/// @param style Font, line height, and optional color overrides for the terminal view.
/// @return A View that can be laid out and bound to a ViewHandle using ordinary modifiers.
/// @throws std::invalid_argument If style.line_height_scale is nonfinite or nonpositive.
/// @throws std::logic_error If a second interactive view is mounted against the same session.
View Terminal(TerminalSession session, TerminalStyle style = {});

/// Application hook retained for compatibility with the generated project.
/// @param context Owning application context.
void Install(ApplicationContext& context);

} // namespace huxerui::terminal
