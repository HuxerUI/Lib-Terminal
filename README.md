# Lib-Terminal

Lib-Terminal embeds an interactive character-grid terminal in a HuxerUI application. Link
your application with CMake target `HuxerUI::Terminal` and include
`<huxerui/terminal.h>`.

| Session | Availability | Factory |
| --- | --- | --- |
| Registered application commands | Windows, macOS, Linux, Android, iOS, Web | `CreateCommandSession` |
| Local shell or console program | Windows with ConPTY, macOS, Linux | `OpenShellAsync` |
| Application-provided byte transport | Platforms supported by the supplied transport | `OpenTransportAsync` |

Retain the returned `TerminalSession` in application state and render it with
`Terminal(session).With(Grow())` inside a bounded layout. Add one interactive view per
session. The view takes focus on click or tap. Input appears after the prompt in both
shell and command sessions. View removal preserves output, history, and the command
draft. Await `session.CloseAsync()` to finish a session and release its backend.
Each command handler receives parsed arguments, a plain-text writer and a cooperative
stop token. The command session provides `help`, `clear`, history and command-name
completion.

Use Enter to submit, Up and Down to recall commands, or Tab to complete an application
command name. Use Ctrl+C to interrupt. The grid supports scrollback through the wheel,
touch gestures, and Shift+Page Up/Down. Shift+End returns to the live screen. Use
Ctrl+Shift+C or Command+C to copy and Ctrl+V or Command+V to paste. The preview
binds Ctrl+Escape to return focus to its toolbar.

The VT implementation uses libvterm. Command-line Unicode editing uses utf8proc. Both
are fetched with pinned checksums, linked privately, and carry their original license
terms. Byte-stream emulation follows libvterm's cell-width and escape-sequence behavior.

Run the preview from `examples/preview` with `huxerui run windows`, or substitute
your target platform. Use **+** to open another application command session or a
local shell tab where supported. On desktop, the same menu opens independent
terminal windows. To run the deterministic session and UI tests, configure the library
with `-DHUXERUI_TERMINAL_BUILD_TESTS=ON` and run CTest against that build directory.
