#include <cassert>
#include <cstdio>
#include <string>
#include "../../components/winamp_display/text_layout.h"

using namespace esphome::winamp_display::text;

static int w(const char *p, size_t n) { (void) p; size_t c = 0; const char *e = p + n; while (p < e) { decode(p, e); c++; } return (int) c; }

static std::string vis(const char *s, bool rtl) {
  char out[256];
  reorder(s, strlen(s), rtl, out, sizeof(out));
  return out;
}

int main() {
  // Hebrew word is reversed; the paragraph is RTL.
  assert(is_rtl_paragraph("שלום", 8));
  assert(!is_rtl_paragraph("hello", 5));
  assert(!is_rtl_paragraph("123 !? привет", 17));
  assert(vis("שלום", true) == "םולש");
  assert(vis("abc", false) == "abc");
  assert(vis("привет", false) == "привет");
  // RTL paragraph with embedded Latin keeps the Latin word LTR and moves it to the left.
  assert(vis("שלום abc", true) == "abc םולש");
  // Neutral punctuation at the end of an RTL line moves to the left.
  assert(vis("שלום!", true) == "!םולש");
  // Mirrored brackets.
  assert(vis("(שלום)", true) == "(םולש)");
  // LTR paragraph with a Hebrew word in the middle.
  assert(vis("go שלום now", false) == "go םולש now");
  // UTF-8 round trip.
  { char b[4]; assert(encode(0x5D0, b) == 2); const char *p = b; assert(decode(p, b + 2) == 0x5D0); }
  { char b[4]; assert(encode(0x20AA, b) == 3); const char *p = b; assert(decode(p, b + 3) == 0x20AA); }

  // Wrapping (1 px per codepoint).
  Span lines[8];
  bool trunc;
  const char *t = "hello big world";
  size_t n = wrap(t, strlen(t), 9, w, lines, 8, &trunc);
  assert(n == 2 && !trunc);
  assert(std::string(t + lines[0].off, lines[0].len) == "hello big");
  assert(std::string(t + lines[1].off, lines[1].len) == "world");
  // Hard break of an over-long word.
  const char *l = "abcdefghij";
  n = wrap(l, strlen(l), 4, w, lines, 8, &trunc);
  assert(n == 3 && std::string(l + lines[0].off, lines[0].len) == "abcd" &&
         std::string(l + lines[2].off, lines[2].len) == "ij");
  // Truncation flag.
  n = wrap(l, strlen(l), 2, w, lines, 2, &trunc);
  assert(n == 2 && trunc);
  // Multi-byte words are never split mid-codepoint.
  const char *c = "привет мир";
  n = wrap(c, strlen(c), 4, w, lines, 8, &trunc);
  assert(n == 3);
  assert(std::string(c + lines[0].off, lines[0].len) == "прив");
  // Empty and whitespace-only.
  assert(wrap("", 0, 5, w, lines, 8, &trunc) == 0);
  assert(wrap("   ", 3, 5, w, lines, 8, &trunc) == 0);
  puts("text_layout tests passed");
  return 0;
}
