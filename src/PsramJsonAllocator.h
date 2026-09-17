#pragma once

/* ArduinoJson allocations, sent to PSRAM where the board has it.

   A JsonDocument grows in small steps - pool pages and one copy per string -
   and every one of them is under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096 on
   this platform), so a plain malloc() puts all of it in internal RAM however
   much PSRAM is free. The status payload is rebuilt several times a second, so
   those documents were a constant draw on the one pool WiFi and lwIP cannot do
   without, for data that has no reason to be there.

   Asked for by caps rather than by size, so it lands in PSRAM regardless of the
   threshold. Falls back to the ordinary heap when PSRAM cannot answer: a build
   without it, a -psram image on a module that turned out to have none, or
   PSRAM simply full. On a build without BOARD_HAS_PSRAM it is plain malloc and
   costs nothing.

   Documents only. The send buffer a document serialises into is a
   std::vector the web socket library owns, so it keeps its default allocator. */

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
  void* allocate(size_t size) override {
#ifdef BOARD_HAS_PSRAM
    if (void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
#endif
    return malloc(size);
  }

  // free() finds the right heap from the pointer, whichever pool it came from
  void deallocate(void* ptr) override { free(ptr); }

  void* reallocate(void* ptr, size_t newSize) override {
#ifdef BOARD_HAS_PSRAM
    // Leaves ptr untouched on failure, so the fallback below still has it
    if (void* p = heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
#endif
    return realloc(ptr, newSize);
  }

  static ArduinoJson::Allocator* instance() {
    static PsramJsonAllocator allocator;
    return &allocator;
  }
};
