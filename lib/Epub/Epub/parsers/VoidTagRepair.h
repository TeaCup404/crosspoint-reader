#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Streaming repair for HTML void elements written without the XHTML self-closing slash,
// e.g. `<meta charset="utf-8">` or `<br>`. Expat is a strict XML parser: one such tag makes
// the matching parent end tag a "mismatched tag" error and the whole chapter fails to index.
// This filter rewrites `<br ...>` to `<br .../>` and drops explicit `</br>` end tags (which
// well-formed XHTML may use as `<br></br>`), so already-valid documents parse identically.
//
// Feed bytes in chunks; state carries across chunk boundaries. Output never exceeds
// input + MAX_GROWTH(input) bytes, which lets callers repair in place inside one buffer
// (read input at offset `maxGrowth(len)`, write output from offset 0).
class VoidTagRepair {
 public:
  static constexpr size_t MAX_PENDING = 18;  // "</" + longest void name we buffer + terminator slack

  // Upper bound on (output - input) for one chunk of `len` input bytes: pending bytes from the
  // previous chunk, plus at most one inserted '/' per 4 input bytes ("<br>"), plus one for a tag
  // that started in the previous chunk.
  static constexpr size_t maxGrowth(size_t len) { return MAX_PENDING + 8 + len / 4; }

  // Processes `len` bytes at `in`, writing to `out`. `out` may alias `in` - maxGrowth(len)
  // (i.e. output trails input by at least the growth bound). Returns bytes written.
  size_t process(const char* in, size_t len, char* out, bool final) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
      const char c = in[i];
      switch (state_) {
        case State::Text:
          if (c == '<') {
            pendLen_ = 0;
            pend_[pendLen_++] = c;
            isEnd_ = false;
            state_ = State::TagName;
          } else {
            out[o++] = c;
          }
          break;

        case State::TagName: {
          if (pendLen_ == 1 && c == '/' && !isEnd_) {
            isEnd_ = true;
            pend_[pendLen_++] = c;
            break;
          }
          const bool isNameChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
          const size_t nameStart = isEnd_ ? 2 : 1;
          if (isNameChar && pendLen_ < MAX_PENDING - 1) {
            pend_[pendLen_++] = c;
            break;
          }
          const bool terminator = c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
          const bool isVoid = terminator && isVoidName(pend_ + nameStart, pendLen_ - nameStart);
          if (isVoid && isEnd_) {
            // Drop the explicit end tag of a void element; we self-close the start tag instead.
            pendLen_ = 0;
            state_ = (c == '>') ? State::Text : State::SkipEndTag;
            break;
          }
          o = flushPending(out, o);
          if (isVoid) {
            state_ = State::VoidAttrs;
            quote_ = 0;
            lastNonSpace_ = 0;
            o = voidAttrsByte(c, out, o);
          } else {
            // Not a void element (or a comment/PI/DOCTYPE): copy through untouched.
            state_ = State::Text;
            if (c == '<') {
              pend_[pendLen_++] = c;
              isEnd_ = false;
              state_ = State::TagName;
            } else {
              out[o++] = c;
            }
          }
          break;
        }

        case State::VoidAttrs:
          o = voidAttrsByte(c, out, o);
          break;

        case State::SkipEndTag:
          if (c == '>') state_ = State::Text;
          break;
      }
    }
    if (final) {
      o = flushPending(out, o);
      state_ = State::Text;
    }
    return o;
  }

 private:
  enum class State : uint8_t { Text, TagName, VoidAttrs, SkipEndTag };

  static bool isVoidName(const char* name, size_t len) {
    static constexpr const char* VOID_TAGS[] = {"area", "base", "br",   "col",   "embed", "hr",    "img",
                                                "input", "link", "meta", "param", "source", "track", "wbr"};
    if (len == 0 || len > 6) return false;
    char lower[7];
    for (size_t i = 0; i < len; i++) {
      const char ch = name[i];
      lower[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    lower[len] = '\0';
    for (const char* tag : VOID_TAGS) {
      if (strcmp(lower, tag) == 0) return true;
    }
    return false;
  }

  size_t flushPending(char* out, size_t o) {
    for (size_t k = 0; k < pendLen_; k++) out[o++] = pend_[k];
    pendLen_ = 0;
    return o;
  }

  size_t voidAttrsByte(const char c, char* out, size_t o) {
    if (quote_) {
      if (c == quote_) quote_ = 0;
    } else if (c == '"' || c == '\'') {
      quote_ = c;
    } else if (c == '>') {
      if (lastNonSpace_ != '/') out[o++] = '/';
      out[o++] = '>';
      state_ = State::Text;
      return o;
    }
    out[o++] = c;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') lastNonSpace_ = c;
    return o;
  }

  State state_ = State::Text;
  bool isEnd_ = false;
  char quote_ = 0;
  char lastNonSpace_ = 0;
  size_t pendLen_ = 0;
  char pend_[MAX_PENDING];
};
