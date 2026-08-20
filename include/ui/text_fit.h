#pragma once

#include <cstddef>
#include <cstring>

namespace ui::text {

/** U+2026 HORIZONTAL ELLIPSIS, the glyph appended to truncated strings. */
constexpr char kEllipsis[] = "\xE2\x80\xA6";
constexpr size_t kEllipsisLen = sizeof(kEllipsis) - 1;

inline bool isUtf8Continuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

/**
 * Write up to `max_src_bytes` bytes of `src` followed by an ellipsis into `buf`.
 *
 * The cut is snapped back to a UTF-8 character boundary and the ellipsis is
 * written whole or not at all, so this never *introduces* a torn multi-byte
 * sequence -- the font renders those as replacement junk. Bytes copied from
 * `src` are passed through as-is; already-malformed input stays malformed.
 *
 * Returns the number of source bytes actually copied (0 means `buf` holds a
 * bare ellipsis), or -1 if `buf` is too small to hold even that.
 */
inline int buildEllipsized(char* buf, size_t buf_len, const char* src,
                           size_t max_src_bytes) {
  if (buf == nullptr || buf_len < kEllipsisLen + 1) {
    return -1;
  }

  const size_t src_len = (src == nullptr) ? 0 : strlen(src);
  size_t n = (max_src_bytes < src_len) ? max_src_bytes : src_len;
  const size_t room = buf_len - kEllipsisLen - 1;
  if (n > room) {
    n = room;
  }
  // src[n] is the first dropped byte; a continuation byte there means the cut
  // landed inside a character, so walk back to its start.
  while (n > 0 && isUtf8Continuation(src[n])) {
    --n;
  }

  memcpy(buf, src, n);
  memcpy(buf + n, kEllipsis, kEllipsisLen);
  buf[n + kEllipsisLen] = '\0';
  return static_cast<int>(n);
}

}  // namespace ui::text
