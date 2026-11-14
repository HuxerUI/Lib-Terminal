#pragma once

#include <array>
#include <deque>
#include <thread>

#include <huxerui/state.h>
#include <huxerui/terminal.h>
#include <vterm.h>

namespace huxerui::terminal::detail {

struct Cluster {
  std::size_t start;
  std::size_t end;
  int offset;
  int length;
  int columns;
};

std::optional<std::vector<Cluster>> Clusters(std::string_view text);
std::optional<std::size_t> ByteOffset(std::string_view text, TextOffset offset);
TextOffset TextLength(std::string_view text);
std::string Encode(std::uint32_t scalar);
TerminalError Error(TerminalErrorCode code, std::string message);
bool ValidSize(TerminalSize size);
VTermModifier Modifiers(KeyModifiers mods);

struct ScrollbackRow {
  std::vector<VTermScreenCell> cells;
  bool continuation;
};

struct TerminalPalette {
  Color foreground;
  Color background;
  Color cursor;
  Color selection;
  std::array<Color, 16> ansi;

  bool operator==(const TerminalPalette&) const = default;
};

class Session final : public std::enable_shared_from_this<Session> {
public:
  explicit Session(TerminalSessionOptions options);
  ~Session();

  void CheckThread() const;
  void Changed();
  void Feed(std::string_view text);
  void Print(std::string_view text);
  void Resize(TerminalSize size);
  void SetPalette(const TerminalPalette& palette);
  void Queue(std::string_view bytes);
  void StartTransport();
  void StartClose(std::optional<TerminalStatus> outcome = {});
  void Fail(TerminalError error);
  bool Interrupt();
  void Submit();
  void Complete();
  void Recall(int direction);
  void SetDraft(std::string text);
  void Focus(bool focused);
  VTermScreenCell Cell(std::int64_t row, int column) const;
  int Columns(std::int64_t row) const;
  bool Wrapped(std::int64_t row) const;
  std::int64_t FirstRow() const;
  std::int64_t ScreenRow() const;
  VTermPos Cursor() const;
  bool IsOpen() const { return snapshot.status == TerminalStatus::Open; }

  TaskScope tasks;
  const TerminalSessionOptions options;
  const std::thread::id thread = std::this_thread::get_id();
  State<TerminalSnapshot> observed;
  TerminalSnapshot snapshot;
  std::function<void()> update_view;
  std::function<bool(const ScrollInputEvent&)> scroll_view;
  std::unique_ptr<VTerm, decltype(&vterm_free)> terminal{nullptr, vterm_free};
  VTermState* state = nullptr;
  VTermScreen* screen = nullptr;
  std::deque<ScrollbackRow> history;
  std::int64_t history_base = 0;
  bool alternate = false;
  bool cursor_visible = true;
  int cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
  int mouse_mode = VTERM_PROP_MOUSE_NONE;
  std::optional<std::int64_t> viewport_top;
  std::optional<std::int64_t> main_viewport_top;
  TerminalSize requested_size;

  std::optional<TerminalCommandOptions> commands;
  std::string draft;
  bool composing = false;
  TextSelection draft_selection{0, 0, TextAffinity::Downstream};
  std::deque<std::string> command_history;
  std::size_t history_position = 0;
  std::string saved_draft;
  std::size_t command_id = 0;
  std::stop_source command_stop;
  TaskHandle command_task;

  std::shared_ptr<TerminalBackend> backend;
  std::deque<std::variant<Bytes, TerminalSize>> outbound;
  std::size_t outbound_bytes = 0;
  TaskHandle reader;
  TaskHandle writer;
  bool writing = false;
  bool closing = false;
  std::optional<TerminalResult<void>> close_result;
  std::vector<CallbackCompletion<TerminalResult<void>>> close_waiters;
};

struct Access {
  static TerminalSession Handle(std::shared_ptr<Session> session);
  static std::shared_ptr<Session> Model(const TerminalSession& handle);
  static TerminalCommandOutput Output(const std::shared_ptr<Session>& session, std::size_t id);
};

Task<TerminalResult<std::unique_ptr<TerminalBackend>>> StartShell(
    TerminalShellOptions shell, TerminalSize size);

} // namespace huxerui::terminal::detail
