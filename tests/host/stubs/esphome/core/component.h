#pragma once
namespace esphome {
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  bool is_failed() const { return failed_; }
  void mark_failed() { failed_ = true; }
  void status_set_warning(const char *) {}
 private:
  bool failed_{false};
};
}  // namespace esphome
#define LOG_STR(x) x
