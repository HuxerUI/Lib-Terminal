#include "session.h"

#include <utf8proc.h>

namespace huxerui::terminal::detail {

std::optional<std::vector<Cluster>> Clusters(std::string_view text) {
  std::vector<Cluster> result;
  int offset = 0;
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t state = 0;
  for (std::size_t start = 0; start < text.size();) {
    utf8proc_int32_t scalar = 0;
    const auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + start),
                                        static_cast<utf8proc_ssize_t>(text.size() - start), &scalar);
    if (count <= 0) return std::nullopt;
    const auto end = start + static_cast<std::size_t>(count);
    const int units = scalar > 0xffff ? 2 : 1;
    const int columns = utf8proc_charwidth(scalar);
    if (result.empty() || utf8proc_grapheme_break_stateful(previous, scalar, &state)) {
      result.push_back({start, end, offset, units, columns});
    } else {
      auto& cluster = result.back();
      cluster.end = end;
      cluster.length += units;
      cluster.columns = std::max(cluster.columns, columns);
      if (scalar == 0xfe0f || (scalar >= 0x1f1e6 && scalar <= 0x1f1ff)) {
        cluster.columns = std::max(cluster.columns, 2);
      }
    }
    previous = scalar;
    offset += units;
    start = end;
  }
  return result;
}

std::optional<std::size_t> ByteOffset(std::string_view text, TextOffset offset) {
  if (offset < 0) return {};
  std::size_t index = 0;
  while (index < text.size() && offset > 0) {
    utf8proc_int32_t scalar = 0;
    auto count = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + index),
                                  static_cast<utf8proc_ssize_t>(text.size() - index), &scalar);
    if (count <= 0) return {};
    offset -= scalar > 0xffff ? 2 : 1;
    index += static_cast<std::size_t>(count);
  }
  return offset == 0 ? std::optional{index} : std::nullopt;
}

TextOffset TextLength(std::string_view text) {
  const auto clusters = Clusters(text);
  if (!clusters || clusters->empty()) return 0;
  return clusters->back().offset + clusters->back().length;
}

std::string Encode(std::uint32_t scalar) {
  utf8proc_uint8_t bytes[4];
  const auto size = utf8proc_encode_char(static_cast<utf8proc_int32_t>(scalar), bytes);
  return {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size)};
}

} // namespace huxerui::terminal::detail
