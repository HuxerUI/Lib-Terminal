#include "input.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace huxerui::terminal::detail {
namespace {

constexpr std::size_t MaxDraftBytes = 64 * 1024;

bool ValidRange(std::string_view text, TextRange range) {
  return range.start <= range.end && ByteOffset(text, range.start) && ByteOffset(text, range.end);
}

bool ValidInput(std::string_view text) {
  return Clusters(text) && std::ranges::none_of(text, [](unsigned char ch) {
    return ch < 0x20 || ch == 0x7f;
  });
}

bool Replace(std::string& text, TextRange range, std::string_view replacement) {
  if (!ValidRange(text, range) || !Clusters(replacement)) return false;
  const auto first = *ByteOffset(text, range.start);
  const auto last = *ByteOffset(text, range.end);
  if (text.size() - (last - first) + replacement.size() > MaxDraftBytes) return false;
  text.replace(first, last - first, replacement);
  return true;
}

VTermKey TerminalKey(Key key) {
  if (key >= Key::F1 && key <= Key::F24) {
    return static_cast<VTermKey>(VTERM_KEY_FUNCTION(1 + static_cast<int>(key) - static_cast<int>(Key::F1)));
  }
  if (key >= Key::Numpad0 && key <= Key::Numpad9) {
    return static_cast<VTermKey>(VTERM_KEY_KP_0 + static_cast<int>(key) - static_cast<int>(Key::Numpad0));
  }
  switch (key) {
  case Key::Enter: return VTERM_KEY_ENTER;
  case Key::NumpadEnter: return VTERM_KEY_KP_ENTER;
  case Key::Tab: return VTERM_KEY_TAB;
  case Key::Backspace: return VTERM_KEY_BACKSPACE;
  case Key::Escape: return VTERM_KEY_ESCAPE;
  case Key::ArrowUp: return VTERM_KEY_UP;
  case Key::ArrowDown: return VTERM_KEY_DOWN;
  case Key::ArrowLeft: return VTERM_KEY_LEFT;
  case Key::ArrowRight: return VTERM_KEY_RIGHT;
  case Key::Insert: return VTERM_KEY_INS;
  case Key::Delete: return VTERM_KEY_DEL;
  case Key::Home: return VTERM_KEY_HOME;
  case Key::End: return VTERM_KEY_END;
  case Key::PageUp: return VTERM_KEY_PAGEUP;
  case Key::PageDown: return VTERM_KEY_PAGEDOWN;
  default: return VTERM_KEY_NONE;
  }
}

TextOffset Adjacent(std::string_view text, TextOffset offset, bool forward) {
  const auto clusters = Clusters(text);
  if (!clusters || clusters->empty()) return 0;
  for (const auto& cluster : *clusters) {
    const int end = cluster.offset + cluster.length;
    if (forward && end > offset) return end;
    if (!forward && end >= offset) return cluster.offset;
  }
  return TextLength(text);
}

} // namespace

TextInputConfiguration Input::Configuration() const {
  return {.action = TextInputAction::Go, .autocorrect = false, .read_only = !session_->IsOpen(),
          .keyboard_policy = SoftwareKeyboardPolicy::Manual};
}

std::string_view Input::Text() const { return session_->commands ? session_->draft : preedit_; }

TextSelection Input::Selection() const { return session_->commands ? session_->draft_selection : preedit_selection_; }

TextInputState Input::State() const {
  const auto selection = Selection();
  if (last_text_ != Text()) {
    last_text_ = Text();
    ++content_revision_;
    ++revision_;
  }
  if (last_selection_ != selection) {
    last_selection_ = selection;
    ++revision_;
  }
  return {id_, revision_, content_revision_, selection, composition_};
}

TextInputState Input::BeginTextInput(TextInputSessionId id) {
  id_ = id;
  return State();
}

void Input::Update() {
  ++revision_;
  if (session_->update_view) session_->update_view();
}

void Input::Forward(std::string_view text) {
  if (!text.empty()) session_->Queue(text);
}

TextInputApplyResult Input::ApplyTextInput(const TextInputCommandBatch& batch) {
  using Kind = TextInputCommandKind;
  if (!Matches(batch.session_id)) return {TextInputResultCode::SessionMismatch};
  if (!session_->IsOpen() || session_->snapshot.active_command) return {TextInputResultCode::ReadOnly};
  if (batch.commands.empty()) return {};
  auto text = std::string(Text());
  auto selection = Selection();
  auto composition = composition_;
  auto baseline = baseline_;
  std::string committed;

  for (const auto& operation : batch.commands) {
    auto range = operation.target.value_or(composition.value_or(selection.Range()));
    if (operation.coordinate_space == TextInputCoordinateSpace::Composition) {
      if (!composition || !operation.target || operation.kind != Kind::UpdateComposition ||
          range.start < 0 || range.end < range.start || range.end > composition->end - composition->start) return {};
      range.start += composition->start;
      range.end += composition->start;
    }
    switch (operation.kind) {
    case Kind::SetSelection:
      if (!operation.selection_after || operation.target || !operation.text.empty()) return {};
      break;
    case Kind::BeginComposition:
      if (composition || !operation.target || !ValidRange(text, range) || !operation.text.empty() ||
          operation.selection_after) return {};
      baseline = TextEditingValue{.text = text, .selection = selection};
      composition = range;
      break;
    case Kind::UpdateComposition:
    case Kind::CommitText: {
      if (composition && operation.target && (operation.kind == Kind::CommitText ||
          operation.coordinate_space != TextInputCoordinateSpace::Composition)) return {};
      if (operation.kind == Kind::UpdateComposition && !composition) {
        baseline = TextEditingValue{.text = text, .selection = selection};
        composition = range;
      }
      if (!ValidInput(operation.text) || !Replace(text, range, operation.text)) return {};
      const auto end = range.start + TextLength(operation.text);
      selection = {end, end};
      if (operation.kind == Kind::UpdateComposition) {
        composition->end += end - range.end;
      } else {
        composition.reset();
        baseline.reset();
        if (!session_->commands) committed += text;
      }
      break;
    }
    case Kind::CancelComposition:
      if (baseline) {
        text = std::move(baseline->text);
        selection = baseline->selection;
      }
      [[fallthrough]];
    case Kind::FinishComposition:
      if (operation.kind == Kind::FinishComposition && composition && !session_->commands) committed += text;
      composition.reset();
      baseline.reset();
      break;
    case Kind::DeleteSurrounding: {
      auto selected = selection.Range();
      if (operation.delete_before < 0 || operation.delete_after < 0 ||
          (composition && (operation.delete_before != 0 || operation.delete_after != 0))) return {};
      const auto length = TextLength(text);
      TextOffset first = selected.start;
      TextOffset last = selected.end;
      if (operation.delete_unit == TextInputUnit::UnicodeCodePoint) {
        for (auto count = operation.delete_before; first > 0 && count > 0; --count) {
          do { --first; } while (first > 0 && !ByteOffset(text, first));
        }
        for (auto count = operation.delete_after; last < length && count > 0; --count) {
          do { ++last; } while (last < length && !ByteOffset(text, last));
        }
      } else {
        first -= std::min(first, operation.delete_before);
        last += std::min(length - last, operation.delete_after);
      }
      if (!Replace(text, {selected.end, last}, {}) || !Replace(text, {first, selected.start}, {})) return {};
      selection.anchor -= selected.start - first;
      selection.active -= selected.start - first;
      break;
    }
    }
    if (operation.selection_after) selection = *operation.selection_after;
    if (!ValidRange(text, selection.Range())) return {};
    if (!session_->commands && !composition) {
      text.clear();
      selection = {};
    }
  }

  composition_ = composition;
  session_->composing = composition_.has_value();
  baseline_ = std::move(baseline);
  if (text != Text() || !committed.empty()) session_->viewport_top.reset();
  if (session_->commands) {
    session_->draft = std::move(text);
    session_->draft_selection = selection;
  } else {
    preedit_ = std::move(text);
    preedit_selection_ = selection;
    Forward(committed);
  }
  Update();
  return {TextInputResultCode::Ok};
}

TextInputContext Input::QueryTextInputContext(TextInputSessionId id, TextOffset start, TextOffset length) const {
  if (!Matches(id)) return {TextInputResultCode::SessionMismatch, id};
  if (start < 0 || length < 0) return {TextInputResultCode::Rejected, id};
  return {TextInputResultCode::Ok, id, 0, TextLength(Text()), std::string(Text()), Selection(), composition_};
}

TextInputGeometry Input::QueryTextInputGeometry(TextInputSessionId id, TextRange range) const {
  if (!Matches(id)) return {TextInputResultCode::SessionMismatch, id};
  if (!ValidRange(Text(), range) || geometry_.empty()) return {TextInputResultCode::Rejected, id};
  const auto end = std::ranges::lower_bound(geometry_, range.end, {}, &std::pair<TextOffset, Rect>::first);
  if (end == geometry_.end()) return {TextInputResultCode::Rejected, id};
  TextInputGeometry result{TextInputResultCode::Ok, id, end->second};
  auto start = std::ranges::lower_bound(geometry_, range.start, {}, &std::pair<TextOffset, Rect>::first);
  for (; start != end; ++start) {
    auto rect = start->second;
    const auto next = std::next(start)->second;
    rect.width = next.y == rect.y ? std::max(1.0F, next.x - rect.x) : rect.height / 2;
    result.range_rects.push_back(rect);
  }
  return result;
}

TextInputPositionResult Input::QueryTextInputPosition(TextInputSessionId id, Point point) const {
  if (!Matches(id)) return {TextInputResultCode::SessionMismatch, id};
  if (geometry_.empty()) return {TextInputResultCode::Rejected, id};
  const auto distance = [point](const auto& cell) {
    return std::abs(point.x - cell.second.x) + 4 * std::abs(point.y - cell.second.y);
  };
  const auto found = std::ranges::min_element(geometry_, {}, distance);
  return {TextInputResultCode::Ok, id, {found->first}};
}

void Input::SetGeometry(std::vector<std::pair<TextOffset, Rect>> geometry) {
  if (geometry_ != geometry) {
    geometry_ = std::move(geometry);
    ++revision_;
  }
}

TextInputKeyResult Input::HandleTextKey(const KeyEvent& event) {
  constexpr auto handled = TextInputKeyResult::Handled;
  constexpr auto unhandled = TextInputKeyResult::Unhandled;
  if (event.type != KeyEventType::Down || !session_->IsOpen()) return unhandled;
  const auto mods = event.modifiers;
  if (mods.meta || (mods.control && mods.shift)) return unhandled;
  if (composition_) {
    if (event.key == Key::Escape && id_) {
      ApplyTextInput({id_, {{.kind = TextInputCommandKind::CancelComposition}}});
      return handled;
    }
    return unhandled;
  }
  if (mods.shift && (event.key == Key::PageUp || event.key == Key::PageDown || event.key == Key::End)) {
    return unhandled;
  }
  if (session_->commands && event.key == Key::Tab && mods.shift) return unhandled;
  if (event.key == Key::C && mods.control && !mods.alt) {
    session_->Interrupt();
    return handled;
  }
  if (mods.control && event.key == Key::V) return unhandled;
  session_->viewport_top.reset();
  if (!session_->commands) {
    const auto key = TerminalKey(event.key);
    if (key != VTERM_KEY_NONE) {
      vterm_keyboard_key(session_->terminal.get(), key, Modifiers(mods));
    } else if (mods.control && event.key >= Key::A && event.key <= Key::Z) {
      vterm_keyboard_unichar(session_->terminal.get(), 'a' + static_cast<int>(event.key) - static_cast<int>(Key::A),
                             Modifiers(mods));
    } else if (mods.control && event.key == Key::Space) {
      vterm_keyboard_unichar(session_->terminal.get(), ' ', Modifiers(mods));
    } else if (!event.text.empty() && !mods.control) {
      if (mods.alt) Forward("\x1b");
      Forward(event.text);
    } else {
      return unhandled;
    }
    Update();
    return handled;
  }
  if (session_->snapshot.active_command) return unhandled;
  auto& text = session_->draft;
  auto& selection = session_->draft_selection;
  const auto range = selection.Range();
  if (event.key == Key::Enter || event.key == Key::NumpadEnter) {
    if (!event.repeat) session_->Submit();
  } else if (event.key == Key::Tab && !mods.control && !mods.alt) {
    if (!event.repeat) session_->Complete();
  } else if (event.key == Key::ArrowUp || event.key == Key::ArrowDown) {
    session_->Recall(event.key == Key::ArrowUp ? -1 : 1);
  } else if (event.key == Key::ArrowLeft || event.key == Key::ArrowRight || event.key == Key::Home ||
             event.key == Key::End || (mods.control && (event.key == Key::A || event.key == Key::E))) {
    TextOffset position;
    if (event.key == Key::Home || event.key == Key::A) position = 0;
    else if (event.key == Key::End || event.key == Key::E) position = TextLength(text);
    else {
      const bool forward = event.key == Key::ArrowRight;
      position = !mods.shift && !selection.IsCollapsed() ? (forward ? range.end : range.start)
                                                        : Adjacent(text, selection.active, forward);
    }
    selection = {mods.shift ? selection.anchor : position, position};
  } else if (event.key == Key::Backspace || event.key == Key::Delete || (mods.control && event.key == Key::D)) {
    auto target = range;
    if (selection.IsCollapsed()) {
      const bool forward = event.key != Key::Backspace;
      const auto adjacent = Adjacent(text, selection.active, forward);
      target = {std::min(adjacent, selection.active), std::max(adjacent, selection.active)};
    }
    Replace(text, target, {});
    selection = {target.start, target.start};
  } else if (!event.text.empty() && !mods.control && !mods.alt && ValidInput(event.text)) {
    if (Replace(text, range, event.text)) {
      const auto end = range.start + TextLength(event.text);
      selection = {end, end};
    }
  } else {
    return unhandled;
  }
  Update();
  return handled;
}

bool Input::Paste(std::string text) {
  if (!session_->IsOpen() || session_->snapshot.active_command || composition_ ||
      text.size() > MaxDraftBytes || !Clusters(text)) return false;
  if (session_->commands) {
    std::ranges::replace(text, '\n', ' ');
    std::ranges::replace(text, '\r', ' ');
    std::ranges::replace(text, '\t', ' ');
    if (!ValidInput(text)) return false;
    const auto range = session_->draft_selection.Range();
    if (!Replace(session_->draft, range, text)) return false;
    const auto end = range.start + TextLength(text);
    session_->draft_selection = {end, end};
  } else {
    std::erase_if(text, [](unsigned char character) {
      return (character < 0x20 && character != '\n' && character != '\r' && character != '\t') || character == 0x7f;
    });
    vterm_keyboard_start_paste(session_->terminal.get());
    Forward(text);
    vterm_keyboard_end_paste(session_->terminal.get());
  }
  session_->viewport_top.reset();
  Update();
  return true;
}

void Input::EndTextInput(TextInputSessionId id, TextInputEndReason) {
  if (!Matches(id)) return;
  if (composition_ && !session_->commands && session_->IsOpen()) Forward(preedit_);
  id_ = 0;
  composition_.reset();
  session_->composing = false;
  baseline_.reset();
  preedit_.clear();
  preedit_selection_ = {};
  Update();
}

} // namespace huxerui::terminal::detail
