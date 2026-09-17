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

/* VE.Direct frame parser health.

   A parser fault and a wiring fault look identical from the outside: the shunt
   goes stale, the fallback takes over, and the log says only that it happened.
   These separate them. hexMidFrame in particular is the one that needs
   watching - Victron devices send asynchronous HEX messages unprompted and
   document that they can interrupt a text frame, which is the event that used
   to strand a half-built block for the next one to be appended to.

   recordsDropped and nameOverflows should both stay at zero. They sit on the
   bounds checks in VeDirectFrameHandler, so a non-zero count means something
   reached them by a route that is not yet understood - which is worth far more
   than the reboot it would otherwise have caused.

   Small and fixed on purpose: this rides in RTC slow memory beside the heap
   figures, and a copy is kept as the previous run's. */
struct DiagVeCounters {
  uint32_t hexMessages;     // asynchronous HEX messages seen at all
  uint32_t hexMidFrame;     // ...of those, the ones that cut into a text block
  uint32_t blocksDiscarded; // blocks dropped on a failed checksum
  uint32_t recordsDropped;  // records refused because the block was already full
  uint32_t nameOverflows;   // field names too long for the name buffer
};

/* The bursts that take internal RAM in a lump, stamped as they happen so a new
   low-water mark can say which of them it arrived beside.

   The low water is only checked once a second, and the dip that sets it is
   usually over well within that - so "what is running now" is the wrong
   question by the time anyone asks it. "What ran in the last few seconds" is
   the right one, and a timestamp per burst is enough to answer it. */
enum class DiagEvent : uint8_t {
  HaDiscovery,   // a pass of Home Assistant discovery messages went out
  WsConnect,     // a browser opened a web socket
  WsFullSync,    // the full settings payload was broadcast
  WsLogReplay,   // a Logs tab pulled the backlog
  PageServe,     // the web page itself was queued for sending
  Count
};

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

#if defined(BMS_S3)
  /* % of the last ~5s window each core's idle task actually ran - i.e. how
     much CPU headroom that core has free for new work right now. S3-only:
     needs per-core idle tasks to mean anything, and the classic ESP32 boards
     were not asked for this. -1 until the first two samples have
     landed, about 5s after boot. */
  float CpuHeadroomCore0() const { return _idleCore0Percent; }
  float CpuHeadroomCore1() const { return _idleCore1Percent; }
#endif

  const char* ResetReason() const { return _reasonName; }
  // Panic, watchdog or brownout - as opposed to a power-on or a reboot we asked
  // for. The distinction is the whole point of the boot line.
  bool     Crashed() const        { return _wasCrash; }
  uint32_t BootCount() const      { return _bootCount; }
  uint32_t UptimeSecs() const;

  /* Internal RAM, which on a PSRAM board is the only pool that can run out -
     WiFi and lwIP need DMA-capable memory and cannot use PSRAM. The warning
     thresholds are judged on these, and so are the figures the dashboard and
     Home Assistant show: a low water counted across PSRAM read 8.3MB on a board
     whose internal RAM had just touched 612 bytes. */
  uint32_t InternalFree() const;
  uint32_t InternalMin() const;
  uint32_t InternalBlock() const;
  uint32_t HeapMin() const   { return InternalMin(); }  // lowest internal free this run
  uint32_t BlockMin() const  { return _blockMin; }      // smallest internal largest-free-block this run

  /* Stamp a burst - see DiagEvent. One millis() write, so safe from any task
     and cheap enough to call on every pass of the thing being stamped. */
  void Note(DiagEvent e) { _eventMs[(size_t)e] = millis(); }
  /* Something outside this file that knows the live state worth adding to a
     low-water report - web socket clients, whether discovery is mid-sequence.
     Writes a short phrase into out and returns its length. Called from the main
     loop only. */
  using ContextFn = size_t (*)(char* out, size_t n);
  void SetContextProvider(ContextFn fn) { _contextFn = fn; }

  /* Counted where they happen, in the parser. Kept directly in RTC memory
     rather than mirrored there once a second, so a board that panics reports
     the counts as they actually stood when it went rather than as they were at
     the last tick - the events and the crash are seconds apart at most. */
  void VeHexMessage(bool midFrame);
  void VeBlockDiscarded();
  void VeRecordDropped();
  void VeNameOverflow();

  // This run so far.
  const DiagVeCounters& VeCounters() const;
  // And as the previous run left them. All zero when there is no history.
  const DiagVeCounters& PrevVeCounters() const { return _prevVe; }

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
  DiagVeCounters _prevVe  = {};
  uint32_t _lastTickMs    = 0;
  uint32_t _lastWarnedHeap = 0;   // 0 = nothing reported yet
  uint32_t _lastMilestoneFree = 0;
  bool     _tasksReported = false;
  uint32_t _lastSpareTotal = 0;
  uint32_t _lastTaskCheckMs = 0;
  uint32_t _lastCurveMs = 0;      // 0 = no boot-window sample taken yet
  uint32_t _eventMs[(size_t)DiagEvent::Count] = {};   // 0 = not seen this run
  ContextFn _contextFn = nullptr;

  void ReportLowWaterContext();

#if defined(BMS_S3)
  void     SampleCpuUsage();
  uint32_t _lastCpuTickMs    = 0;
  int64_t  _lastCpuSampleUs  = 0; // 0 = no sample taken yet
  uint32_t _prevIdle0Runtime = 0;
  uint32_t _prevIdle1Runtime = 0;
  bool     _haveIdlePrev     = false;
  float    _idleCore0Percent = -1;
  float    _idleCore1Percent = -1;
#endif
};

extern DiagnosticsClass Diag;
