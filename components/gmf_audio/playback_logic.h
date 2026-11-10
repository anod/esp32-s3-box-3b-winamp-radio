#pragma once
#include <cstdint>

namespace esphome {
namespace gmf_audio {
namespace playback_logic {

inline const char *phase(bool fault, bool announcing, bool voice, int state,
                         bool requested, bool pending, bool failed) {
  if (fault) return "control_timeout";
  if (announcing) return "announcing";
  if (voice) return "voice";
  if (state == 1) return "playing";
  if (state == 2) return "paused";
  if (failed) return "retrying";
  if (requested) return "connecting";
  if (pending) return "stopping";
  return "idle";
}

// `launched` is set by the control worker immediately before the pipeline run,
// so time spent stopping the previous stream never counts against startup.
inline bool startup_expired(bool requested, bool launched, bool pcm_started, bool pending,
                            uint32_t now, uint32_t start) {
  const uint32_t elapsed = now - start;
  return requested && launched && !pcm_started && !pending && elapsed >= 20000 &&
         elapsed < 0x80000000u;
}

inline bool control_expired(bool pending, bool fault, uint32_t now, uint32_t start) {
  const uint32_t elapsed = now - start;
  return pending && !fault && elapsed >= 10000 && elapsed < 0x80000000u;
}

// Radio intent captured when a voice session or standalone announcement takes
// over the pipeline. A paused radio (including a not-yet-applied PAUSE, which
// marks the state paused and clears `requested` at request time) stays paused.
inline bool resume_after_voice(int state, bool requested, bool failed) {
  return state == 1 || requested || failed;
}

// A stream is cancelled once a newer STOP/RUN request superseded the epoch the
// running pipeline was launched with.
inline bool http_cancelled(uint32_t active_epoch, uint32_t current_epoch) {
  return active_epoch != current_epoch;
}

enum class FailedOp { STOP, RUN, PAUSE, RESUME };

// A failed PAUSE/RESUME leaves GMF in an unknown state: the worker must stop it.
inline bool needs_cleanup_stop(FailedOp op) {
  return op == FailedOp::PAUSE || op == FailedOp::RESUME;
}

// Only failures of operations that asked for radio audio schedule a retry.
inline bool failure_retries(FailedOp op, bool announcement) {
  return !announcement && (op == FailedOp::RUN || op == FailedOp::RESUME);
}

// Tracks whether the worker must ask GMF to stop the pipeline. GMF's task thread
// clears its RUNNING action only after STOP_BIT wakes the (higher-priority) waiter, so
// a second waited stop issued straight after the first sees "running", waits for a
// STOP_BIT that was already consumed and then blocks forever. A pipeline is stopped
// only once after each launch attempt; a failed stop keeps it armed for a retry.
class PipelineStopTracker {
 public:
  bool needs_stop() const { return this->created_ && !this->stopped_; }
  void stop_result(bool ok) {
    if (ok) this->stopped_ = true;
  }
  // Any run attempt may have started the GMF task (e.g. a run timeout), so re-arm it.
  void run_attempted(bool pipeline_exists) {
    if (pipeline_exists) this->created_ = true;
    this->stopped_ = !this->created_;
  }

 private:
  bool created_{false};
  bool stopped_{true};
};

}  // namespace playback_logic
}  // namespace gmf_audio
}  // namespace esphome
