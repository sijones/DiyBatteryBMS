#include "Diagnostics.h"
#include "WebLog.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_idf_version.h>

DiagnosticsClass Diag;

/* Once free heap is under this, the board is in the territory where an
   allocation failure aborts it, so every new low is worth a line. Above it the
   heap moves around constantly and reporting would be noise. */
#define DIAG_HEAP_WARN_FLOOR  40000
// ...but only when the mark has moved this far since the last line, so a slow
// leak leaves a readable trail rather than a line a second.
#define DIAG_HEAP_WARN_STEP    4000

// How long after boot to sample the heap, and how often - see Loop()
#define DIAG_BOOT_CURVE_SECS    600
#define DIAG_BOOT_CURVE_MS    30000

/* Carried across the reset, so the run that died can be described by the run
   that follows it. Deliberately small and fixed - RTC slow memory is scarce and
   shared with anything else that wants to survive a reboot. */
#define DIAG_RTC_MAGIC 0x424D5344u   // 'BMSD'

struct DiagRtcState {
  uint32_t magic;
  uint32_t boots;        // consecutive restarts since the last power-on
  uint32_t uptimeSecs;   // how long the run lasted, as of its last tick
  uint32_t heapMin;      // and its lowest free heap
  uint32_t blockMin;     // and its smallest largest-free-block
};

static RTC_NOINIT_ATTR DiagRtcState _rtc;

static const char* reasonName(esp_reset_reason_t r, bool& isCrash)
{
  // Static, not a local: the caller holds this pointer for the life of the
  // device to answer ResetReason(), so it has to outlive this frame.
  static char unknown[24];

  isCrash = false;
  switch (r) {
    case ESP_RST_POWERON:   return "Power on";
    case ESP_RST_EXT:       return "External reset pin";
    case ESP_RST_SW:        return "Software restart";       // our own ESP.restart()
    case ESP_RST_DEEPSLEEP: return "Woke from deep sleep";
    case ESP_RST_SDIO:      return "SDIO reset";
    case ESP_RST_PANIC:     isCrash = true; return "Panic / exception";
    case ESP_RST_INT_WDT:   isCrash = true; return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:  isCrash = true; return "Task watchdog";
    case ESP_RST_WDT:       isCrash = true; return "Other watchdog";
    case ESP_RST_BROWNOUT:  isCrash = true; return "Brownout (supply dipped)";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    case ESP_RST_USB:       return "USB peripheral reset";
    case ESP_RST_JTAG:      return "JTAG reset";
    case ESP_RST_EFUSE:     isCrash = true; return "eFuse error";
    case ESP_RST_PWR_GLITCH:isCrash = true; return "Power glitch";
    case ESP_RST_CPU_LOCKUP:isCrash = true; return "CPU lockup";
#endif
    default: break;
  }
  // Named rather than swallowed, so a reason this build predates still says
  // something useful enough to look up.
  snprintf(unknown, sizeof(unknown), "Unknown (%d)", (int)r);
  return unknown;
}

// "3d 4h", "2h 11m", "47s" - readable at a glance, which a raw second count of
// a run that lasted two days is not.
static void fmtDuration(uint32_t secs, char* out, size_t n)
{
  if (secs >= 86400)   snprintf(out, n, "%ud %uh", (unsigned)(secs / 86400), (unsigned)((secs % 86400) / 3600));
  else if (secs >= 3600) snprintf(out, n, "%uh %um", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
  else if (secs >= 60)   snprintf(out, n, "%um %us", (unsigned)(secs / 60), (unsigned)(secs % 60));
  else                   snprintf(out, n, "%us", (unsigned)secs);
}

uint32_t DiagnosticsClass::UptimeSecs() const
{
  // esp_timer, not millis(): 64-bit microseconds since boot, so no 49-day wrap
  // to reason about on a device that is meant to run for months.
  return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

uint32_t DiagnosticsClass::HeapMin() const
{
  // The IDF already tracks this one for us, across every heap it manages.
  return (uint32_t)esp_get_minimum_free_heap_size();
}

/* Internal RAM only - the pool that actually runs out.

   On a board with PSRAM the totals above are worse than useless for judging
   safety: this S3 boots with 8,537,572 bytes free and 156,112 of it internal,
   so a floor of 40,000 against the total can never be crossed no matter how
   completely the internal pool is exhausted. WiFi and lwIP buffers must be
   internal because they have to be DMA-capable, and those are exactly the
   allocations that fail. Watch this one instead.

   On a board without PSRAM it is the same number as the total, so nothing is
   lost by using it everywhere. */
uint32_t DiagnosticsClass::InternalFree() const
{
  return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t DiagnosticsClass::InternalMin() const
{
  return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

void DiagnosticsClass::Begin()
{
  const esp_reset_reason_t reason = esp_reset_reason();
  _reasonName = reasonName(reason, _wasCrash);

  /* A power-on leaves whatever was in RTC memory before, so the magic word
     alone is not enough - a warm reboot after a cold start would otherwise
     report the pre-power-cut run as if it were the one that just ended. */
  _haveHistory = (_rtc.magic == DIAG_RTC_MAGIC && reason != ESP_RST_POWERON);
  if (_haveHistory) {
    _bootCount    = _rtc.boots + 1;
    _prevUptime   = _rtc.uptimeSecs;
    _prevHeapMin  = _rtc.heapMin;
    _prevBlockMin = _rtc.blockMin;
  }

  const uint32_t freeNow  = (uint32_t)esp_get_free_heap_size();
  const uint32_t blockNow = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  _blockMin = blockNow;

  _rtc.magic      = DIAG_RTC_MAGIC;
  _rtc.boots      = _bootCount;
  _rtc.uptimeSecs = 0;
  // Internal, to match what Loop() writes here a second from now and what the
  // warnings are judged on. Only matters for a crash inside that first second.
  _rtc.heapMin    = InternalFree();
  _rtc.blockMin   = blockNow;
  _lastTickMs     = millis();
  _lastMilestoneFree = freeNow;   // first milestone is measured from boot

  /* Straight to the serial line as well as through the log macros, for the same
     reason SerialSetup.h does it: this build runs at CORE_DEBUG_LEVEL=1, where
     nothing below an error reaches the cable. A boot line that only shows up in
     the web log is no use for diagnosing a board that keeps rebooting, because
     the browser loses its WebSocket every time it does. */
  Serial.printf("[boot] boot #%u, reset reason: %s%s\r\n",
                (unsigned)_bootCount, _reasonName, _wasCrash ? "  <-- CRASH" : "");
  /* Internal alongside the total, because on a PSRAM board the total is the
     wrong number to read: 8.5MB free means nothing when WiFi and lwIP can only
     allocate from the 156KB of internal RAM behind it. On a board without
     PSRAM the two are equal and the line simply says so twice. */
  Serial.printf("[boot] heap %u B free (%u B internal), largest block %u B\r\n",
                (unsigned)freeNow, (unsigned)InternalFree(), (unsigned)blockNow);

  /* Say plainly whether PSRAM was found, because the failure is silent.
     BOARD_HAS_PSRAM with no PSRAM present - or with the wrong memory_type for
     the part, quad settings on an octal module - logs a failure deep in the
     core and carries on with internal RAM only. The board then behaves exactly
     like one that was never meant to have any, and the build looks correct. */
  const size_t psramSize = ESP.getPsramSize();
  if (psramSize) {
    Serial.printf("[boot] PSRAM %u B total, %u B free\r\n",
                  (unsigned)psramSize, (unsigned)ESP.getFreePsram());
    WS_LOG_I("PSRAM %u KB total, %u KB free", (unsigned)(psramSize / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
  } else {
#ifdef BOARD_HAS_PSRAM
    // Built expecting it and did not get it - worth an error, not a shrug
    Serial.println("[boot] PSRAM: NONE FOUND, but this build expects it - check memory_type");
    WS_LOG_E("PSRAM expected by this build but not found - check board_build.arduino.memory_type");
#else
    Serial.println("[boot] PSRAM: none (not enabled in this build)");
#endif
  }

  if (_wasCrash)
    WS_LOG_E("Boot #%u after a CRASH - reset reason: %s", (unsigned)_bootCount, _reasonName);
  else
    WS_LOG_I("Boot #%u, reset reason: %s", (unsigned)_bootCount, _reasonName);

  if (_haveHistory) {
    char dur[24];
    fmtDuration(_prevUptime, dur, sizeof(dur));
    Serial.printf("[boot] previous run lasted %s, internal low water %u B, smallest block %u B\r\n",
                  dur, (unsigned)_prevHeapMin, (unsigned)_prevBlockMin);
    /* The line that answers "was it the heap?". A run that ended with tens of
       kilobytes spare did not die of exhaustion and the cause is elsewhere; one
       that ended in the low thousands almost certainly did. */
    WS_LOG_W("Previous run lasted %s, heap low water %u B, smallest block %u B",
             dur, (unsigned)_prevHeapMin, (unsigned)_prevBlockMin);
  }

  WS_LOG_I("Heap at boot: %u B free of %u B, largest block %u B",
           (unsigned)freeNow, (unsigned)ESP.getHeapSize(), (unsigned)blockNow);
}

void DiagnosticsClass::Loop()
{
  const uint32_t now = millis();

#if defined(BMS_S3)
  // Its own interval, independent of the once-a-second gate below - 5s is
  // enough resolution to be useful as "headroom right now" without the
  // sampling itself (one uxTaskGetSystemState() call) running any more often
  // than it needs to.
  if ((uint32_t)(now - _lastCpuTickMs) >= 5000) {
    _lastCpuTickMs = now;
    SampleCpuUsage();
  }
#endif

  if ((uint32_t)(now - _lastTickMs) < 1000) return;
  _lastTickMs = now;

  const uint32_t freeNow  = (uint32_t)esp_get_free_heap_size();
  const uint32_t blockNow = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // Warnings are judged on internal RAM, not the total - see InternalFree()
  const uint32_t heapMin  = InternalMin();
  if (blockNow < _blockMin) _blockMin = blockNow;

  /* Written out every tick rather than saved on the way down, because there is
     no way down: a panic or a watchdog reset gives no notice and runs no
     handler of ours. Whatever is in here when the board dies is exactly what
     the next boot has to work with. */
  _rtc.uptimeSecs = UptimeSecs();
  _rtc.heapMin    = heapMin;
  _rtc.blockMin   = _blockMin;

  /* The boot window, sampled.

     Something releases roughly 21KB in the minutes after boot: at 15:20:57 this
     board had 29.7KB free with nothing connected, and at 15:27:43 it had 51.2KB
     having been asked for nothing in between. That window is where the original
     crash loop lived - a browser arriving eleven seconds after boot met a 1KB
     low water, while the same browser seven minutes later cost 1,132 bytes and
     nothing noticed.

     Sampling it draws the shape, and the shape names the suspect: a sharp step
     at a fixed moment reads like a buffer pool being handed back, a slow decay
     like a queue draining. The DMA figure is here to separate them - the WiFi
     driver's RX and TX buffers must be DMA-capable, so if the recovery shows up
     there it is the radio settling rather than the MQTT outbox emptying.

     Serial only, deliberately: twenty lines in the web log would push out
     everything else in a 60-entry ring buffer. */
  if (UptimeSecs() < DIAG_BOOT_CURVE_SECS &&
      (_lastCurveMs == 0 || (uint32_t)(now - _lastCurveMs) >= DIAG_BOOT_CURVE_MS)) {
    _lastCurveMs = now;
    Serial.printf("[heap] t+%-4u free %6u  largest %6u  min %6u  dma %6u  internal %6u\r\n",
                  (unsigned)UptimeSecs(), (unsigned)freeNow, (unsigned)blockNow,
                  (unsigned)heapMin,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }

  /* First table half a minute in - late enough that every task exists, early
     enough to be near the top of the log. After that, every five minutes, but
     printed only when something has actually got tighter.

     The re-check is the point. A stack trimmed to fit an idle measurement is a
     stack overflow waiting for the one path nobody exercised: async_tcp runs
     the web server's callbacks, so its worst case arrives with web traffic,
     not at boot. Watching the total fall as the UI is used is what makes it
     safe to cut anything. */
  if (!_tasksReported && UptimeSecs() >= 30) {
    _tasksReported = true;
    ReportTasks();
  } else if (_tasksReported && (uint32_t)(now - _lastTaskCheckMs) >= 300000) {
    _lastTaskCheckMs = now;
    const uint32_t spare = TotalStackSpare();
    // Only when a meaningful bite has been taken out of the headroom since the
    // last table - otherwise this is 17 lines of unchanged numbers every five
    // minutes, and the one time it changes would be lost in them.
    if (spare && _lastSpareTotal && (spare + 512) <= _lastSpareTotal) {
      Serial.printf("[task] stack headroom fell %u B since the last table\r\n",
                    (unsigned)(_lastSpareTotal - spare));
      ReportTasks();
    }
  }

  /* A descending trail, not a line a second: report only a new low, only under
     the floor worth caring about, and only once it has moved far enough to be
     news. WS_LOG_W reaches syslog, so on a board that is about to die the trail
     survives the reboot - which the web log's RAM buffer does not. */
  if (heapMin < DIAG_HEAP_WARN_FLOOR &&
      (_lastWarnedHeap == 0 || heapMin + DIAG_HEAP_WARN_STEP <= _lastWarnedHeap)) {
    _lastWarnedHeap = heapMin;
    /* Serial as well as the log macro. WS_LOG_W runs log_w, which this build
       discards at CORE_DEBUG_LEVEL=1, so the trail existed everywhere except
       the one channel still attached when the board dies. */
    Serial.printf("[heap] internal low water %u B (now %u internal, %u total free)\r\n",
                  (unsigned)heapMin, (unsigned)InternalFree(), (unsigned)freeNow);
    WS_LOG_W("Internal RAM low water down to %u B (now %u internal free)",
             (unsigned)heapMin, (unsigned)InternalFree());
  }
}

/* Where the heap actually went.

   The trail above only starts once free heap is under the floor, which on this
   board is already past WiFi association, the MQTT connect and the discovery
   burst - so it says the heap is nearly gone without saying who took it. These
   are called at the points either side of the big consumers, and the
   difference between two of them is that stage's bill. */
void DiagnosticsClass::Milestone(const char* what)
{
  const uint32_t freeNow  = (uint32_t)esp_get_free_heap_size();
  const uint32_t blockNow = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const int32_t  delta    = (int32_t)freeNow - (int32_t)_lastMilestoneFree;

  Serial.printf("[heap] %-22s %7u B free, largest block %6u B  (%+d)\r\n",
                what, (unsigned)freeNow, (unsigned)blockNow, (int)delta);
  WS_LOG_I("Heap after %s: %u B free, largest block %u B (%+d)",
           what, (unsigned)freeNow, (unsigned)blockNow, (int)delta);

  _lastMilestoneFree = freeNow;
}

/* Sum of every task's untouched stack. Same snapshot ReportTasks() prints, but
   without the 17 lines - used to decide whether printing them is worth it. */
uint32_t DiagnosticsClass::TotalStackSpare()
{
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
  if (!st) return 0;                       // no heap to ask with; not an answer
  const UBaseType_t got = uxTaskGetSystemState(st, n, nullptr);
  uint32_t total = 0;
  for (UBaseType_t i = 0; i < got; i++) total += (uint32_t)st[i].usStackHighWaterMark;
  free(st);
  return total;
}

void DiagnosticsClass::ReportTasks()
{
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  /* One allocation, freed immediately. Asking for the table is the only way to
     get every task including the ones the libraries create - AsyncTCP's and
     the WiFi driver's - which are exactly the ones no call site of ours can
     name. */
  TaskStatus_t* st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
  if (!st) { Serial.println("[task] not enough heap to list tasks"); return; }

  const UBaseType_t got = uxTaskGetSystemState(st, n, nullptr);
  Serial.printf("[task] %u tasks - 'spare' is stack never touched, i.e. reclaimable\r\n",
                (unsigned)got);
  uint32_t spareTotal = 0;
  for (UBaseType_t i = 0; i < got; i++) {
    // ESP-IDF reports the high-water mark in bytes, not words
    const uint32_t spare = (uint32_t)st[i].usStackHighWaterMark;
    spareTotal += spare;
    Serial.printf("[task]   %-18s core %-2d prio %-2u  spare %5u B\r\n",
                  st[i].pcTaskName,
                  (int)((st[i].xCoreID == tskNO_AFFINITY) ? -1 : (int)st[i].xCoreID),
                  (unsigned)st[i].uxCurrentPriority, (unsigned)spare);
    /* Flush between lines. Fifteen printf calls back to back overrun the
       ESP32-S3's USB CDC buffer and the table comes out shredded - lines
       merged, fields half-written - which is worse than useless for a
       diagnostic whose whole job is to be read. Costs a few milliseconds,
       once. */
    Serial.flush();
  }
  Serial.printf("[task] %u B of stack has never been used\r\n", (unsigned)spareTotal);
  free(st);
  _lastSpareTotal   = spareTotal;
  _lastTaskCheckMs  = millis();
}

#if defined(BMS_S3)
/* Each core's own idle task (FreeRTOS names them IDLE0/IDLE1 and pins each to
   its core) only runs when that core has nothing else ready to run - so its
   share of a wall-clock window IS that core's own idle fraction, no per-core
   division needed. The window is measured independently via
   esp_timer_get_time() rather than trusting uxTaskGetSystemState()'s own
   pulTotalRunTime total, which on a dual-core SMP build is not documented
   clearly enough to rely on - a task's ulRunTimeCounter and esp_timer both
   count the same shared microsecond clock regardless, so the ratio holds
   either way. */
void DiagnosticsClass::SampleCpuUsage()
{
  const UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
  if (!st) return;
  const UBaseType_t got = uxTaskGetSystemState(st, n, nullptr);

  uint32_t idle0 = 0, idle1 = 0;
  bool found0 = false, found1 = false;
  for (UBaseType_t i = 0; i < got; i++) {
    if (!strcmp(st[i].pcTaskName, "IDLE0"))      { idle0 = st[i].ulRunTimeCounter; found0 = true; }
    else if (!strcmp(st[i].pcTaskName, "IDLE1")) { idle1 = st[i].ulRunTimeCounter; found1 = true; }
  }
  free(st);

  const int64_t nowUs = esp_timer_get_time();
  if (_haveIdlePrev) {
    const int64_t windowUs = nowUs - _lastCpuSampleUs;
    if (windowUs > 0) {
      if (found0) _idleCore0Percent = 100.0f * (float)(idle0 - _prevIdle0Runtime) / (float)windowUs;
      if (found1) _idleCore1Percent = 100.0f * (float)(idle1 - _prevIdle1Runtime) / (float)windowUs;
    }
  }
  _prevIdle0Runtime = idle0;
  _prevIdle1Runtime = idle1;
  _lastCpuSampleUs  = nowUs;
  _haveIdlePrev     = true;
}
#endif
