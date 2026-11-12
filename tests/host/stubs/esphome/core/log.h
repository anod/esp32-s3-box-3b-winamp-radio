#pragma once
template<typename... Args> inline void host_log(Args &&...) {}
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) host_log(__VA_ARGS__)
