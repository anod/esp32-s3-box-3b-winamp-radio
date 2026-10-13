// Host unit tests for components/internet_radio/media_player/voice_logic.h
// Build/run: tests/run_host_tests.sh
#include <cstdio>
#include <cstdlib>
#include "../../components/internet_radio/media_player/voice_logic.h"

using namespace esphome::internet_radio::voice_logic;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_should_restore() {
  CHECK(should_restore(true, false, false, false, false));
  // Each blocker alone must prevent the restore
  CHECK(!should_restore(false, false, false, false, false));
  CHECK(!should_restore(true, true, false, false, false));    // announcing
  CHECK(!should_restore(true, false, true, false, false));    // queued announcement
  CHECK(!should_restore(true, false, false, true, false));    // mic requested (follow-up)
  CHECK(!should_restore(true, false, false, false, true));    // RX still enabled
}

static void test_gain() {
  int16_t a[4] = {1000, -1000, 32767, -32768};
  apply_gain_q8(a, 4, 256);  // unity: untouched
  CHECK(a[0] == 1000 && a[2] == 32767 && a[3] == -32768);

  int16_t b[4] = {1000, -1000, 32767, -32768};
  apply_gain_q8(b, 4, 128);  // 0.5
  CHECK(b[0] == 500 && b[1] == -500 && b[2] == 16383 && b[3] == -16384);

  int16_t c[2] = {20000, -20000};
  apply_gain_q8(c, 2, 512);  // 2x must saturate, not wrap
  CHECK(c[0] == 32767 && c[1] == -32768);

  int16_t d[2] = {1234, -1234};
  apply_gain_q8(d, 2, 0);  // mute
  CHECK(d[0] == 0 && (d[1] == 0 || d[1] == -1));

  int16_t e[1] = {32767};
  apply_gain_q8(e, 0, 81);  // zero length is a no-op
  CHECK(e[0] == 32767);

  // Voice reply gain (81/256 ≈ -10 dB)
  int16_t f[1] = {10000};
  apply_gain_q8(f, 1, 81);
  CHECK(f[0] == 3164);
}

static void test_extract_mono() {
  int16_t st[8] = {1, -10, 2, -20, 3, 30, -4, 40};
  int16_t mono[4];
  int peak[2] = {0, 0};
  extract_mono(st, 4, 0, mono, peak);
  CHECK(mono[0] == 1 && mono[1] == 2 && mono[2] == 3 && mono[3] == -4);
  CHECK(peak[0] == 4 && peak[1] == 40);

  extract_mono(st, 4, 1, mono, nullptr);
  CHECK(mono[0] == -10 && mono[1] == -20 && mono[2] == 30 && mono[3] == 40);

  int16_t edge[2] = {-32768, 32767};
  int pk[2] = {0, 0};
  int16_t m1[1];
  extract_mono(edge, 1, 0, m1, pk);
  CHECK(m1[0] == -32768 && pk[0] == 32768 && pk[1] == 32767);
}

static void test_level_pct() {
  // Log meter: idle noise reads 0, speech mid-range, clipping 100, monotonic
  CHECK(level_pct(0) == 0);
  CHECK(level_pct(50) == 0);
  CHECK(level_pct(3000) > 50 && level_pct(3000) < 80);
  CHECK(level_pct(32767) == 100);
  for (int v = 1; v < 32767; v++) CHECK(level_pct(v) <= level_pct(v + 1));
}

int main() {
  test_level_pct();
  test_should_restore();
  test_gain();
  test_extract_mono();
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("voice_logic: all tests passed\n");
  return 0;
}
