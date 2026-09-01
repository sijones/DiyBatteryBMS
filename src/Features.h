#pragma once

/* Optional feature registry.
 *
 * Some features do not belong to every build: they need hardware only one board
 * has, or they exist for one integration rather than for the project at large.
 * Before this file each of them had to be written into the shared files by hand
 * - an include and a call in main.cpp's setup(), another in loop(), a block of
 * fields in buildDataDoc(), a matching block of setters in handleWSRequest(),
 * its NVS keys in mEEPROM.h and its defaults in config.h - every one of those
 * wrapped in the same #ifdef. Six shared files edited to add one self-contained
 * thing, and the same six edited again to add the next one.
 *
 * A feature now says all of that itself. It subclasses Feature, overrides the
 * hooks it cares about, and declares one instance of itself at file scope in
 * its own .cpp:
 *
 *     static MyFeature _myFeature;   // registers itself, see below
 *
 * PlatformIO compiles everything under src/, so dropping the pair of files in
 * is the whole installation - there is no list of features anywhere to add it
 * to, and nothing shared to merge. A feature that must not exist on some builds
 * wraps its own .cpp in its own #ifdef rather than asking main.cpp to.
 *
 * NVS keys and defaults belong to the feature too, declared in its header
 * beside the code that reads them, so config.h and mEEPROM.h stay untouched as
 * well. `const` at namespace scope is internally linked in C++, so a key
 * defined in a feature header cannot collide with anything at link time.
 *
 * How the registration works, and why it is safe this early: each Feature links
 * itself into a static chain from its constructor, which for a file-scope
 * instance runs during dynamic initialisation, before setup(). The head pointer
 * below is constant-initialised, which the language orders strictly before all
 * dynamic initialisation in every translation unit - so the list is a
 * well-defined empty list by the time the first constructor looks at it,
 * whichever object file the linker happens to put first. This is the one part
 * of the mechanism worth not rearranging: giving _head a runtime initialiser
 * would reintroduce the static initialisation order fiasco and the failure
 * would be a silently missing feature, not a build error.
 *
 * Registration order is the reverse of construction order, which across
 * translation units the standard leaves unspecified. Nothing here may depend on
 * it: hooks run in an arbitrary order and features must not rely on another
 * having been set up first. They are independent by construction - that is what
 * makes them droppable in the first place.
 */

#include <ArduinoJson.h>

class Feature {
  public:
    Feature();
    virtual ~Feature() {}

    /** Short name, for logging only. */
    virtual const char* Name() const = 0;

    /** Called once from setup(), after WiFi, NVS and the web server are up, so
        a feature may read its settings and start tasks or sockets. */
    virtual void Setup() {}

    /** Called every pass of the main loop. Must return promptly - this shares
        the loop with the charge logic, so anything that blocks belongs in a
        task of the feature's own. */
    virtual void Loop() {}

    /** Add this feature's fields to the status/settings JSON. `all` is true for
        a full snapshot (the settings page asking for everything) and false for
        the routine live broadcast, matching buildDataDoc()'s own parameter -
        settings a user edits belong behind it, live readings do not. */
    virtual void BuildDoc(JsonDocument& doc, bool all) { (void)doc; (void)all; }

    /** Apply any of this feature's keys present in an inbound WebSocket
        message. Return true if at least one was recognised, which is what tells
        handleWSRequest() the message was not an unknown request. */
    virtual bool HandleWS(JsonDocument& doc) { (void)doc; return false; }

    // Run one hook across every registered feature. Called only from main.cpp
    // and HTTPWSFunctions.h - these four call sites are the entire cost the
    // shared files pay, however many features exist.
    static void SetupAll();
    static void LoopAll();
    static void BuildDocAll(JsonDocument& doc, bool all);
    static bool HandleWSAll(JsonDocument& doc);

    /** How many features this build ended up with - worth a line in the boot
        log, since which ones are compiled in is a property of the env rather
        than of the source. */
    static uint8_t Count();

  private:
    Feature* _next;
    static Feature* _head;   // constant-initialised - see the note above
};
