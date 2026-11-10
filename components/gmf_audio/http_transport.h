#pragma once

#include "esp_gmf_io_http.h"
#include "esp_http_client.h"
#include <atomic>
#include <mutex>

namespace esphome::gmf_audio {

class HttpTransport {
 public:
  void set_epoch(uint32_t epoch);
  void launch(uint32_t epoch) {
    this->bitrate_.store(0);
    this->active_epoch_.store(epoch);
  }
  bool cancelled() const { return this->epoch_.load() != this->active_epoch_.load(); }
  bool take_title(char *output, size_t capacity);
  int bitrate() const { return this->cancelled() ? 0 : this->bitrate_.load(); }
  static int event(http_stream_event_msg_t *msg);
  std::atomic<uint32_t> cancel_close{0};
  std::atomic<uint32_t> cancel_close_tls{0};
  std::atomic<uint32_t> stale_close{0};
  std::atomic<uint32_t> stale_close_tls{0};

 protected:
  int fence_(const char *where);
  int read_(esp_http_client_handle_t client, char *buffer, int length);
  int icy_read_(esp_http_client_handle_t client, char *output, int length);
  void parse_title_();
  std::atomic<uint32_t> epoch_{0};
  std::atomic<uint32_t> active_epoch_{0};
  std::atomic<uint32_t> cancel_logged_{UINT32_MAX};
  std::atomic<int> bitrate_{0};
  std::mutex title_mutex_;
  char title_[128]{};
  bool title_dirty_{false};
  int metaint_{0};
  int remaining_{0};
  int meta_remaining_{0};
  int meta_index_{0};
  bool headers_checked_{false};
  bool own_reads_{false};
  char metadata_[512]{};
};

}  // namespace esphome::gmf_audio
