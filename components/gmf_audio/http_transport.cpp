#include "http_transport.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace esphome::gmf_audio {

static const char *const TAG = "gmf_http";
static constexpr int READ_POLL_MS = 200;
static constexpr uint32_t IDLE_TIMEOUT_MS = 30000;

void HttpTransport::set_epoch(uint32_t epoch) {
  std::lock_guard<std::mutex> lock(this->title_mutex_);
  this->epoch_.store(epoch);
  this->title_[0] = '\0';
  this->title_dirty_ = false;
}

bool HttpTransport::take_title(char *output, size_t capacity) {
  std::lock_guard<std::mutex> lock(this->title_mutex_);
  if (!this->title_dirty_ || this->cancelled()) return false;
  strlcpy(output, this->title_, capacity);
  this->title_dirty_ = false;
  return true;
}

void HttpTransport::parse_title_() {
  this->metadata_[this->meta_index_] = '\0';
  const char *title = strstr(this->metadata_, "StreamTitle='");
  if (title == nullptr) return;
  title += 13;
  const char *end = strstr(title, "';");
  const size_t length = end ? static_cast<size_t>(end - title) : strlen(title);
  if (length == 0 || length >= sizeof(this->title_)) return;
  std::lock_guard<std::mutex> lock(this->title_mutex_);
  if (this->cancelled()) return;
  if (strncmp(this->title_, title, length) == 0 && this->title_[length] == '\0') return;
  memcpy(this->title_, title, length);
  this->title_[length] = '\0';
  this->title_dirty_ = true;
  ESP_LOGI(TAG, "ICY: %s", this->title_);
}

int HttpTransport::fence_(const char *where) {
  if (!this->cancelled()) return 0;
  const uint32_t epoch = this->active_epoch_.load();
  if (this->cancel_logged_.exchange(epoch) != epoch)
    ESP_LOGW(TAG, "HTTP stream cancelled at %s (epoch %u superseded by %u)", where,
             static_cast<unsigned>(epoch), static_cast<unsigned>(this->epoch_.load()));
  return -1;
}

int HttpTransport::read_(esp_http_client_handle_t client, char *buffer, int length) {
  const uint32_t start = millis();
  for (;;) {
    if (this->fence_("read") < 0) return -1;
    const int count = esp_http_client_read(client, buffer, length);
    if (count != -ESP_ERR_HTTP_EAGAIN || millis() - start >= IDLE_TIMEOUT_MS) return count;
  }
}

int HttpTransport::icy_read_(esp_http_client_handle_t client, char *output, int length) {
  int position = 0;
  int count = 0;
  while (position < length) {
    if (this->meta_remaining_ > 0) {
      char buffer[256];
      count = this->read_(client, buffer, std::min(this->meta_remaining_, int(sizeof(buffer))));
      if (count <= 0) break;
      const int space = sizeof(this->metadata_) - 1 - this->meta_index_;
      const int copied = std::min(count, space);
      if (copied > 0) {
        memcpy(this->metadata_ + this->meta_index_, buffer, copied);
        this->meta_index_ += copied;
      }
      this->meta_remaining_ -= count;
      if (this->meta_remaining_ == 0) {
        this->parse_title_();
        this->meta_index_ = 0;
        this->remaining_ = this->metaint_;
      }
    } else if (this->remaining_ == 0) {
      char prefix;
      count = this->read_(client, &prefix, 1);
      if (count <= 0) break;
      this->meta_remaining_ = static_cast<uint8_t>(prefix) * 16;
      this->meta_index_ = 0;
      if (this->meta_remaining_ == 0) this->remaining_ = this->metaint_;
    } else {
      const int wanted = std::min(length - position, this->remaining_);
      count = this->read_(client, output + position, wanted);
      if (count <= 0) break;
      this->remaining_ -= count;
      position += count;
      if (count < wanted) break;
    }
  }
  return position > 0 ? position : (count < 0 ? -1 : 0);
}

int HttpTransport::event(http_stream_event_msg_t *msg) {
  auto *self = static_cast<HttpTransport *>(msg->user_data);
  auto client = static_cast<esp_http_client_handle_t>(msg->http_client);
  if (client && msg->event_id == HTTP_STREAM_PRE_REQUEST && esp_http_client_get_socket(client) >= 0) {
    self->stale_close++;
    if (esp_http_client_get_transport_type(client) == HTTP_TRANSPORT_OVER_SSL) self->stale_close_tls++;
    ESP_LOGW(TAG, "Closing stale HTTP connection left by a failed open");
    esp_http_client_close(client);
    esp_http_client_clear_response_buffer(client);
  }
  if (self->fence_("hook") < 0) {
    if (client && msg->event_id == HTTP_STREAM_ON_REQUEST) {
      self->cancel_close++;
      if (esp_http_client_get_transport_type(client) == HTTP_TRANSPORT_OVER_SSL) self->cancel_close_tls++;
      esp_http_client_close(client);
    }
    return -1;
  }
  if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    self->metaint_ = self->remaining_ = self->meta_remaining_ = self->meta_index_ = 0;
    self->bitrate_.store(0);
    self->headers_checked_ = self->own_reads_ = false;
  } else if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
    if (!self->headers_checked_) {
      self->headers_checked_ = true;
      char *value = nullptr;
      if (esp_http_client_get_response_header(client, "icy-metaint", &value) == ESP_OK && value) {
        const int interval = std::atoi(value);
        if (interval > 0) self->metaint_ = self->remaining_ = interval;
      }
      value = nullptr;
      if (esp_http_client_get_response_header(client, "icy-br", &value) == ESP_OK && value)
        self->bitrate_.store(std::max(0, std::atoi(value)));
      value = nullptr;
      const bool gzip = esp_http_client_get_response_header(client, "Content-Encoding", &value) == ESP_OK &&
                        value != nullptr && strstr(value, "gzip") != nullptr;
      self->own_reads_ = self->metaint_ > 0 || !gzip;
      if (self->own_reads_) esp_http_client_set_timeout_ms(client, READ_POLL_MS);
    }
    if (self->metaint_ > 0)
      return self->icy_read_(client, static_cast<char *>(msg->buffer), msg->buffer_len);
    if (self->own_reads_) return self->read_(client, static_cast<char *>(msg->buffer), msg->buffer_len);
  }
  return 0;
}

}  // namespace esphome::gmf_audio
