#pragma once

#include "session.h"

namespace huxerui::terminal::detail {

class Input final : public TextInputClient {
public:
  explicit Input(std::shared_ptr<Session> session) : session_(std::move(session)) {}

  TextInputConfiguration Configuration() const override;
  TextInputState State() const override;
  TextInputState BeginTextInput(TextInputSessionId id) override;
  TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) override;
  TextInputContext QueryTextInputContext(TextInputSessionId id, TextOffset start, TextOffset length) const override;
  TextInputGeometry QueryTextInputGeometry(TextInputSessionId id, TextRange range) const override;
  TextInputPositionResult QueryTextInputPosition(TextInputSessionId id, Point point) const override;
  TextInputKeyResult HandleTextKey(const KeyEvent& event) override;
  void EndTextInput(TextInputSessionId id, TextInputEndReason reason) override;

  std::string_view Text() const;
  TextSelection Selection() const;
  bool Paste(std::string text);
  void SetGeometry(std::vector<std::pair<TextOffset, Rect>> geometry);
  void Detach() { EndTextInput(id_, TextInputEndReason::ClientRemoved); }

private:
  bool Matches(TextInputSessionId id) const { return id != 0 && id_ == id; }
  void Update();
  void Forward(std::string_view text);

  std::shared_ptr<Session> session_;
  TextInputSessionId id_ = 0;
  mutable std::uint64_t revision_ = 0;
  mutable std::uint64_t content_revision_ = 0;
  mutable std::string last_text_;
  mutable TextSelection last_selection_{0, 0, TextAffinity::Downstream};
  std::string preedit_;
  TextSelection preedit_selection_{0, 0, TextAffinity::Downstream};
  std::optional<TextEditingValue> baseline_;
  std::optional<TextRange> composition_;
  std::vector<std::pair<TextOffset, Rect>> geometry_;
};

} // namespace huxerui::terminal::detail
