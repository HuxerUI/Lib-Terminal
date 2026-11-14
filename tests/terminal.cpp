#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>

#include <huxerui/huxerui.h>
#include <huxerui/terminal.h>
#include <huxerui/testing/ui_test.h>

using namespace huxerui;
using namespace huxerui::terminal;
using namespace huxerui::testing;
using namespace std::chrono_literals;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Fixture {
  State<std::optional<TerminalSession>> session;
  State<bool> visible;
  std::vector<std::string> arguments;
  std::shared_ptr<Clipboard> clipboard;
  TerminalStyle style;
  std::function<void()> start;
};

std::weak_ptr<Fixture> active_fixture;

[[huxerui::composable]]
View Content(const std::shared_ptr<Fixture>& fixture) {
  Lifecycle([fixture] {
    fixture->start();
    return [] {};
  });
  const bool visible = fixture->visible;
  const std::optional<TerminalSession> current = fixture->session;
  if (!visible || !current) return Text("Unmounted");
  return Terminal(*current, fixture->style).Key("terminal").With(Grow());
}

View Root() { return Content(active_fixture.lock()); }

Application TestApp(std::shared_ptr<Fixture> fixture, std::function<void()> start) {
  fixture->start = std::move(start);
  return Application{
      Root,
      {.show_debug_overlay = false, .application_hooks = {[fixture](ApplicationContext&) {
        fixture->session = State<std::optional<TerminalSession>>{std::optional<TerminalSession>{}};
        fixture->visible = State<bool>{true};
        active_fixture = fixture;
      }}}
  };
}

bool HasText(UiTest& ui, std::string text) {
  return ui.FindSemantics(UiSemanticSelector::Label(text)).Exists();
}

void Wait(UiTest& ui, const std::function<bool()>& condition, const char* message) {
  try {
    ui.PumpUntil(condition, {.step = 5ms});
  } catch (const UiTestFailure&) {
    throw std::runtime_error(message);
  }
}

void Commit(UiTest& ui, std::string text, std::optional<TextRange> target = {}) {
  const auto input = ui.ActiveTextInput();
  Check(input.has_value(), "Terminal must own the text-input session");
  const auto result = ui.SendTextInput({input->session_id, {{
      .kind = TextInputCommandKind::CommitText, .target = target, .text = std::move(text),
  }}});
  Check(result.result_code == TextInputResultCode::Ok, "Accept committed text");
  ui.Pump();
}

void Commands() {
  auto fixture = std::make_shared<Fixture>();
  const auto app = TestApp(fixture, [weak_fixture = std::weak_ptr(fixture)] {
    const auto fixture = weak_fixture.lock();
    auto session = CreateCommandSession({
        .prompt = "$ ",
        .commands = {
            {.name = "echo", .help = "Print arguments",
             .handler = [weak = std::weak_ptr(fixture)](TerminalCommandContext context) -> Task<int> {
               if (auto model = weak.lock()) model->arguments = context.arguments;
               auto result = co_await context.output.WriteAsync("OUTPUT\n");
               co_return result.Succeeded() ? 0 : 1;
             }},
            {.name = "wait", .help = "Wait for cancellation",
             .handler = [](TerminalCommandContext context) -> Task<int> {
               while (!context.stop_token.stop_requested()) co_await Delay(10ms);
               co_return 1;
             }},
        },
    });
    fixture->session = std::move(session).Value();
  });
  UiTest ui(app, {.viewport = {520, 320}});
  ui.Pump();
  auto terminal = ui.Find(UiSelector::Key("terminal"));
  terminal.Tap();
  const auto input = ui.ActiveTextInput();
  Check(input.has_value(), "Terminal must own the text-input session");
  Commit(ui, "echo \"two words\" ''");
  auto composed = ui.SendTextInput({input->session_id, {{
      .kind = TextInputCommandKind::UpdateComposition, .text = " \xE4\xB8\xAD",
  }}});
  Check(composed.result_code == TextInputResultCode::Ok, "Accept IME preedit");
  Check(ui.ActiveTextInput()->composition.has_value(), "Retain provisional input");
  auto committed = ui.SendTextInput({input->session_id, {{
      .kind = TextInputCommandKind::CommitText, .text = " \xE4\xB8\xAD",
  }}});
  Check(committed.result_code == TextInputResultCode::Ok, "Commit IME text");
  ui.PressKey(Key::Enter);
  Wait(ui, [&] { return HasText(ui, "OUTPUT"); }, "Publish command output");
  Check(fixture->arguments == std::vector<std::string>{"two words", "", "\xE4\xB8\xAD"}, "Parse quoted arguments");
  ui.PressKey(Key::ArrowUp);
  Check(HasText(ui, "$ echo \"two words\" '' \xE4\xB8\xAD"), "Recall history inline");
  fixture->visible = false;
  ui.Pump();
  fixture->visible = true;
  ui.Pump();
  Check(HasText(ui, "OUTPUT"), "Retain screen across detach");
  terminal.Tap();
  Check(HasText(ui, "$ echo \"two words\" '' \xE4\xB8\xAD"), "Retain draft across detach");
  const auto recalled = ui.ActiveTextInput();
  Check(recalled.has_value(), "Terminal restores input focus");
  Commit(ui, "wait", TextRange{0, recalled->selection.active});
  ui.PressKey(Key::Enter);
  Wait(ui, [&] { return fixture->session->value().Snapshot().active_command.has_value(); }, "Start cancellable command");
  ui.PressKey(Key::C, {.control = true});
  Wait(ui, [&] { return !fixture->session->value().Snapshot().active_command; }, "Finish cancelled command");
  Check(HasText(ui, "^C"), "Show cancellation in the grid");
}

void CopyScrollback() {
  constexpr Size viewport{360, 240};
  auto fixture = std::make_shared<Fixture>();
  const auto app = TestApp(fixture, [weak = std::weak_ptr(fixture)] {
    const auto fixture = weak.lock();
    fixture->clipboard = UseApplication().Clipboard();
    auto session = CreateCommandSession({
        .prompt = "$ ",
        .commands = {{
            .name = "lines",
            .handler = [weak](TerminalCommandContext context) -> Task<int> {
              const auto fixture = weak.lock();
              const auto size = fixture->session->value().Snapshot().size;
              std::string text(size.columns - 1, 'a');
              text += " bcd\n";
              text += std::string(size.columns, 'h') + "\nend\n";
              for (int row = 0; row < size.rows + 1; ++row) text += "later\n";
              co_return (co_await context.output.WriteAsync(std::move(text))).Succeeded() ? 0 : 1;
            },
        }},
    });
    fixture->session = std::move(session).Value();
  });
  UiTest ui(app, {.viewport = viewport});
  ui.PumpUntil([&] {
    return fixture->session->has_value() && fixture->session->value().Snapshot().size != TerminalSize{};
  });
  const auto size = fixture->session->value().Snapshot().size;
  Check(size.rows >= 6 && size.columns >= 8, "Terminal viewport must fit the copied lines");
  ui.Find(UiSelector::Key("terminal")).Tap();
  Commit(ui, "lines");
  ui.PressKey(Key::Enter);
  Wait(ui, [&] { return HasText(ui, "later") && !fixture->session->value().Snapshot().active_command; },
       "Write lines into scrollback");
  const std::string start(size.columns - 1, 'a');
  Check(!HasText(ui, start) && !HasText(ui, "end"), "The copied range must be in scrollback");
  const auto show_history = [&] {
    ui.PressKey(Key::PageUp, {.shift = true});
    ui.PressKey(Key::PageUp, {.shift = true});
  };
  float row_height = 0;
  const auto copy = [&] {
    const auto first = ui.FindSemantics(UiSemanticSelector::Label(start)).One().bounds;
    const auto last = ui.FindSemantics(UiSemanticSelector::Label("end")).One().bounds;
    const float cell_width = first.width / size.columns;
    row_height = first.height;
    ui.Drag({first.x + cell_width / 2, first.y + first.height / 2},
            {last.x + cell_width * 2.5F, last.y + last.height / 2},
            {.pointer = {.device_kind = PointerDeviceKind::Mouse}});
    Check(fixture->clipboard->WriteText("previous clipboard"), "Seed the test clipboard before copying");
    ui.PressKey(Key::C, {.shift = true, .control = true});
    return fixture->clipboard->ReadText();
  };
  const auto expected = start + " bcd\n" + std::string(size.columns, 'h') + "\nend";
  show_history();
  Check(copy() == expected, "Copy joins soft wraps and preserves hard breaks in scrollback");
  Commit(ui, "typing");
  Check(HasText(ui, "$ typing") && !HasText(ui, start), "Committed input returns to the live prompt");

  const auto grow = [&](int rows) {
    ui.SetWindowMetrics({.viewport = {viewport.width, viewport.height + rows * row_height}});
    Wait(ui, [&] { return fixture->session->value().Snapshot().size.rows == size.rows + rows; },
         "Resize the terminal to restore rows from scrollback");
    ui.PressKey(Key::End, {.shift = true});
  };
  grow(5);
  Check(!HasText(ui, start) && HasText(ui, "bcd"), "The soft wrap must cross from scrollback into the screen");
  show_history();
  Check(copy() == expected, "Copy joins the soft wrap across the scrollback boundary");

  grow(6);
  Check(HasText(ui, start), "The entire copied range must be back on the screen");
  Check(copy() == expected, "Copy preserves line breaks when scrollback rows return to the screen");
}

struct TransportTrace {
  std::vector<TerminalSize> sizes;
  std::size_t chunks_read = 0;
  bool closed = false;
};

class MemoryBackend final : public TerminalBackend {
public:
  MemoryBackend(std::shared_ptr<TransportTrace> trace, std::span<const std::string_view> chunks)
      : trace_(std::move(trace)), chunks_(chunks) {}

  Task<TerminalResult<TerminalRead>> ReadAsync(std::size_t maximum_bytes) override {
    co_await Delay(25ms);
    if (trace_->closed) {
      co_return TerminalResult<TerminalRead>::Failure({TerminalErrorCode::Closed, "Closed"});
    }
    if (trace_->chunks_read == chunks_.size()) {
      co_return TerminalResult<TerminalRead>::Success(TerminalExit{TerminalExitReason::Exited, 7});
    }
    const auto text = chunks_[trace_->chunks_read++];
    Check(text.size() <= maximum_bytes, "Transport respected read limit");
    Bytes bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    co_return TerminalResult<TerminalRead>::Success(std::move(bytes));
  }

  Task<TerminalResult<void>> WriteAsync(Bytes) override { co_return TerminalResult<void>::Success(); }

  Task<TerminalResult<void>> ResizeAsync(TerminalSize size) override {
    trace_->sizes.push_back(size);
    co_return TerminalResult<void>::Success();
  }

  Task<TerminalResult<void>> CloseAsync() override {
    trace_->closed = true;
    co_return TerminalResult<void>::Success();
  }

private:
  std::shared_ptr<TransportTrace> trace_;
  std::span<const std::string_view> chunks_;
};

void Transport() {
  static constexpr std::array<std::string_view, 5> chunks{
      "\x1b[3", "2m\xE4", "\xB8\xAD\x1b[0m\r\n", "\x1b[?1049hALT", "\x1b[?1049l\x1b[97mTAIL",
  };
  auto fixture = std::make_shared<Fixture>();
  std::array<Color, 16> ansi{};
  ansi[2] = Color{0.25F, 0.5F, 0.75F, 1.0F};
  ansi[15] = Color{0.75F, 0.5F, 0.25F, 1.0F};
  fixture->style.ansi = ansi;
  auto trace = std::make_shared<TransportTrace>();
  const auto app = TestApp(fixture, [weak_fixture = std::weak_ptr(fixture), trace] {
    const auto fixture = weak_fixture.lock();
    UseApplicationTaskScope().Launch([fixture, trace]() -> Task<void> {
      auto result = co_await OpenTransportAsync(std::make_unique<MemoryBackend>(trace, chunks));
      fixture->session = std::move(result).Value();
    });
  });
  UiTest ui(app, {.viewport = {520, 320}});
  Wait(ui, [&] { return HasText(ui, "ALT"); }, "Enter the alternate screen");
  Check(!HasText(ui, "\xE4\xB8\xAD"), "Alternate screen hides the main screen");
  Wait(ui, [&] { return fixture->session->has_value() &&
      fixture->session->value().Snapshot().status == TerminalStatus::Exited; }, "Observe transport exit");
  Check(HasText(ui, "\xE4\xB8\xAD") && HasText(ui, "TAIL"), "Restore main screen and drain the last chunk");
  const auto paint = ui.CaptureSnapshot().ToString();
  Check(paint.find("red=0.250000") != std::string::npos && paint.find("red=0.750000") != std::string::npos,
        "Render standard and bright ANSI overrides");
  Check(fixture->session->value().Snapshot().exit->code == 7 && trace->closed, "Publish exit and release the transport");
  Check(trace->sizes.size() >= 2 && trace->sizes.front() == TerminalSize{}, "Apply initial and mounted grid sizes");
}

void CursorVisibility() {
  static constexpr std::array<std::string_view, 4> chunks{"prompt> ", "\x1b[?25l", "x", "\x1b[?25h"};
  auto fixture = std::make_shared<Fixture>();
  fixture->style.cursor = Color{0.125F, 0.375F, 0.625F, 1.0F};
  const auto trace = std::make_shared<TransportTrace>();
  const auto app = TestApp(fixture, [weak = std::weak_ptr(fixture), trace] {
    UseApplicationTaskScope().Launch([weak, trace]() -> Task<void> {
      auto result = co_await OpenTransportAsync(std::make_unique<MemoryBackend>(trace, chunks));
      if (auto fixture = weak.lock()) fixture->session = std::move(result).Value();
    });
  });
  UiTest ui(app, {.viewport = {520, 320}});
  const auto cursor_painted = [&] { return ui.CaptureSnapshot().ToString().find("red=0.125") != std::string::npos; };
  Wait(ui, [&] { return HasText(ui, "prompt>"); }, "Display the byte-stream prompt");
  const auto paint = ui.CaptureSnapshot().ToString();
  Check(paint.find("red=0.980392") != std::string::npos, "Keep the theme background when overriding only the cursor");
  ui.Find(UiSelector::Key("terminal")).Tap();
  Check(cursor_painted(), "Paint the cursor when the terminal gains focus");
  Wait(ui, [&] { return trace->chunks_read == 2; }, "Receive the cursor hide sequence");
  Check(!cursor_painted(), "Hide the cursor without moving it");
  Wait(ui, [&] { return HasText(ui, "prompt> x"); }, "Receive output while the cursor is hidden");
  Check(!cursor_painted(), "Keep the cursor hidden when the peer moves it");
  Wait(ui, [&] { return trace->chunks_read == 4; }, "Receive the cursor show sequence");
  Check(cursor_painted(), "Restore the cursor without moving it");
}

#if defined(_WIN32) || defined(__linux__)
void WaitForProcess(UiTest& ui, const std::function<bool()>& condition, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!condition()) {
    Check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(8ms);
    ui.Pump(8ms);
  }
}

void Shell() {
  auto fixture = std::make_shared<Fixture>();
  auto failure = std::make_shared<std::optional<TerminalError>>();
  const auto app = TestApp(fixture, [weak_fixture = std::weak_ptr(fixture), failure] {
    UseApplicationTaskScope().Launch([weak_fixture, failure]() -> Task<void> {
#ifdef _WIN32
      TerminalShellOptions shell{.executable = "powershell.exe",
          .arguments = {"-NoLogo", "-NoProfile", "-Command",
                        "[Console]::WriteLine('READY'); $line=[Console]::ReadLine(); "
                        "[Console]::WriteLine('received ' + $line); exit 7"}};
#else
      TerminalShellOptions shell{.executable = "/bin/sh",
          .arguments = {"-c", "printf 'READY\\n'; read -r line; printf 'received %s\\n' \"$line\"; exit 7"}};
#endif
      auto result = co_await OpenShellAsync(std::move(shell));
      if (!result.Succeeded()) *failure = result.Error();
      else if (auto fixture = weak_fixture.lock()) fixture->session = std::move(result).Value();
    });
  });
  UiTest ui(app, {.viewport = {520, 320}});
  WaitForProcess(ui, [&] { return failure->has_value() || HasText(ui, "READY"); }, "Start platform shell");
  if (*failure) throw std::runtime_error(failure->value().message);
  ui.Find(UiSelector::Key("terminal")).Tap();
  Commit(ui, "hello");
  ui.PressKey(Key::Enter);
  WaitForProcess(ui, [&] { return fixture->session->value().Snapshot().status != TerminalStatus::Open &&
      fixture->session->value().Snapshot().status != TerminalStatus::Closing; }, "Wait for platform shell exit");
  const auto snapshot = fixture->session->value().Snapshot();
  Check(snapshot.status == TerminalStatus::Exited && snapshot.exit->code == 7, "Report platform shell exit code");
  Check(HasText(ui, "received hello"), "Drain output from the real shell");
}
#endif

} // namespace

int main() {
  try {
    try {
      Commands();
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("Commands: ") + error.what());
    }
    CopyScrollback();
    CursorVisibility();
    try {
      Transport();
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("Transport: ") + error.what());
    }
#if defined(_WIN32) || defined(__linux__)
    try {
      Shell();
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string("Shell: ") + error.what());
    }
#endif
    std::cout << "Terminal tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
