#include <cstddef>
#include "esp_heap_caps.h"

namespace esphome {
namespace gmf_audio {
// Pull this translation unit out of the component archive before weak codec hooks resolve.
void initialize_codec_memory() {}
}
}

// The codec archives expose these weak hooks. Their default libc allocation
// prefers internal RAM, which cannot fit the measured 35,460-byte AAC workspace
// after DMA/wake initialization. Codec workspaces are CPU-only, not DMA buffers.
extern "C" {

void *media_lib_module_malloc(const char *, size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *media_lib_module_calloc(const char *, size_t count, size_t size) {
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *media_lib_module_realloc(const char *, void *pointer, size_t size) {
  return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}
