#pragma once

/* Why the board last restarted, and how close it came to running out of heap
   before it did.

   A crash-rebooting board does not look like a crashing board from the outside.
   It looks like a network fault: the MQTT broker logs a fresh connection every
   twenty to sixty seconds taking over the previous session, the browser's
   WebSocket keeps dropping, and nothing on any channel says the device
   restarted. The reset reason was sitting in the RTC registers the whole time -
   it was simply never read.

   The heap figures are the other half of it. esp_reset_reason() says "panic" or
   "task watchdog" but never why, and the usual why on this firmware is heap
   exhaustion: with exceptions disabled a failed allocation is not a thrown
   bad_alloc but a call to abort() (see the note above the BLE block in
   HTTPWSFunctions.h). A board that died that way had a heap low-water mark in
   the low tens of kilobytes on the way down, and that number is the diagnosis.

   So the low-water marks are carried through the reset in RTC memory, and each
   boot reports what the run before it had left when it went. RTC_NOINIT_ATTR
   rather than RTC_DATA_ATTR: the bootloader reloads .rtc.data from the image on
   every reset, which would wipe the figures on exactly the reboot they exist to
   explain. .rtc_noinit survives everything except a power cycle, so a magic
   word says whether what is in there is ours or the garbage a cold start left. */

#include <Arduino.h>
#include <esp_system.h>

class DiagnosticsClass
{
public:
  // Reads the reset reason and the previous run's figures out of RTC memory,
  // then reports both. Call once, as early in setup() as the serial port allows.
  void Begin();
  // Refreshes the low-water marks and warns as the heap descends. Cheap: does
  // its work once a second and returns immediately in between.
  void Loop();
  /* Reports heap here and what it cost since the last call, so the big
     consumers during startup can be told apart. `what` names the stage that
     has just finished, e.g. "WiFi associated". */
  void Milestone(const char* what);

  /* Every task's name and how much of its stack has never been touched.
     Stacks come out of the same heap everything else competes for, and a task
     given 4KB that has only ever used 900 bytes is holding 3KB hostage for the
     life of the device. Printed once, thirty seconds in, by which time every
     task has been through its worst path. */
  void ReportTasks();
  // The same total without the 17 lines, for deciding whether to print them
  uint32_t TotalStackSpare();

  const char* ResetReason() const { return _reasonName; }
  // Panic, watchdog or brownout - as opposed to a power-on or a reboot we asked
  // for. The distinction is the whole point of the boot line.
  bool     Crashed() const        { return _wasCrash; }
  uint32_t BootCount() const      { return _bootCount; }
  uint32_t UptimeSecs() const;

  uint32_t HeapMin() const;                  // lowest free heap this run
  /* Internal RAM, which on a PSRAM board is the only pool that can run out -
     WiFi and lwIP need DMA-capable memory and cannot use PSRAM. The warning
     thresholds are judged on these, not on the totals. */
  uint32_t InternalFree() const;
  uint32_t InternalMin() const;
  uint32_t BlockMin() const  { return _blockMin; }   // smallest largest-free-block this run

  // Zero when there is no history - a cold start, or a first boot on firmware
  // that did not keep any.
  uint32_t PrevUptimeSecs() const { return _prevUptime; }
  uint32_t PrevHeapMin() const    { return _prevHeapMin; }
  uint32_t PrevBlockMin() const   { return _prevBlockMin; }
  bool     HaveHistory() const    { return _haveHistory; }

private:
  const char* _reasonName = "Unknown";
  bool     _wasCrash      = false;
  bool     _haveHistory   = false;
  uint32_t _bootCount     = 1;
  uint32_t _blockMin      = 0xFFFFFFFF;
  uint32_t _prevUptime    = 0;
  uint32_t _prevHeapMin   = 0;
  uint32_t _prevBlockMin  = 0;
  uint32_t _lastTickMs    = 0;
  uint32_t _lastWarnedHeap = 0;   // 0 = nothing reported yet
  uint32_t _lastMilestoneFree = 0;
  bool     _tasksReported = false;
  uint32_t _lastSpareTotal = 0;
  uint32_t _lastTaskCheckMs = 0;
  uint32_t _lastCurveMs = 0;      // 0 = no boot-window sample taken yet
};

extern DiagnosticsClass Diag;
