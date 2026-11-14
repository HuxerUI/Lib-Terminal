#include "input.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <huxerui/clipboard.h>
#include <huxerui/layout.h>
#include <huxerui/presentation.h>
#include <huxerui/theme.h>

namespace huxerui::terminal {
namespace {

using detail::Input;
using detail::Session;
using detail::TerminalPalette;

constexpr std::uint32_t Continuation = std::numeric_limits<std::uint32_t>::max();
const TextLayoutOptions LayoutOptions{.shaping = {.direction = TextDirection::LeftToRight}, .wrap = TextWrap::NoWrap};

TerminalPalette ResolvePalette(const ThemeSpec& theme, const TerminalStyle& style) {
  constexpr std::array dark{0x343541ffu, 0xff8c95ffu, 0xa9d8a7ffu, 0xf0cd88ffu,
      0x9cb8f7ffu, 0xdab1e8ffu, 0x8fd6d7ffu, 0xd8d9e5ffu,
      0x75778effu, 0xffa8afffu, 0xc3ebbeffu, 0xffe1a9ffu, 0xb8ceffffu,
      0xe9c8f4ffu, 0xb0eef0ffu, 0xf9f9ffffu};
  constexpr std::array light{0x252535ffu, 0xa53142ffu, 0x32673cffu, 0x805f13ffu,
      0x345e9fffu, 0x7c417effu, 0x24686fffu, 0xc4c3d3ffu,
      0x565871ffu, 0x9d3445ffu, 0x2e6c43ffu, 0x7f5d11ffu, 0x365d9cffu,
      0x823e89ffu, 0x20636bffu, 0xffffffffu};
  const bool dark_theme = theme.brightness == ThemeBrightness::Dark;
  TerminalPalette palette{
      .foreground = style.foreground.value_or(dark_theme ? Color::Rgb(229, 226, 242) : Color::Rgb(40, 37, 58)),
      .background = style.background.value_or(dark_theme ? Color::Rgb(22, 22, 31) : Color::Rgb(250, 249, 253)),
      .cursor = style.cursor.value_or(theme.colors.primary),
      .selection = style.selection.value_or(theme.colors.primary_container),
  };
  if (style.ansi) palette.ansi = *style.ansi;
  else std::ranges::transform(dark_theme ? dark : light, palette.ansi.begin(), Color::Rgba32);
  return palette;
}

struct CellPosition {
  std::int64_t row;
  int column;
  auto operator<=>(const CellPosition&) const = default;
};

std::string CellText(const VTermScreenCell& cell) {
  std::string text;
  if (!cell.chars[0] || cell.chars[0] == Continuation || cell.attrs.conceal) return text;
  for (const auto scalar : cell.chars) {
    if (!scalar) break;
    text += detail::Encode(scalar);
  }
  return text;
}

struct Surface {
  class Extension;
  std::shared_ptr<Session> session;
  Font font;
  float line_height_scale;
  TerminalPalette palette;
  ViewHandle focus;
  TaskScope tasks;
  DialogHandle dialog;
};

class Surface::Extension final : public NodeExtension, public TextSelectionClient {
public:
  Extension(ViewNode&, const Surface& spec)
      : session_(spec.session), input_(std::make_shared<Input>(session_)), spec_(spec) {
    Attach();
  }

  ~Extension() override { Detach(); }

  void Update(ViewNode&, const Surface& spec) {
    if (session_ != spec.session) {
      Detach();
      session_ = spec.session;
      input_ = std::make_shared<Input>(session_);
      Attach();
    }
    spec_ = spec;
    dirty_ = true;
    InvalidatePaint();
  }

  std::shared_ptr<TextInputClient> GetTextInputClient() noexcept override { return attached_ ? input_ : nullptr; }
  TextSelectionClient* GetTextSelectionClient() noexcept override { return attached_ ? this : nullptr; }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer& measurer) override {
    if (!attached_) return PaintInvalidation::None;
    const auto bounds = node.ContentBounds();
    const auto text = measurer.MeasureText("MMMMMMMM", TextStyle{.font = spec_.font},
                                          std::numeric_limits<float>::infinity(), LayoutOptions);
    const float width = std::max(1.0F, text.size.width / 8);
    const float height = std::max(1.0F, text.size.height * spec_.line_height_scale);
    if (!dirty_ && bounds_ == bounds && width_ == width && height_ == height) return PaintInvalidation::None;
    bounds_ = bounds;
    width_ = width;
    height_ = height;
    text_height_ = text.size.height;
    if (!applied_palette_ || *applied_palette_ != spec_.palette) {
      session_->SetPalette(spec_.palette);
      applied_palette_ = spec_.palette;
    }
    if (bounds.width >= width && bounds.height >= height) {
      session_->requested_size = {
          static_cast<int>(std::clamp(std::floor(bounds.width / width), 1.0F, 1000.0F)),
          static_cast<int>(std::clamp(std::floor(bounds.height / height), 1.0F, 1000.0F)),
      };
      if (session_->requested_size != session_->snapshot.size && !resize_pending_) {
        resize_pending_ = true;
        std::weak_ptr<Session> weak = session_;
        resize_task_ = spec_.tasks.Launch([this, weak]() -> Task<void> {
          resize_pending_ = false;
          if (auto session = weak.lock()) session->Resize(session->requested_size);
          co_return;
        });
      }
    }
    BuildDraft();
    dirty_ = false;
    return PaintInvalidation::Foreground;
  }

  void OnFocusChanged(ViewNode&, bool focused, bool) override {
    if (!attached_) return;
    focused_ = focused;
    session_->Focus(focused);
    Refresh();
  }

  bool Wheel(const ScrollInputEvent& event) {
    if (event.delta_y == 0) return false;
    if (session_->mouse_mode && !event.modifiers.shift && !session_->viewport_top && last_mouse_) {
      ReportMouse(*last_mouse_, event.modifiers);
      const auto button = event.delta_y < 0 ? 4 : 5;
      vterm_mouse_button(session_->terminal.get(), button, true, detail::Modifiers(event.modifiers));
      vterm_mouse_button(session_->terminal.get(), button, false, detail::Modifiers(event.modifiers));
      return true;
    }
    if (session_->alternate) return false;
    const auto previous = Top();
    Scroll(event.delta_y < 0 ? -3 : 3);
    return Top() != previous;
  }

  bool HoverHitTest(ViewNode&, Point point) const override { return attached_ && bounds_.Contains(point); }

  void OnHover(ViewNode&, const HoverEvent& event) override {
    if (event.type == HoverEventType::Leave) {
      last_mouse_.reset();
      return;
    }
    last_mouse_ = Hit(event.position);
    if (session_->mouse_mode == VTERM_PROP_MOUSE_MOVE && !session_->viewport_top && !pointer_) {
      ReportMouse(*last_mouse_, {});
    }
  }

  bool HitTest(ViewNode&, Point point) const override { return attached_ && bounds_.Contains(point); }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    if (!attached_) return PointerResult::Ignored;
    const bool touch = event.device_kind == PointerDeviceKind::Touch;
    if (touch && event.type == PointerEventType::Down) last_mouse_.reset();
    if (event.type == PointerEventType::Down && (touch || event.changed_button == PointerButton::Primary)) {
      if (pointer_) return PointerResult::Ignored;
      if (!touch) spec_.focus.RequestFocus();
      pointer_ = event.pointer_id;
      down_ = event.position;
      touch_moved_ = false;
      touch_selected_ = false;
      report_mouse_ = !touch && session_->mouse_mode && !event.modifiers.shift && !session_->viewport_top;
      const auto cell = Hit(event.position);
      if (report_mouse_) {
        ReportMouse(cell, event.modifiers);
        vterm_mouse_button(session_->terminal.get(), 1, true, detail::Modifiers(event.modifiers));
      } else if (!touch) {
        selection_.reset();
        if (event.modifiers.control || event.modifiers.meta) {
          if (const auto link = LinkAt(cell); !link.empty()) {
            EmitEvent<TerminalEvents::LinkActivated>(link);
            pointer_.reset();
            Refresh();
            return PointerResult::Handled;
          }
        }
      }
      Refresh();
      return PointerResult::Capture;
    }
    if (pointer_ != event.pointer_id) return PointerResult::Ignored;
    if (event.type == PointerEventType::Move) {
      if (touch) {
        if (touch_selected_) return PointerResult::Handled;
        const auto rows = static_cast<int>((down_.y - event.position.y) / height_);
        if (rows) {
          Scroll(rows);
          down_.y -= rows * height_;
          touch_moved_ = true;
        }
        return PointerResult::Handled;
      }
      const auto cell = Hit(event.position);
      if (report_mouse_) ReportMouse(cell, event.modifiers);
      else if (cell != Hit(down_) || selection_) selection_ = std::pair{Hit(down_), cell};
      Refresh();
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      pointer_.reset();
      if (report_mouse_) {
        vterm_mouse_button(session_->terminal.get(), 1, false, detail::Modifiers(event.modifiers));
        report_mouse_ = false;
      } else if (event.type == PointerEventType::Up && touch && !touch_moved_ && !touch_selected_) {
        spec_.focus.RequestFocus();
        ShowKeyboard();
      }
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  bool OnKey(ViewNode&, const KeyEvent& event) override {
    if (!attached_ || event.type != KeyEventType::Down) return false;
    const auto mods = event.modifiers;
    if ((mods.meta || (mods.control && mods.shift)) && event.key == Key::C) {
      if (HasSelection()) Copy();
      return true;
    }
    if ((mods.meta || mods.control) && event.key == Key::V) {
      if (!event.repeat && CanPaste()) ReadPaste();
      return true;
    }
    if (mods.shift && (event.key == Key::PageUp || event.key == Key::PageDown || event.key == Key::End)) {
      if (event.key == Key::End) {
        session_->viewport_top.reset();
        Refresh();
      } else {
        Scroll(event.key == Key::PageUp ? -session_->snapshot.size.rows : session_->snapshot.size.rows);
      }
      return true;
    }
    return input_->HandleTextKey(event) == TextInputKeyResult::Handled;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner({.role = SemanticRole::Generic, .label = "Terminal"});
    if (!attached_) return;
    for (int row = 0; row < session_->snapshot.size.rows; ++row) {
      if (Box(row, 0).y < bounds_.y) continue;
      const auto logical = Top() + row;
      auto text = CopyRange({logical, 0}, {logical, session_->snapshot.size.columns - 1});
      if (!text.empty()) {
        builder.AddChild(static_cast<std::uint64_t>(logical) + 1, Box(row, 0, session_->snapshot.size.columns),
                         {.role = SemanticRole::Text, .label = std::move(text)}, true);
      }
    }
    if (session_->commands && cursor_) {
      builder.AddChild(std::numeric_limits<std::uint64_t>::max(), *cursor_,
                       {.role = SemanticRole::Text, .label = session_->commands->prompt + std::string(input_->Text())},
                       true);
    }
  }

  TextEditingActionStatus GetTextEditingActionStatus(TextEditingAction action, const Clipboard&) const override {
    using Status = TextEditingActionStatus;
    if (action == TextEditingAction::Copy) return HasSelection() ? Status::Handle : Status::Disabled;
    if (action == TextEditingAction::Paste) return CanPaste() ? Status::Handle : Status::Disabled;
    return Status::Disabled;
  }

  void PerformTextEditingAction(TextEditingAction action, const Clipboard&) override {
    if (action == TextEditingAction::Copy && HasSelection()) Copy();
    else if (action == TextEditingAction::Paste && CanPaste()) ReadPaste();
  }

  bool ClearSelection() override {
    if (!selection_) return false;
    selection_.reset();
    Refresh();
    return true;
  }

  bool SelectWord(Point point) override {
    const auto cell = Hit(point);
    auto first = cell;
    auto last = cell;
    const auto is_word = [this](CellPosition position) {
      const auto scalar = session_->Cell(position.row, position.column).chars[0];
      return scalar && scalar != ' ';
    };
    if (is_word(cell)) {
      while (first.column > 0 && is_word({first.row, first.column - 1})) --first.column;
      while (last.column + 1 < session_->snapshot.size.columns &&
             is_word({last.row, last.column + 1})) ++last.column;
    }
    selection_ = std::pair{first, last};
    if (pointer_) touch_selected_ = true;
    Refresh();
    return true;
  }

  bool ExtendSelection(Point point, bool start_handle) override {
    if (!selection_) return false;
    auto [first, last] = Ordered();
    if (start_handle) first = std::min(Hit(point), last);
    else last = std::max(first, Hit(point));
    selection_ = std::pair{first, last};
    Refresh();
    return true;
  }

  std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
    if (!HasSelection()) return {};
    const auto [first, last] = Ordered();
    const auto rect = [this](CellPosition cell) -> std::optional<Rect> {
      if (cell.row < Top() || cell.row >= Top() + session_->snapshot.size.rows) return {};
      auto box = Box(static_cast<int>(cell.row - Top()), cell.column);
      if (box.y < bounds_.y) return {};
      box.width = 1;
      return box;
    };
    return TextSelectionGeometry{.start = rect(first), .end = rect({last.row, last.column + 1}),
                                 .toolbar_anchor = rect(std::max(first, CellPosition{Top(), 0}))};
  }

  Color SelectionHandleColor() const noexcept override { return spec_.palette.cursor; }

  void PaintAboveContent(const ViewNode& node, PaintContext& paint) const override {
    paint.DrawRect(node.Bounds(), spec_.palette.background);
    if (!attached_ || bounds_.width <= 0 || bounds_.height <= 0) return;
    paint.PushClip(bounds_);
    const auto top = Top();
    const auto [first, last] = Ordered();
    const int visible_columns = std::min(session_->snapshot.size.columns,
        static_cast<int>(std::min(1000.0F, std::ceil(bounds_.width / width_))));
    for (int row = draft_shift_; row < session_->snapshot.size.rows && Box(row, 0).y < bounds_.y + bounds_.height;
         ++row) {
      for (int col = 0; col < visible_columns;) {
        const auto cell = session_->Cell(top + row, col);
        const int span = std::max(1, static_cast<int>(cell.width));
        auto foreground = ColorFor(cell.fg, true);
        auto background = ColorFor(cell.bg, false);
        if (cell.attrs.reverse) std::swap(foreground, background);
        const CellPosition start{top + row, col};
        const CellPosition end{top + row, col + span - 1};
        if (HasSelection() && start <= last && end >= first) background = spec_.palette.selection;
        auto rect = Box(row, col, span);
        if (background != spec_.palette.background) paint.DrawRect(rect, background);
        TextStyle style{.font = spec_.font, .foreground = foreground};
        if (cell.attrs.bold) style.font = style.font.WithWeight(FontWeight::Bold);
        if (cell.attrs.italic) style.font = style.font.WithSlant(FontSlant::Italic);
        if (cell.attrs.underline) style.decoration = style.decoration | TextDecoration::Underline;
        if (cell.attrs.strike) style.decoration = style.decoration | TextDecoration::StrikeThrough;
        Draw(paint, rect, CellText(cell), style);
        col += span;
      }
    }
    for (const auto& glyph : draft_) {
      if (glyph.bounds.y < bounds_.y || glyph.bounds.y >= bounds_.y + bounds_.height) continue;
      if (glyph.selected) paint.DrawRect(glyph.bounds, spec_.palette.selection);
      Draw(paint, glyph.bounds, glyph.text,
           {.font = spec_.font, .foreground = glyph.prompt ? spec_.palette.ansi[4] : spec_.palette.foreground,
            .decoration = glyph.underlined ? TextDecoration::Underline : TextDecoration::None});
    }
    if (node.IsFocused() && session_->IsOpen() && !session_->viewport_top && cursor_) {
      auto rect = *cursor_;
      if (session_->cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
        rect.y += rect.height - 2;
        rect.height = 2;
      } else if (session_->cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
        rect.width = 2;
      }
      auto color = spec_.palette.cursor;
      if (session_->cursor_shape == VTERM_PROP_CURSORSHAPE_BLOCK) color.alpha = 0.65F;
      paint.DrawRect(rect, color);
    }
    if (session_->viewport_top && !session_->alternate) {
      const auto count = session_->ScreenRow() - session_->FirstRow() + session_->snapshot.size.rows;
      const auto ratio = static_cast<float>(session_->snapshot.size.rows) / static_cast<float>(count);
      const auto extent = std::min(bounds_.height, std::max(12.0F, bounds_.height * ratio));
      const auto progress = static_cast<float>(top - session_->FirstRow()) /
          static_cast<float>(std::max<std::int64_t>(1, session_->ScreenRow() - session_->FirstRow()));
      auto color = spec_.palette.cursor;
      color.alpha = 0.55F;
      paint.DrawRect({bounds_.x + bounds_.width - 3, bounds_.y + (bounds_.height - extent) * progress, 3, extent}, color);
    }
    paint.PopClip();
  }

private:
  struct Glyph {
    Rect bounds;
    std::string text;
    bool prompt;
    bool underlined;
    bool selected;
  };

  void Attach() {
    if (session_->update_view) throw std::logic_error("A terminal session already has an interactive view");
    attached_ = true;
    session_->update_view = [this] { Refresh(); };
    session_->scroll_view = [this](const ScrollInputEvent& event) { return Wheel(event); };
  }

  void Detach() {
    if (!std::exchange(attached_, false)) return;
    session_->update_view = {};
    session_->scroll_view = {};
    if (focused_) session_->Focus(false);
    input_->Detach();
    selection_.reset();
    focused_ = false;
    resize_task_.Cancel();
    resize_pending_ = false;
    applied_palette_.reset();
  }

  void Refresh() {
    if (selection_ && (selection_->first.row < session_->FirstRow() || selection_->second.row < session_->FirstRow())) {
      selection_.reset();
    }
    if (seen_alternate_ != session_->alternate) {
      seen_alternate_ = session_->alternate;
      selection_.reset();
    }
    dirty_ = true;
    InvalidatePaint();
    InvalidateSemantics();
  }

  void ReportMouse(CellPosition cell, KeyModifiers mods) {
    vterm_mouse_move(session_->terminal.get(), static_cast<int>(cell.row - session_->ScreenRow()), cell.column,
                     detail::Modifiers(mods));
  }

  void Scroll(int delta) {
    if (session_->alternate) return;
    const auto top = std::clamp(Top() + delta, session_->FirstRow(), session_->ScreenRow());
    if (top == session_->ScreenRow()) session_->viewport_top.reset();
    else session_->viewport_top = top;
    Refresh();
  }

  std::int64_t Top() const {
    return std::clamp(session_->viewport_top.value_or(session_->ScreenRow()), session_->FirstRow(), session_->ScreenRow());
  }

  Rect Box(int row, int column, int span = 1) const {
    return {bounds_.x + column * width_, bounds_.y + (row - draft_shift_) * height_, width_ * span, height_};
  }

  CellPosition Hit(Point point) const {
    const int row = static_cast<int>(std::floor((point.y - bounds_.y) / height_)) + draft_shift_;
    const int column = static_cast<int>(std::floor((point.x - bounds_.x) / width_));
    return {Top() + std::clamp(row, 0, session_->snapshot.size.rows - 1),
            std::clamp(column, 0, session_->snapshot.size.columns - 1)};
  }

  bool HasSelection() const { return selection_.has_value(); }

  bool CanPaste() const { return session_->IsOpen() && !session_->snapshot.active_command; }

  bool Copy() const {
    const auto clipboard = UseApplication().Clipboard();
    const auto [first, last] = Ordered();
    return clipboard && clipboard->WriteText(CopyRange(first, last));
  }

  void ReadPaste() {
    auto clipboard = UseApplication().Clipboard();
    if (!clipboard) return;
    std::weak_ptr<Input> weak = input_;
    const auto dialog = spec_.dialog;
    const auto paste = [weak, dialog](std::string text) {
      if (text.size() > 64 * 1024 || weak.expired()) return;
      if (text.find_first_of("\r\n") == std::string::npos) {
        if (auto input = weak.lock()) input->Paste(std::move(text));
        return;
      }
      const auto clusters = detail::Clusters(text);
      if (!clusters) return;
      const auto end = std::ranges::upper_bound(*clusters, std::size_t{4096}, {}, &detail::Cluster::end);
      const auto preview = end == clusters->begin() ? std::string{} : text.substr(0, std::prev(end)->end);
      dialog.Show("Paste multiple lines?", preview, "Paste", "Cancel", [weak, text = std::move(text)] {
        if (auto target = weak.lock()) target->Paste(text);
      });
    };
    spec_.tasks.Launch([clipboard, paste]() -> Task<void> {
      auto result = co_await clipboard->ReadTextAsync();
      if (result.Succeeded()) paste(std::move(result).Value());
    });
  }

  std::pair<CellPosition, CellPosition> Ordered() const {
    if (!selection_) return {};
    return std::minmax(selection_->first, selection_->second);
  }

  std::string CopyRange(CellPosition first, CellPosition last) const {
    std::string output;
    first.row = std::max(first.row, session_->FirstRow());
    for (auto row = first.row; row <= last.row; ++row) {
      std::string line;
      const int from = row == first.row ? first.column : 0;
      const bool wrapped = row != last.row && session_->Wrapped(row);
      const int to = row == last.row ? last.column : session_->Columns(row) - 1;
      std::size_t text_end = 0;
      for (int col = from; col <= to; ++col) {
        auto cell = session_->Cell(row, col);
        if (col == from && col && cell.chars[0] == Continuation) cell = session_->Cell(row, col - 1);
        else if (cell.chars[0] == Continuation) continue;
        const auto text = CellText(cell);
        line += text.empty() ? " " : text;
        if (!text.empty()) text_end = line.size();
      }
      if (wrapped) line.resize(text_end);
      else while (!line.empty() && line.back() == ' ') line.pop_back();
      output += line;
      if (row != last.row && !wrapped) output += '\n';
    }
    return output;
  }

  std::string LinkAt(CellPosition position) const {
    const auto read = [this, row = position.row](int col) {
      return session_->Cell(row, col).chars[0];
    };
    const auto delimiter = [](std::uint32_t scalar) {
      return scalar <= ' ' || scalar == '"' || scalar == '\'' || scalar == '<' || scalar == '>';
    };
    if (delimiter(read(position.column))) return {};
    int first = position.column;
    int last = position.column;
    while (first > 0 && !delimiter(read(first - 1))) --first;
    while (last + 1 < session_->snapshot.size.columns && !delimiter(read(last + 1))) ++last;
    auto text = CopyRange({position.row, first}, {position.row, last});
    if (!text.empty() && (text.front() == '(' || text.front() == '[')) text.erase(0, 1);
    while (!text.empty() && std::string_view(").,;]}").find(text.back()) != std::string_view::npos) text.pop_back();
    const auto uri = Uri::Parse(text);
    if (!uri || (uri->Scheme() != "https" && uri->Scheme() != "http") || !uri->Authority() ||
        uri->Authority()->empty()) return {};
    return text;
  }

  Color ColorFor(VTermColor color, bool foreground) const {
    if ((foreground && VTERM_COLOR_IS_DEFAULT_FG(&color)) || (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&color))) {
      return foreground ? spec_.palette.foreground : spec_.palette.background;
    }
    if (VTERM_COLOR_IS_INDEXED(&color) && color.indexed.idx < spec_.palette.ansi.size()) {
      return spec_.palette.ansi[color.indexed.idx];
    }
    vterm_screen_convert_color_to_rgb(session_->screen, &color);
    return Color::Rgb(color.rgb.red, color.rgb.green, color.rgb.blue);
  }

  void Draw(PaintContext& paint, Rect rect, std::string text, const TextStyle& style) const {
    if (text.empty() || text == " ") return;
    paint.PushClip(rect);
    rect.y += std::max(0.0F, (height_ - text_height_) / 2);
    rect.width += width_;
    paint.DrawText(rect, std::move(text), style, LayoutOptions);
    paint.PopClip();
  }

  void BuildDraft() {
    draft_.clear();
    cursor_.reset();
    draft_shift_ = 0;
    std::vector<std::pair<TextOffset, Rect>> geometry;
    const auto cursor = session_->Cursor();
    if (session_->viewport_top || !session_->IsOpen() || session_->snapshot.active_command) {
      input_->SetGeometry({});
      return;
    }
    int row = cursor.row;
    int col = cursor.col;
    const int columns = session_->snapshot.size.columns;
    auto advance = [&](int width) {
      if (col + width > columns) { ++row; col = 0; }
    };
    auto append = [&](std::string_view text, bool prompt) {
      const auto clusters = detail::Clusters(text);
      if (!clusters) return;
      const auto input_state = input_->State();
      const auto selection = input_state.selection.Range();
      for (const auto& cluster : *clusters) {
        const int width = std::clamp(cluster.columns, 1, columns);
        advance(width);
        if (!prompt) geometry.emplace_back(cluster.offset, Box(row, col));
        const bool composing = !prompt && input_state.composition &&
            cluster.offset < input_state.composition->end &&
            cluster.offset + cluster.length > input_state.composition->start;
        const bool selected = !prompt && cluster.offset < selection.end && cluster.offset + cluster.length > selection.start;
        draft_.push_back({Box(row, col, width), std::string(text.substr(cluster.start, cluster.end - cluster.start)),
                          prompt, composing, selected});
        col += width;
      }
    };
    if (session_->commands) append(session_->commands->prompt, true);
    append(input_->Text(), false);
    advance(1);
    geometry.emplace_back(detail::TextLength(input_->Text()), Box(row, col));
    const auto selected = std::ranges::lower_bound(geometry, input_->Selection().active, {},
                                                  &std::pair<TextOffset, Rect>::first);
    if (selected != geometry.end() && (session_->commands || session_->cursor_visible)) cursor_ = selected->second;
    if (cursor_) {
      draft_shift_ = std::max(0, static_cast<int>(std::lround((cursor_->y - bounds_.y) / height_)) -
                                session_->snapshot.size.rows + 1);
      const float shift = draft_shift_ * height_;
      for (auto& glyph : draft_) glyph.bounds.y -= shift;
      for (auto& point : geometry) point.second.y -= shift;
      cursor_->y -= shift;
    }
    input_->SetGeometry(std::move(geometry));
  }

  std::shared_ptr<Session> session_;
  std::shared_ptr<Input> input_;
  Surface spec_;
  Rect bounds_;
  float width_ = 1;
  float height_ = 1;
  float text_height_ = 1;
  bool attached_ = false;
  bool focused_ = false;
  bool dirty_ = true;
  TaskHandle resize_task_;
  bool resize_pending_ = false;
  int draft_shift_ = 0;
  bool seen_alternate_ = false;
  bool touch_moved_ = false;
  bool touch_selected_ = false;
  bool report_mouse_ = false;
  std::optional<std::int64_t> pointer_;
  std::optional<CellPosition> last_mouse_;
  Point down_;
  std::optional<std::pair<CellPosition, CellPosition>> selection_;
  std::optional<Rect> cursor_;
  std::optional<TerminalPalette> applied_palette_;
  std::vector<Glyph> draft_;
};

} // namespace

[[huxerui::composable]]
View Terminal(TerminalSession session, TerminalStyle style) {
  auto model = detail::Access::Model(session);
  if (!model || !std::isfinite(style.line_height_scale) || style.line_height_scale <= 0) {
    throw std::invalid_argument("Invalid Terminal view configuration");
  }
  model->CheckThread();
  const auto palette = ResolvePalette(UseTheme(), style);
  const auto focus = UseViewHandle();
  return Canvas([](PaintContext&, Size) {})
      .With(Focusable{}, focus, Padding(10.0F),
            Surface{model, style.font, style.line_height_scale, palette, focus, UseTaskScope(), UseDialog()})
      .On<ViewEvents::KeyIntercept>([model](const KeyEvent& event) {
        const auto mods = event.modifiers;
        return event.type == KeyEventType::Down && event.key == Key::C && mods.control &&
            !mods.shift && !mods.meta && !mods.alt && !model->composing && model->Interrupt();
      })
      .On<ViewEvents::ScrollInput>([model](const ScrollInputEvent& event) {
        return model->scroll_view && model->scroll_view(event);
      });
}

} // namespace huxerui::terminal
