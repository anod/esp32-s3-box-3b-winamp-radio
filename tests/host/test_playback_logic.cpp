#include <cassert>
#include <cstring>
#include "../../components/internet_radio/media_player/playback_logic.h"

using namespace esphome::internet_radio::playback_logic;

static void test_pipeline_stop_tracker() {
  PipelineStopTracker pipeline;
  assert(!pipeline.needs_stop());  // Nothing created yet.
  pipeline.run_attempted(false);   // Setup failed before a pipeline existed.
  assert(!pipeline.needs_stop());
  pipeline.run_attempted(true);
  assert(pipeline.needs_stop());
  pipeline.stop_result(false);  // A failed stop is retried.
  assert(pipeline.needs_stop());
  pipeline.stop_result(true);
  // Superseded RUN: the next STOP/RUN must not issue a second waited stop.
  assert(!pipeline.needs_stop());
  pipeline.run_attempted(true);  // Failed or successful launch re-arms the stop.
  assert(pipeline.needs_stop());
  pipeline.stop_result(true);
  pipeline.run_attempted(false);  // Existing pipeline is still re-armed after setup failure.
  assert(pipeline.needs_stop());
}

int main() {
  assert(std::strcmp(phase(false, false, false, 0, true, true, false), "connecting") == 0);
  assert(std::strcmp(phase(false, false, false, 0, false, true, false), "stopping") == 0);
  assert(std::strcmp(phase(false, true, true, 1, true, false, false), "announcing") == 0);
  assert(std::strcmp(phase(true, true, true, 1, true, false, false), "control_timeout") == 0);
  assert(std::strcmp(phase(false, false, false, 1, true, false, false), "playing") == 0);
  assert(std::strcmp(phase(false, false, false, 2, true, false, false), "paused") == 0);
  assert(!startup_expired(true, true, false, true, 25000, 0));
  assert(!startup_expired(true, true, true, false, 25000, 0));
  assert(!startup_expired(false, true, false, false, 25000, 0));
  assert(!startup_expired(true, true, false, false, 19999, 0));
  assert(startup_expired(true, true, false, false, 20000, 0));
  assert(startup_expired(true, true, false, false, 10000, UINT32_MAX - 10000));
  assert(!control_expired(false, false, 10000, 0));
  assert(!control_expired(true, true, 10000, 0));
  assert(!control_expired(true, false, 9999, 0));
  assert(control_expired(true, false, 10000, 0));
  assert(control_expired(true, false, 5000, UINT32_MAX - 5000));
  assert(!control_expired(true, false, 10000, 10001));
  assert(!startup_expired(true, true, false, false, 10000, 10001));
  // Not launched yet (worker still stopping the previous stream): no startup clock.
  assert(!startup_expired(true, false, false, false, 25000, 0));

  // Paused radio (also a pending PAUSE: state=paused, requested cleared) stays paused.
  assert(!resume_after_voice(2, false, false));
  assert(resume_after_voice(1, false, false));
  assert(resume_after_voice(0, true, false));
  assert(resume_after_voice(0, false, true));
  assert(resume_after_voice(2, true, false));  // RESUME requested before the session
  assert(!resume_after_voice(0, false, false));

  assert(!http_cancelled(7, 7));
  assert(http_cancelled(7, 8));
  assert(http_cancelled(UINT32_MAX, 0));

  assert(needs_cleanup_stop(FailedOp::PAUSE));
  assert(needs_cleanup_stop(FailedOp::RESUME));
  assert(!needs_cleanup_stop(FailedOp::RUN));
  assert(!needs_cleanup_stop(FailedOp::STOP));
  assert(failure_retries(FailedOp::RUN, false));
  assert(failure_retries(FailedOp::RESUME, false));
  assert(!failure_retries(FailedOp::PAUSE, false));
  assert(!failure_retries(FailedOp::STOP, false));
  assert(!failure_retries(FailedOp::RUN, true));
  test_pipeline_stop_tracker();
}
