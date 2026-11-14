#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <app_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/terminal.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

using namespace huxerui;
using namespace huxerui::terminal;
using namespace std::chrono_literals;

View App();

namespace {

using TabId = std::uint64_t;

struct Tab {
  TabId id;
  std::string title;
  std::optional<TerminalSession> session;
  std::string error;
};

struct WindowTabs {
  std::vector<Tab> tabs;
  std::optional<TabId> active;
  TabId next_id = 1;
};

struct ShellProfile {
  std::string name;
  TerminalShellOptions options;
};

const WindowOptions preview_window{
    .title = "Terminal Preview",
    .initial_size = {900.0F, 550.0F},
    .minimum_size = Size{420.0F, 280.0F},
    .chrome_mode = WindowChromeMode::Custom,
    .title_bar_height = 44.0F,
};

TerminalResult<TerminalSession> OpenCommands() {
  return CreateCommandSession({
      .prompt = "workspace \xE2\x9D\xAF ",
      .commands = {
          {.name = "status", .help = "Check the workspace",
           .handler = [](TerminalCommandContext context) -> Task<int> {
             if (!(co_await context.output.WriteAsync("Checking workspace...\n")).Succeeded()) co_return 1;
             co_await Delay(700ms);
             if (context.stop_token.stop_requested()) co_return 1;
             co_return (co_await context.output.WriteAsync("Workspace ready\n")).Succeeded() ? 0 : 1;
           }},
          {.name = "echo", .help = "Print arguments",
           .handler = [](TerminalCommandContext context) -> Task<int> {
             std::string line;
             for (const auto& argument : context.arguments) {
               if (!line.empty()) line += ' ';
               line += argument;
             }
             co_return (co_await context.output.WriteAsync(line + '\n')).Succeeded() ? 0 : 1;
           }},
      },
  });
}

std::optional<ShellProfile> LocalShell() {
#ifdef _WIN32
  return ShellProfile{"PowerShell", {.executable = "powershell.exe", .arguments = {"-NoLogo", "-NoProfile"}}};
#elif defined(__linux__) && !defined(__ANDROID__)
  return ShellProfile{"Bash", {.executable = "/bin/bash", .arguments = {"--noprofile", "--norc"}}};
#elif defined(__APPLE__) && TARGET_OS_OSX
  return ShellProfile{"Zsh", {.executable = "/bin/zsh", .arguments = {"-f"}};
#else
  return {};
#endif
}

Tab& AppendTab(WindowTabs& window, std::string name) {
  const auto id = window.next_id++;
  window.tabs.push_back({.id = id, .title = std::move(name) + " " + std::to_string(id)});
  window.active = id;
  return window.tabs.back();
}

void SetSession(Tab& tab, TerminalResult<TerminalSession> result) {
  if (result.Succeeded()) tab.session = std::move(result).Value();
  else tab.error = std::move(result).Error().message;
}

void AddCommands(State<WindowTabs> tabs) {
  WindowTabs next = tabs;
  SetSession(AppendTab(next, "Commands"), OpenCommands());
  tabs = std::move(next);
}

void AddShell(State<WindowTabs> tabs, TaskScope tasks, ShellProfile shell) {
  WindowTabs next = tabs;
  const auto id = AppendTab(next, std::move(shell.name)).id;
  tabs = std::move(next);
  tasks.Launch([tabs, id, options = std::move(shell.options)]() -> Task<void> {
    auto result = co_await OpenShellAsync(options);
    WindowTabs updated = tabs;
    const auto tab = std::ranges::find(updated.tabs, id, &Tab::id);
    if (tab == updated.tabs.end()) {
      if (result.Succeeded()) (void)co_await result.Value().CloseAsync();
      co_return;
    }
    SetSession(*tab, std::move(result));
    tabs = std::move(updated);
  });
}

void CloseSession(const TerminalSession& session, const TaskScope& tasks) {
  tasks.Launch([session]() -> Task<void> { (void)co_await session.CloseAsync(); });
}

void CloseTab(State<WindowTabs> tabs, const TaskScope& tasks, TabId id) {
  WindowTabs next = tabs;
  const auto tab = std::ranges::find(next.tabs, id, &Tab::id);
  if (tab == next.tabs.end()) return;

  auto session = std::move(tab->session);
  const auto following = next.tabs.erase(tab);
  if (next.active == id) {
    if (next.tabs.empty()) next.active.reset();
    else next.active = following == next.tabs.end() ? next.tabs.back().id : following->id;
  }
  tabs = std::move(next);
  if (session) CloseSession(*session, tasks);
}

std::string Status(const TerminalSnapshot& snapshot) {
  if (snapshot.error) return snapshot.error->message;
  if (snapshot.active_command) {
    return snapshot.active_command->cancelling ? "Stopping" : "Running " + snapshot.active_command->name;
  }
  switch (snapshot.status) {
  case TerminalStatus::Open: return "Ready";
  case TerminalStatus::Closing: return "Closing";
  case TerminalStatus::Closed: return "Closed";
  case TerminalStatus::Failed: return "Failed";
  case TerminalStatus::Exited:
    if (snapshot.exit && snapshot.exit->code) return "Exited (" + std::to_string(*snapshot.exit->code) + ")";
    return "Disconnected";
  }
  return {};
}

View SessionTab(const Tab& tab, bool active, const State<WindowTabs>& tabs, const TaskScope& app_tasks,
                const ColorScheme& colors) {
  const auto close_label = "Close " + tab.title;
  return Row{
      Button(tab.title)
          .With(Semantics{.role = SemanticRole::Tab, .selected = active})
          .OnClick([tabs, id = tab.id] {
            if (tabs->active == id) return;
            tabs.Update([id](WindowTabs& window) { window.active = id; });
          }),
      IconButton(app::images::close, close_label).With(Tooltip(close_label))
          .OnClick([tabs, app_tasks, id = tab.id] { CloseTab(tabs, app_tasks, id); }),
  }.With(CrossAlign(CrossAxisAlignment::Center), CornerRadius(7.0F),
         Background(active ? colors.surface_container_highest : colors.surface_container_low)).Key(tab.id);
}

View StatusBar(const TerminalSnapshot& snapshot, const ColorScheme& colors) {
  const auto size = std::to_string(snapshot.size.columns) + " \xC3\x97 " + std::to_string(snapshot.size.rows);
  return Row{
      Text(Status(snapshot)).Style({Font::System(12.0F), snapshot.error ? colors.error : colors.on_surface_variant})
          .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
      Text(size).Style({Font::Monospace(12.0F), colors.on_surface_variant}),
  }.With(Padding(EdgeInsets::Symmetric(12.0F, 4.0F)), Spacing(8.0F), Background(colors.surface_container));
}

[[huxerui::composable]]
View SessionPane(TerminalSession session, TabId id, bool desktop) {
  const auto focus = UseViewHandle();
  const auto tasks = UseTaskScope();
  Lifecycle([focus, tasks, desktop] {
    if (desktop) tasks.Launch([focus]() -> Task<void> {
      focus.RequestFocus();
      co_return;
    });
    return [] {};
  }, id);
  return Column{
      Terminal(session).Key(id).With(Grow(), focus, Semantics{.identifier = "preview-terminal"}),
      StatusBar(session.Snapshot(), UseTheme().colors),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View Preview() {
  const auto tabs = UseState(WindowTabs{});
  const auto tasks = UseTaskScope();
  const auto app_tasks = UseApplicationTaskScope();
  const auto application = UseApplication();
  const bool desktop = application.SupportsMultipleWindows();
  const auto menu = UseMenu();
  const auto add_anchor = UseViewHandle();
  const auto& theme = UseTheme();
  const auto& colors = theme.colors;

  Lifecycle([tabs, app_tasks] {
    AddCommands(tabs);
    return [tabs, app_tasks] {
      for (const auto& tab : tabs->tabs) {
        if (tab.session) CloseSession(*tab.session, app_tasks);
      }
    };
  });

  const auto show_menu = [tabs, tasks, application, desktop, menu, add_anchor] {
    std::vector<MenuEntry> entries{MenuItem("Application commands", [tabs] { AddCommands(tabs); })};
    if (auto shell = LocalShell()) {
      entries.emplace_back(MenuItem(shell->name, [tabs, tasks, profile = *shell] { AddShell(tabs, tasks, profile); }));
    }
    if (desktop) {
      entries.emplace_back(MenuSection{});
      entries.emplace_back(MenuItem("New window", [application] { application.OpenWindow(App, preview_window); }));
    }
    menu.Show(add_anchor, std::move(entries), {.placement = {AnchorSide::Below, AnchorAlignment::End}});
  };

  const WindowTabs& window = tabs;
  std::vector<View> tab_views;
  const Tab* active = nullptr;
  for (const auto& tab : window.tabs) {
    const bool selected = window.active == tab.id;
    if (selected) active = &tab;
    tab_views.push_back(SessionTab(tab, selected, tabs, app_tasks, colors));
  }
  const float tab_height = desktop ? 32.0F : 40.0F;
  ThemeDefinition toolbar_style;
  auto button_style = UseEnvironment<ButtonStyle>();
  button_style.background = Color::Transparent();
  button_style.label_style = {Font::System(13.0F).WithWeight(FontWeight::Medium), colors.on_surface};
  button_style.padding = EdgeInsets{.left = 10.0F};
  button_style.minimum_width = 0.0F;
  button_style.minimum_height = tab_height;
  button_style.corner_radii = CornerRadii{7.0F};
  toolbar_style.Set(button_style);
  auto icon_style = UseEnvironment<IconButtonStyle>();
  icon_style.icon_size = 16.0F;
  icon_style.minimum_interactive_size = tab_height;
  icon_style.state_layer_size = tab_height - 4.0F;
  toolbar_style.Set(icon_style);

  auto toolbar_content = Row{
      ScrollView(Row(std::move(tab_views)).With(Spacing(4.0F), Semantics{.role = SemanticRole::TabList}))
          .ScrollAxis(Axis::Horizontal).With(Grow(), ScrollBar{}),
      IconButton(app::images::add, "New session").With(add_anchor, Tooltip("New session")).OnClick(show_menu),
  }.With(Grow(), Padding(EdgeInsets::Symmetric(8.0F, 4.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center));
  View toolbar = desktop ? View(WindowTitleBar{std::move(toolbar_content)}) : View(std::move(toolbar_content));

  View content = active && active->session
      ? SessionPane(*active->session, active->id, desktop)
      : View(Stack{
          Text(active ? (active->error.empty() ? "Opening shell..." : active->error) : "No sessions open")
              .Style({Font::System(14.0F), active && !active->error.empty() ? colors.error : colors.on_surface_variant}),
        }.With(Align(HorizontalAlignment::Center, VerticalAlignment::Center), Padding(20.0F)));

  return Column{
      Theme{std::move(toolbar_style), std::move(toolbar).With(Background(colors.surface_container))},
      Divider(),
      std::move(content).With(Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface))
      .On<ViewEvents::KeyIntercept>([add_anchor](const KeyEvent& event) {
        return event.type == KeyEventType::Down && event.key == Key::Escape && event.modifiers.control &&
            add_anchor.RequestFocus(true);
      });
}

} // namespace

View App() { return MaterialTheme{Preview()}; }

const Application application{
    App,
    {.window = preview_window,
     .show_debug_overlay = false,
     .application_hooks = {huxerui::terminal::Install}}
};
