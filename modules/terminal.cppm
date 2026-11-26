// Hand-maintained module interface (`cpp_modules_gen = false` under [package.metadata.huxerui]).
// HuxerUI's `strip_noncode()` misparses the `10'000` digit separator in include/huxerui/terminal.h:65
// as a character literal, omitting public entities declared after TerminalSize.
//
// `import huxerui.terminal;` and including <huxerui/terminal.h> reach the same 25 public entities.

module;

#include <typeinfo>
#include <huxerui/terminal.h>

export module huxerui.terminal;

export import huxerui;

export namespace huxerui::terminal {
    using huxerui::terminal::CreateCommandSession;
    using huxerui::terminal::Install;
    using huxerui::terminal::OpenShellAsync;
    using huxerui::terminal::OpenTransportAsync;
    using huxerui::terminal::Terminal;
    using huxerui::terminal::TerminalBackend;
    using huxerui::terminal::TerminalCommand;
    using huxerui::terminal::TerminalCommandContext;
    using huxerui::terminal::TerminalCommandOptions;
    using huxerui::terminal::TerminalCommandOutput;
    using huxerui::terminal::TerminalCommandState;
    using huxerui::terminal::TerminalError;
    using huxerui::terminal::TerminalErrorCode;
    using huxerui::terminal::TerminalEvents;
    using huxerui::terminal::TerminalExit;
    using huxerui::terminal::TerminalExitReason;
    using huxerui::terminal::TerminalRead;
    using huxerui::terminal::TerminalResult;
    using huxerui::terminal::TerminalSession;
    using huxerui::terminal::TerminalSessionOptions;
    using huxerui::terminal::TerminalShellOptions;
    using huxerui::terminal::TerminalSize;
    using huxerui::terminal::TerminalSnapshot;
    using huxerui::terminal::TerminalStatus;
    using huxerui::terminal::TerminalStyle;
}
