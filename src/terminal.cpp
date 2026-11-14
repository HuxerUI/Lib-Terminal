#include "session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace huxerui::terminal::detail {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t MaxInputBytes = 64 * 1024;

const VTermScreenCallbacks ScreenCallbacks{
    .settermprop = [](VTermProp property, VTermValue* value, void* user) {
      auto& session = *static_cast<Session*>(user);
      switch (property) {
      case VTERM_PROP_ALTSCREEN:
        if (session.alternate != (value->boolean != 0)) {
          session.alternate = value->boolean != 0;
          if (session.alternate) session.main_viewport_top = std::exchange(session.viewport_top, {});
          else session.viewport_top = std::exchange(session.main_viewport_top, {});
        }
        break;
      case VTERM_PROP_CURSORVISIBLE: session.cursor_visible = value->boolean != 0; break;
      case VTERM_PROP_CURSORSHAPE: session.cursor_shape = value->number; break;
      case VTERM_PROP_MOUSE: session.mouse_mode = value->number; break;
      default: break;
      }
      return 1;
    },
    .sb_pushline = [](int cols, const VTermScreenCell* cells, bool continuation, void* user) {
      auto& session = *static_cast<Session*>(user);
      if (session.options.scrollback_lines == 0) {
        ++session.history_base;
        return 1;
      }
      session.history.push_back({{cells, cells + cols}, continuation});
      if (session.history.size() > session.options.scrollback_lines) {
        session.history.pop_front();
        ++session.history_base;
      }
      return 1;
    },
    .sb_popline = [](int cols, VTermScreenCell* cells, bool* continuation, void* user) {
      auto& session = *static_cast<Session*>(user);
      if (session.history.empty()) return 0;
      const auto& entry = session.history.back();
      const auto& row = entry.cells;
      const auto count = std::min(cols, static_cast<int>(row.size()));
      std::copy_n(row.data(), count, cells);
      for (int col = count; col < cols; ++col) {
        cells[col] = {};
        cells[col].width = 1;
        vterm_state_get_default_colors(session.state, &cells[col].fg, &cells[col].bg);
      }
      if (count && cells[count - 1].width == 2) cells[count - 1].width = 1;
      *continuation = entry.continuation;
      session.history.pop_back();
      return 1;
    },
    .sb_clear = [](void* user) {
      auto& session = *static_cast<Session*>(user);
      session.history_base += static_cast<std::int64_t>(session.history.size());
      session.history.clear();
      session.viewport_top.reset();
      session.main_viewport_top.reset();
      return 1;
    },
};

std::optional<std::vector<std::string>> Split(std::string_view line) {
  std::vector<std::string> args;
  std::string word;
  char quote = '\0';
  bool started = false;
  bool escaped = false;
  for (char character : line) {
    if (escaped) {
      word += character;
      escaped = false;
    } else if (character == '\\' && quote != '\'') {
      escaped = true;
      started = true;
    } else if (quote) {
      if (character == quote) quote = '\0';
      else word += character;
    } else if (character == '\'' || character == '"') {
      quote = character;
      started = true;
    } else if (character == ' ' || character == '\t') {
      if (started) args.push_back(std::exchange(word, {}));
      started = false;
    } else {
      word += character;
      started = true;
    }
  }
  if (quote || escaped) return {};
  if (started) args.push_back(std::move(word));
  return args;
}

Task<void> ReadLoop(std::weak_ptr<Session> weak, std::shared_ptr<TerminalBackend> backend,
                    std::size_t limit) {
  try {
    for (;;) {
      auto result = co_await backend->ReadAsync(limit);
      auto session = weak.lock();
      if (!session || !session->IsOpen()) co_return;
      if (!result.Succeeded()) {
        session->Fail(std::move(result).Error());
        co_return;
      }
      if (const auto* exit = std::get_if<TerminalExit>(&result.Value())) {
        session->snapshot.exit = *exit;
        session->StartClose(TerminalStatus::Exited);
        co_return;
      }
      const auto& bytes = std::get<Bytes>(result.Value());
      if (bytes.empty() || bytes.size() > limit) {
        session->Fail(Error(TerminalErrorCode::IoFailed, "Transport returned an invalid output chunk"));
        co_return;
      }
      session->Feed({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
      session.reset();
      co_await Delay(1ms);
    }
  } catch (const std::exception& error) {
    if (auto session = weak.lock(); session && session->IsOpen()) {
      session->Fail(Error(TerminalErrorCode::IoFailed, error.what()));
    }
  }
}

Task<void> WriteLoop(std::weak_ptr<Session> weak, std::shared_ptr<TerminalBackend> backend) {
  try {
    for (;;) {
      auto session = weak.lock();
      if (!session || !session->IsOpen()) co_return;
      if (session->outbound.empty()) {
        session->writing = false;
        co_return;
      }
      auto payload = std::move(session->outbound.front());
      session->outbound.pop_front();
      auto* bytes = std::get_if<Bytes>(&payload);
      const auto size = bytes ? bytes->size() : 0;
      auto task = bytes ? backend->WriteAsync(std::move(*bytes))
                        : backend->ResizeAsync(std::get<TerminalSize>(payload));
      session.reset();
      auto result = co_await std::move(task);
      session = weak.lock();
      if (!session || !session->IsOpen()) co_return;
      session->outbound_bytes -= size;
      if (!result.Succeeded()) {
        session->Fail(std::move(result).Error());
        co_return;
      }
    }
  } catch (const std::exception& error) {
    if (auto session = weak.lock(); session && session->IsOpen()) {
      session->Fail(Error(TerminalErrorCode::IoFailed, error.what()));
    }
  }
}

void StartWriter(Session& session) {
  if (!session.writing && session.backend && session.IsOpen()) {
    session.writing = true;
    session.writer = session.tasks.Launch(WriteLoop(session.weak_from_this(), session.backend));
  }
}

Task<void> CloseBackend(std::shared_ptr<Session> session, TerminalStatus outcome) {
  try {
    if (session->backend) session->close_result = co_await session->backend->CloseAsync();
    else {
      while (session->snapshot.active_command) co_await Delay(5ms);
      session->close_result = TerminalResult<void>::Success();
    }
  } catch (const std::exception& error) {
    session->close_result = TerminalResult<void>::Failure(Error(TerminalErrorCode::IoFailed, error.what()));
  } catch (...) {
    session->close_result = TerminalResult<void>::Failure(Error(TerminalErrorCode::IoFailed, "Transport close failed"));
  }
  session->reader.Cancel();
  session->writer.Cancel();
  session->backend.reset();
  if (!session->close_result->Succeeded()) {
    session->snapshot.error = session->close_result->Error();
    outcome = TerminalStatus::Failed;
  }
  session->snapshot.status = outcome;
  session->Changed();
  for (const auto& waiter : std::exchange(session->close_waiters, {})) waiter.Complete(*session->close_result);
}

Task<TerminalResult<void>> WaitForClose(std::shared_ptr<Session> session) {
  session->CheckThread();
  session->StartClose();
  if (session->close_result) co_return *session->close_result;
  co_return co_await AwaitCallback<TerminalResult<void>>(
      [owner = session.get()](CallbackCompletion<TerminalResult<void>> complete) {
        if (owner->close_result) complete.Complete(*owner->close_result);
        else owner->close_waiters.push_back(std::move(complete));
        return std::function<void()>{};
      });
}

Task<void> RunCommand(std::weak_ptr<Session> weak, TerminalCommand command, TerminalCommandContext context) {
  int code = 1;
  std::string message;
  try {
    code = co_await command.handler(std::move(context));
  } catch (const std::exception& error) {
    message = error.what();
  } catch (...) {
    message = "Command failed";
  }
  auto session = weak.lock();
  if (!session) co_return;
  if (session->IsOpen()) {
    if (session->Cursor().col != 0) session->Feed("\r\n");
    if (session->command_stop.stop_requested()) session->Print("^C\n");
    else if (!message.empty()) session->Print(message + "\n");
    else if (code != 0) session->Print("Exit code " + std::to_string(code) + "\n");
  }
  session->snapshot.active_command.reset();
  session->Changed();
}

Task<TerminalResult<void>> WriteCommand(std::weak_ptr<Session> weak, std::size_t id, std::string text) {
  using Result = TerminalResult<void>;
  auto session = weak.lock();
  if (!session || session->command_id != id || !session->snapshot.active_command ||
      !session->IsOpen() || session->command_stop.stop_requested()) {
    co_return Result::Failure(Error(TerminalErrorCode::Closed, "Command output is closed"));
  }
  session->CheckThread();
  if (!Clusters(text)) {
    co_return Result::Failure(Error(TerminalErrorCode::InvalidConfiguration, "Command output is not valid UTF-8"));
  }
  const auto chunk_size = session->options.pending_output_bytes;
  for (std::size_t offset = 0; offset < text.size(); offset += chunk_size) {
    session->Print(std::string_view(text).substr(offset, chunk_size));
    if (text.size() - offset > chunk_size) {
      session.reset();
      co_await Delay(1ms);
      session = weak.lock();
      if (!session || session->command_id != id || !session->snapshot.active_command ||
          !session->IsOpen() || session->command_stop.stop_requested()) {
        co_return Result::Failure(Error(TerminalErrorCode::Closed, "Command output is closed"));
      }
    }
  }
  co_return Result::Success();
}

VTermColor ToVTerm(Color color) {
  auto channel = [](float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
  };
  VTermColor result;
  vterm_color_rgb(&result, channel(color.red), channel(color.green), channel(color.blue));
  return result;
}

bool ValidOptions(const TerminalSessionOptions& options) {
  return ValidSize(options.initial_size) && options.pending_output_bytes > 0 &&
      options.pending_output_bytes <= 16 * 1024 * 1024;
}

bool ValidShell(const TerminalShellOptions& shell) {
  const auto valid_text = [](std::string_view text) {
    return text.find('\0') == std::string_view::npos && Clusters(text).has_value();
  };
  const auto valid_path = [](const std::filesystem::path& path) {
    const auto& value = path.native();
    return value.find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
  };
  return !shell.executable.empty() && valid_path(shell.executable) &&
      (!shell.working_directory || valid_path(*shell.working_directory)) &&
      std::ranges::all_of(shell.arguments, valid_text) &&
      std::ranges::all_of(shell.environment, [&](const auto& entry) {
        return !entry.first.empty() && entry.first.find('=') == std::string::npos &&
            valid_text(entry.first) && valid_text(entry.second);
      });
}

bool PlainLine(std::string_view text) {
  return Clusters(text) && std::ranges::none_of(text, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

} // namespace

TerminalError Error(TerminalErrorCode code, std::string message) { return {code, std::move(message)}; }

bool ValidSize(TerminalSize size) {
  return size.rows > 0 && size.columns > 0 && size.rows <= 1000 && size.columns <= 1000;
}

VTermModifier Modifiers(KeyModifiers mods) {
  return static_cast<VTermModifier>((mods.shift ? VTERM_MOD_SHIFT : 0) |
      (mods.alt ? VTERM_MOD_ALT : 0) | (mods.control ? VTERM_MOD_CTRL : 0));
}

Session::Session(TerminalSessionOptions settings)
    : tasks(UseApplicationTaskScope()), options(settings),
      observed(TerminalSnapshot{.size = settings.initial_size}),
      snapshot{.size = settings.initial_size}, requested_size(settings.initial_size) {
  terminal.reset(vterm_new(snapshot.size.rows, snapshot.size.columns));
  state = vterm_obtain_state(terminal.get());
  screen = vterm_obtain_screen(terminal.get());
  vterm_set_utf8(terminal.get(), 1);
  vterm_screen_set_callbacks(screen, &ScreenCallbacks, this);
  vterm_screen_enable_altscreen(screen, 1);
  vterm_screen_enable_reflow(screen, true);
  vterm_screen_reset(screen, 1);
  vterm_output_set_callback(terminal.get(), [](const char* bytes, std::size_t length, void* user) {
    static_cast<Session*>(user)->Queue({bytes, length});
  }, this);
}

Session::~Session() {
  reader.Cancel();
  writer.Cancel();
  command_stop.request_stop();
  command_task.Cancel();
  if (backend) {
    try {
      tasks.Launch([transport = backend]() -> Task<void> { (void)co_await transport->CloseAsync(); });
    } catch (const std::logic_error&) {
      // The application is shutting down. The transport destructor releases its resources.
    }
  }
}

void Session::CheckThread() const {
  if (thread != std::this_thread::get_id()) throw std::logic_error("Terminal session belongs to another thread");
}

void Session::Changed() {
  observed = snapshot;
  if (update_view) update_view();
}

void Session::Feed(std::string_view text) {
  if (!text.empty()) vterm_input_write(terminal.get(), text.data(), text.size());
  if (update_view) update_view();
}

void Session::Print(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  for (const unsigned char character : text) {
    if (character == '\n') output += "\r\n";
    else if (character == '\t') output += "    ";
    else if (character == '\x1b') output += "^[";
    else if (character >= 0x20 && character != 0x7f) output += static_cast<char>(character);
  }
  Feed(output);
}

void Session::Resize(TerminalSize size) {
  if (snapshot.size == size) return;
  vterm_set_size(terminal.get(), size.rows, size.columns);
  snapshot.size = size;
  if (backend && IsOpen()) {
    if (!outbound.empty() && std::holds_alternative<TerminalSize>(outbound.back())) {
      outbound.back() = size;
    } else {
      outbound.emplace_back(size);
    }
    StartWriter(*this);
  }
  Changed();
}

void Session::SetPalette(const TerminalPalette& palette) {
  const auto foreground = ToVTerm(palette.foreground);
  const auto background = ToVTerm(palette.background);
  vterm_screen_set_default_colors(screen, &foreground, &background);
  for (int index = 0; index < static_cast<int>(palette.ansi.size()); ++index) {
    const auto color = ToVTerm(palette.ansi[index]);
    vterm_state_set_palette_color(state, index, &color);
  }
}

void Session::Queue(std::string_view bytes) {
  if (bytes.empty() || !backend || !IsOpen()) return;
  const auto limit = std::max(MaxInputBytes, options.pending_output_bytes);
  if (bytes.size() > limit - outbound_bytes || outbound.size() >= 1024) {
    Fail(Error(TerminalErrorCode::IoFailed, "Terminal input queue is full"));
    return;
  }
  Bytes copy(bytes.size());
  std::memcpy(copy.data(), bytes.data(), bytes.size());
  outbound_bytes += copy.size();
  if (!outbound.empty()) {
    if (auto* pending = std::get_if<Bytes>(&outbound.back())) {
      pending->insert(pending->end(), copy.begin(), copy.end());
      StartWriter(*this);
      return;
    }
  }
  outbound.emplace_back(std::move(copy));
  StartWriter(*this);
}

void Session::StartTransport() {
  reader = tasks.Launch(ReadLoop(weak_from_this(), backend, std::min(options.pending_output_bytes, std::size_t{64 * 1024})));
}

void Session::StartClose(std::optional<TerminalStatus> outcome) {
  if (closing || close_result) return;
  closing = true;
  command_stop.request_stop();
  outbound.clear();
  if (!outcome) snapshot.status = TerminalStatus::Closing;
  if (outcome == TerminalStatus::Failed) snapshot.status = TerminalStatus::Failed;
  if (outcome == TerminalStatus::Exited) snapshot.status = TerminalStatus::Closing;
  Changed();
  tasks.Launch(CloseBackend(shared_from_this(), outcome.value_or(TerminalStatus::Closed)));
}

void Session::Fail(TerminalError error) {
  if (!IsOpen()) return;
  snapshot.error = std::move(error);
  StartClose(TerminalStatus::Failed);
}

bool Session::Interrupt() {
  if (!IsOpen()) return false;
  if (!commands) {
    Queue("\x03");
    return IsOpen();
  }
  if (snapshot.active_command) {
    if (snapshot.active_command->cancelling) return true;
    snapshot.active_command->cancelling = true;
    command_stop.request_stop();
  } else {
    Print(commands->prompt + draft + "^C\n");
    SetDraft({});
    history_position = command_history.size();
    saved_draft.clear();
  }
  Changed();
  return true;
}

void Session::SetDraft(std::string text) {
  draft = std::move(text);
  const auto end = TextLength(draft);
  draft_selection = {end, end};
  if (update_view) update_view();
}

void Session::Submit() {
  if (!IsOpen() || !commands || snapshot.active_command) return;
  auto args = Split(draft);
  if (!args) {
    if (Cursor().col) Feed("\r\n");
    Print("Unclosed quote or trailing escape\n");
    return;
  }
  if (!args->empty() && commands->history_limit) {
    if (command_history.empty() || command_history.back() != draft) command_history.push_back(draft);
    if (command_history.size() > commands->history_limit) command_history.pop_front();
  }
  history_position = command_history.size();
  saved_draft.clear();
  Print(commands->prompt + std::exchange(draft, {}) + "\n");
  draft_selection = {};
  if (args->empty()) return;
  const auto name = args->front();
  if (name == "clear") {
    Feed("\x1b[H\x1b[2J\x1b[3J");
    return;
  }
  if (name == "help") {
    Print("help  List registered commands\nclear Clear the terminal\n");
    for (const auto& command : commands->commands) Print(command.name + "  " + command.help + "\n");
    return;
  }
  const auto entry = std::ranges::find(commands->commands, name, &TerminalCommand::name);
  if (entry == commands->commands.end()) {
    Print("Unknown command: " + name + "\n");
    return;
  }
  command_stop = std::stop_source{};
  snapshot.active_command = TerminalCommandState{.name = name};
  ++command_id;
  args->erase(args->begin());
  command_task = tasks.Launch(RunCommand(weak_from_this(), *entry,
      {std::move(*args), Access::Output(shared_from_this(), command_id), command_stop.get_token()}));
  Changed();
}

void Session::Recall(int direction) {
  if (command_history.empty()) return;
  if (history_position == command_history.size()) saved_draft = draft;
  if (direction < 0 && history_position > 0) --history_position;
  if (direction > 0 && history_position < command_history.size()) ++history_position;
  SetDraft(history_position < command_history.size() ? command_history[history_position] : saved_draft);
}

void Session::Complete() {
  if (draft.find_first_of(" \t\\\"'") != std::string::npos) return;
  std::vector<std::string_view> matches;
  auto consider = [&](std::string_view name) {
    if (name.starts_with(draft)) matches.push_back(name);
  };
  consider("help");
  consider("clear");
  for (const auto& registration : commands->commands) consider(registration.name);
  if (matches.size() == 1) SetDraft(std::string(matches.front()) + ' ');
  if (matches.size() > 1) {
    Print(commands->prompt + draft + "\n");
    for (auto match : matches) Print(std::string(match) + "\n");
  }
}

void Session::Focus(bool focused) {
  if (focused) vterm_state_focus_in(state);
  else vterm_state_focus_out(state);
}

VTermScreenCell Session::Cell(std::int64_t row, int column) const {
  VTermScreenCell cell{};
  cell.width = 1;
  vterm_state_get_default_colors(state, &cell.fg, &cell.bg);
  if (!alternate && row >= history_base && row < ScreenRow()) {
    const auto& cells = history[static_cast<std::size_t>(row - history_base)].cells;
    if (column >= 0 && static_cast<std::size_t>(column) < cells.size()) cell = cells[column];
  } else if (row >= ScreenRow() && row - ScreenRow() < snapshot.size.rows &&
             column >= 0 && column < snapshot.size.columns) {
    vterm_screen_get_cell(screen, {static_cast<int>(row - ScreenRow()), column}, &cell);
  }
  return cell;
}

int Session::Columns(std::int64_t row) const {
  if (!alternate && row >= history_base && row < ScreenRow()) {
    return static_cast<int>(history[static_cast<std::size_t>(row - history_base)].cells.size());
  }
  return snapshot.size.columns;
}

bool Session::Wrapped(std::int64_t row) const {
  const auto next = row + 1;
  const auto screen_row = ScreenRow();
  if (!alternate && next >= history_base && next < screen_row) {
    return history[static_cast<std::size_t>(next - history_base)].continuation;
  }
  return next >= screen_row && next < screen_row + snapshot.size.rows &&
      vterm_state_get_lineinfo(state, static_cast<int>(next - screen_row))->continuation;
}

std::int64_t Session::FirstRow() const { return alternate ? 0 : history_base; }
std::int64_t Session::ScreenRow() const {
  return alternate ? 0 : history_base + static_cast<std::int64_t>(history.size());
}

VTermPos Session::Cursor() const {
  VTermPos position{};
  vterm_state_get_cursorpos(state, &position);
  return position;
}

TerminalSession Access::Handle(std::shared_ptr<Session> session) { return TerminalSession(std::move(session)); }
std::shared_ptr<Session> Access::Model(const TerminalSession& handle) { return handle.session_; }
TerminalCommandOutput Access::Output(const std::shared_ptr<Session>& session, std::size_t id) {
  return TerminalCommandOutput(session, id);
}

} // namespace huxerui::terminal::detail

namespace huxerui::terminal {

TerminalSession::TerminalSession(std::shared_ptr<detail::Session> session) : session_(std::move(session)) {}

TerminalSnapshot TerminalSession::Snapshot() const {
  session_->CheckThread();
  return session_->observed;
}

bool TerminalSession::Interrupt() const {
  session_->CheckThread();
  return session_->Interrupt();
}

Task<TerminalResult<void>> TerminalSession::CloseAsync() const { return detail::WaitForClose(session_); }

TerminalCommandOutput::TerminalCommandOutput(std::weak_ptr<detail::Session> session, std::size_t command_id)
    : session_(std::move(session)), command_id_(command_id) {}

Task<TerminalResult<void>> TerminalCommandOutput::WriteAsync(std::string text) const {
  return detail::WriteCommand(session_, command_id_, std::move(text));
}

TerminalResult<TerminalSession> CreateCommandSession(TerminalCommandOptions commands, TerminalSessionOptions options) {
  using Result = TerminalResult<TerminalSession>;
  auto invalid = [](std::string message) {
    return Result::Failure(detail::Error(TerminalErrorCode::InvalidConfiguration, std::move(message)));
  };
  if (!detail::ValidOptions(options)) return invalid("Invalid terminal size or output limit");
  if (commands.prompt.size() > 4096 || !detail::PlainLine(commands.prompt)) {
    return invalid("Invalid terminal prompt");
  }
  std::vector<std::string_view> names{"clear", "help"};
  for (const auto& command : commands.commands) {
    if (command.name.empty() || !command.handler || !detail::PlainLine(command.name) ||
        command.name.find_first_of(" \\'\"") != std::string::npos ||
        std::ranges::find(names, command.name) != names.end()) {
      return invalid("Invalid or duplicate terminal command: " + command.name);
    }
    names.push_back(command.name);
  }
  auto session = std::make_shared<detail::Session>(options);
  session->commands = std::move(commands);
  return Result::Success(detail::Access::Handle(std::move(session)));
}

Task<TerminalResult<TerminalSession>> OpenShellAsync(TerminalShellOptions shell, TerminalSessionOptions options) {
  using Result = TerminalResult<TerminalSession>;
  if (!detail::ValidOptions(options) || !detail::ValidShell(shell)) {
    co_return Result::Failure(detail::Error(TerminalErrorCode::InvalidConfiguration, "Invalid shell configuration"));
  }
  auto result = co_await detail::StartShell(std::move(shell), options.initial_size);
  if (!result.Succeeded()) co_return Result::Failure(std::move(result).Error());
  auto session = std::make_shared<detail::Session>(options);
  session->backend = std::move(result).Value();
  session->StartTransport();
  co_return Result::Success(detail::Access::Handle(std::move(session)));
}

Task<TerminalResult<TerminalSession>> OpenTransportAsync(
    std::unique_ptr<TerminalBackend> backend, TerminalSessionOptions options) {
  using Result = TerminalResult<TerminalSession>;
  if (!backend || !detail::ValidOptions(options)) {
    if (backend) (void)co_await backend->CloseAsync();
    co_return Result::Failure(detail::Error(TerminalErrorCode::InvalidConfiguration, "Invalid terminal transport configuration"));
  }
  auto resized = TerminalResult<void>::Success();
  try {
    resized = co_await backend->ResizeAsync(options.initial_size);
  } catch (const std::exception& error) {
    resized = TerminalResult<void>::Failure(detail::Error(TerminalErrorCode::IoFailed, error.what()));
  }
  if (!resized.Succeeded()) {
    (void)co_await backend->CloseAsync();
    co_return Result::Failure(std::move(resized).Error());
  }
  auto session = std::make_shared<detail::Session>(options);
  session->backend = std::move(backend);
  session->StartTransport();
  co_return Result::Success(detail::Access::Handle(std::move(session)));
}

void Install(ApplicationContext&) {}

} // namespace huxerui::terminal
