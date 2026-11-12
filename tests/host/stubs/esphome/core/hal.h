#pragma once
#include <cstdint>
extern uint32_t host_millis;
inline uint32_t millis() { return host_millis; }
