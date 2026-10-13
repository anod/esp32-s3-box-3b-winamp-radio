#pragma once
// Header-only UTF-8 word wrap + simplified bidi (Hebrew) reordering.
// No ESP/LovyanGFX dependencies so it can be unit-tested on the host.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace esphome {
namespace winamp_display {
namespace text {

inline uint32_t decode(const char *&p, const char *end) {
  uint8_t c = (uint8_t) *p++;
  if (c < 0x80) return c;
  int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
  uint32_t cp = (extra == 3) ? (c & 0x07) : (extra == 2) ? (c & 0x0F) : (extra == 1) ? (c & 0x1F) : 0xFFFD;
  while (extra-- > 0 && p < end && ((uint8_t) *p & 0xC0) == 0x80) cp = (cp << 6) | ((uint8_t) *p++ & 0x3F);
  return cp;
}

inline size_t encode(uint32_t cp, char *out) {
  if (cp < 0x80) { out[0] = (char) cp; return 1; }
  if (cp < 0x800) { out[0] = (char) (0xC0 | (cp >> 6)); out[1] = (char) (0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char) (0xE0 | (cp >> 12)); out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F)); return 3;
  }
  out[0] = (char) (0xF0 | (cp >> 18)); out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((cp >> 6) & 0x3F)); out[3] = (char) (0x80 | (cp & 0x3F)); return 4;
}

enum Dir : uint8_t { NEUTRAL = 0, LTR = 1, RTL = 2 };

inline Dir cp_dir(uint32_t cp) {
  if (cp >= 0x590 && cp <= 0x5FF) return RTL;
  if (cp >= 0xFB1D && cp <= 0xFB4F) return RTL;
  if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return LTR;
  if (cp >= 0x400 && cp <= 0x52F) return LTR;
  return NEUTRAL;
}

inline uint32_t mirror(uint32_t cp) {
  switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    default: return cp;
  }
}

// True if the first strong character is Hebrew.
inline bool is_rtl_paragraph(const char *s, size_t len) {
  const char *p = s, *end = s + len;
  while (p < end) {
    Dir d = cp_dir(decode(p, end));
    if (d != NEUTRAL) return d == RTL;
  }
  return false;
}

static constexpr size_t MAX_LINE_CPS = 192;

// Convert one logical line into visual order for left-to-right drawing.
// Returns bytes written (NUL-terminated if room).
inline size_t reorder(const char *in, size_t len, bool para_rtl, char *out, size_t out_sz) {
  uint32_t cps[MAX_LINE_CPS];
  uint8_t dir[MAX_LINE_CPS];
  size_t n = 0;
  const char *p = in, *end = in + len;
  while (p < end && n < MAX_LINE_CPS) {
    cps[n] = decode(p, end);
    dir[n] = cp_dir(cps[n]);
    n++;
  }
  const uint8_t para = para_rtl ? RTL : LTR;
  // Neutrals between two equal strong directions take it; otherwise the paragraph direction.
  for (size_t i = 0; i < n;) {
    if (dir[i] != NEUTRAL) { i++; continue; }
    size_t j = i;
    while (j < n && dir[j] == NEUTRAL) j++;
    uint8_t before = (i > 0) ? dir[i - 1] : para;
    uint8_t after = (j < n) ? dir[j] : para;
    uint8_t res = (before == after) ? before : para;
    for (size_t k = i; k < j; k++) dir[k] = res;
    i = j;
  }
  size_t o = 0;
  auto emit = [&](uint32_t cp) {
    char tmp[4];
    size_t l = encode(cp, tmp);
    if (o + l < out_sz) { memcpy(out + o, tmp, l); o += l; }
  };
  // Walk runs; in an RTL paragraph runs are emitted last-to-first.
  size_t run_end = n;
  size_t run_starts[MAX_LINE_CPS / 2 + 1];
  size_t nruns = 0;
  for (size_t i = 0; i < n; i++)
    if (i == 0 || dir[i] != dir[i - 1]) run_starts[nruns++] = i;
  for (size_t r = 0; r < nruns; r++) {
    size_t idx = para_rtl ? nruns - 1 - r : r;
    size_t s = run_starts[idx];
    size_t e = (idx + 1 < nruns) ? run_starts[idx + 1] : run_end;
    if (dir[s] == RTL)
      for (size_t k = e; k-- > s;) emit(mirror(cps[k]));
    else
      for (size_t k = s; k < e; k++) emit(cps[k]);
  }
  if (o < out_sz) out[o] = '\0';
  return o;
}

struct Span {
  uint16_t off;
  uint16_t len;
};

// Greedy word wrap of `s` into at most max_lines spans, each measuring <= max_w.
// measure(ptr, len) returns pixel width. Returns line count; sets truncated.
template <typename Measure>
inline size_t wrap(const char *s, size_t len, int max_w, Measure measure, Span *lines, size_t max_lines,
                   bool *truncated) {
  *truncated = false;
  size_t n = 0;
  size_t pos = 0;
  while (pos < len && s[pos] == ' ') pos++;
  while (pos < len) {
    if (n == max_lines) { *truncated = true; break; }
    size_t line_start = pos, line_end = pos;
    while (pos < len) {
      size_t w_end = pos;
      while (w_end < len && s[w_end] != ' ') w_end++;
      if (measure(s + line_start, w_end - line_start) <= max_w) {
        line_end = w_end;
        pos = w_end;
        while (pos < len && s[pos] == ' ') pos++;
        continue;
      }
      if (line_end == line_start) {
        // Single word wider than a line: hard-break on codepoint boundaries.
        const char *p = s + pos, *end = s + w_end;
        size_t last_ok = pos;
        while (p < end) {
          const char *q = p;
          decode(q, end);
          if (measure(s + line_start, (size_t) (q - s) - line_start) > max_w && last_ok > line_start) break;
          last_ok = (size_t) (q - s);
          p = q;
        }
        line_end = last_ok;
        pos = last_ok;
      }
      break;
    }
    lines[n].off = (uint16_t) line_start;
    lines[n].len = (uint16_t) (line_end - line_start);
    n++;
  }
  return n;
}

}  // namespace text
}  // namespace winamp_display
}  // namespace esphome
