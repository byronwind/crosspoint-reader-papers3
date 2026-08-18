"""
PlatformIO pre-build script: patch the crosspoint-simulator checkout for the
AnkiEInk simulator env (simulator_m5papers3-anki).

The stock simulator shim is enough for the reader itself, but AnkiEInk needs
two additions:

1. FreeRTOS tick API — DeckStore/ApkgImporter install sqlite3_progress_handler
   callbacks throttled with xTaskGetTickCount()/pdMS_TO_TICKS(). The simulator
   FreeRTOS.h shim provides task primitives but not the tick clock, so this
   patch adds a host-backed millisecond tick (steady_clock since start).

2. HalStorage::hostPath() — the simulator links the macOS system sqlite3,
   whose default unix VFS operates on real filesystem paths. The Anki code
   opens its database through SD-logical paths ("/AnkiEInk/anki.db"), so the
   simulator needs the same logical->host mapping its HalStorage already does
   internally (CROSSPOINT_SIM_SD root) exposed publicly.

Idempotent: each edit is applied only while the original marker is present;
an already-patched tree is left untouched.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os

SIM_ROOT = "/Users/liguoqing/Downloads/PaperS3/crosspoint-simulator"

FREERTOS_H = os.path.join(SIM_ROOT, "src", "freertos", "FreeRTOS.h")
HAL_H = os.path.join(SIM_ROOT, "src", "HalStorage.h")
HAL_CPP = os.path.join(SIM_ROOT, "src", "HalStorage.cpp")

OLD_FR_HEADER = """#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define eIncrement 1
#define portTICK_PERIOD_MS 1"""

NEW_FR_HEADER = """#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define eIncrement 1
#define portTICK_PERIOD_MS 1

// [anki-sim patch] 1 ms ticks like the ESP32 default (configTICK_RATE_HZ =
// 1000), so pdMS_TO_TICKS(x) is an identity conversion.
using TickType_t = uint32_t;
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#define pdPASS 1

// Milliseconds since process start; wraparound matches FreeRTOS semantics
// for the subtraction-based elapsed-time comparisons callers use.
inline TickType_t xTaskGetTickCount() {
  using namespace std::chrono;
  static const steady_clock::time_point start = steady_clock::now();
  return static_cast<TickType_t>(
      duration_cast<milliseconds>(steady_clock::now() - start).count());
}"""

OLD_HAL_DECL = "  static HalStorage &getInstance() { return instance; }"
NEW_HAL_DECL = OLD_HAL_DECL + """

  // [anki-sim patch] Map an SD-logical path to the host filesystem path under
  // the simulated SD root, for code that bypasses HalStorage file APIs —
  // sqlite3's unix VFS needs a real host path to open the Anki database.
  static std::string hostPath(const char *path);"""

# Anchor must sit OUTSIDE the anonymous namespace (which wraps
# resolveStoragePath), or the member definition won't compile.
OLD_HAL_DEF = "bool HalStorage::begin() {"
NEW_HAL_DEF = """// [anki-sim patch] Public wrapper over resolveStoragePath (see HalStorage.h).
std::string HalStorage::hostPath(const char *path) {
  return resolveStoragePath(path);
}

bool HalStorage::begin() {"""


def patch_file(path, old, new, marker=None):
    if not os.path.isfile(path):
        print(f"  [patch_simulator_anki] SKIP {path} (not found)")
        return
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # marker: a substring of `new` whose presence means an earlier run already
    # applied this edit but left `old` matchable (re-apply would duplicate).
    if marker and marker in text:
        return
    if old not in text:
        return  # already patched or layout changed; leave untouched
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new, 1))
    print(f"  [patch_simulator_anki] patched {path}")


patch_file(FREERTOS_H, OLD_FR_HEADER, NEW_FR_HEADER, marker="xTaskGetTickCount")
# Backfill for trees patched by an earlier version of this script, which added
# the tick API without pdPASS (used by DeckImportActivity's xTaskCreate check).
patch_file(FREERTOS_H, "#define configTICK_RATE_HZ 1000",
           "#define configTICK_RATE_HZ 1000\n#define pdPASS 1", marker="#define pdPASS 1")
# NOTE: marker guards are required everywhere — each NEW_* text still contains
# its OLD_* anchor, so an unguarded re-run would duplicate the insertion.
patch_file(HAL_H, OLD_HAL_DECL, NEW_HAL_DECL, marker="hostPath")
patch_file(HAL_CPP, OLD_HAL_DEF, NEW_HAL_DEF, marker="HalStorage::hostPath")
