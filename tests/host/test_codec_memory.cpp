#include <cassert>
#include <cstdlib>
#include <cstring>
#include "esp_heap_caps.h"
#include "../../components/gmf_audio/codec_memory.cpp"

static void check_caps(uint32_t caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
void *heap_caps_malloc(size_t size, uint32_t caps) {
  check_caps(caps);
  return std::malloc(size);
}
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
  check_caps(caps);
  return std::calloc(count, size);
}
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
  check_caps(caps);
  return std::realloc(ptr, size);
}

int main() {
  auto *buffer = static_cast<unsigned char *>(media_lib_module_malloc("aac", 35460));
  assert(buffer);
  std::memset(buffer, 42, 35460);
  buffer = static_cast<unsigned char *>(media_lib_module_realloc("aac", buffer, 40000));
  assert(buffer && buffer[35459] == 42);
  std::free(buffer);
  buffer = static_cast<unsigned char *>(media_lib_module_calloc("mp3", 32, 4));
  assert(buffer);
  for (size_t i = 0; i < 128; i++) assert(buffer[i] == 0);
  std::free(buffer);
}
