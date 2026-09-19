#pragma once

#ifndef FRAMEHANDLER_H_
#define FRAMEHANDLER_H_
#include <Arduino.h>

const byte frameLen = 22;                       // VE.Direct Protocol: max frame size is 22
const byte nameLen = 9;                         // VE.Direct Protocol: max name size is 9 including /0
const byte valueLen = 33;                       // VE.Direct Protocol: max value size is 33 including /0
const byte buffLen = 35;                        // Maximum number of lines possible from the device. Current protocol shows this to be the BMV700 at 33 lines.

class VeDirectFrameHandler {

public:
    VeDirectFrameHandler();
    
    void rxData(uint8_t inbyte);                // byte of serial data to be passed by the application
    void startReadTask();
    // Not volatile-qualified: that says nothing useful about a returned bool and
    // C++20 deprecates it. _newdata itself stays volatile, which is the part that
    // matters - it is written by the read task and read here.
    bool dataavailable();
    bool OpenSerial(uint8_t rxPin, uint8_t txPin);   // txPin 0 = not wired, receive-only
    char veName[buffLen][nameLen] = { };        // public buffer for received names
    char veValue[buffLen][valueLen] = { };      // public buffer for received values

    int frameIndex;                             // which line of the frame are we on
    volatile int veEnd;                         // current size (end) of the public buffer

    uint8_t rxPin;
    uint8_t txPin;
    bool mStop = false;
    bool mRun = false;
    byte FrameLength() {return veEnd;}

    /* Whether the link has earned the right to feed the charge logic.

       The checksum is one byte, so it lets through 1 in 256 of the blocks it
       should have caught. On a clean link that is 1 in 256 of almost nothing;
       on a damaged one - a field report had three blocks in four failing - it
       is a corrupted voltage, current or SOC applied a few times an hour, each
       looking exactly like a good reading. The failure rate is the only warning
       there is, so it is what decides.

       A clean link fails a block a handful of times a day, and the first block
       after the port opens is usually one of them (it is joined partway
       through). So one failure means nothing and two close together mean the
       wire is damaging bytes: trust goes at LINK_DISTRUST_FAILS in the last
       LINK_WINDOW blocks, and comes back only after LINK_WINDOW pass in a row.
       That gap is the hysteresis - without it a link failing three blocks in
       four handed over and back every few seconds, which is what the field log
       showed, and flooded the web log's replay with the handovers.

       Starts trusted, so a good cable is used from its first frame; a bad one
       is found out within a second or two. Read from loop(), the same task
       rxData() runs on, so there is nothing to lock. */
    static const uint8_t LINK_WINDOW = 32;
    static const uint8_t LINK_DISTRUST_FAILS = 2;
    bool    LinkTrusted() const   { return _trusted; }
    uint8_t RecentFailures() const { return (uint8_t)__builtin_popcount(_recentFails); }

private:

    portMUX_TYPE _VEmutex = portMUX_INITIALIZER_UNLOCKED;  

    enum States {                               // state machine
        IDLE,
        RECORD_BEGIN,
        RECORD_NAME,
        RECORD_VALUE,
        CHECKSUM,
        RECORD_HEX
    };

    int mState;                                 // current state
    volatile bool _newdata = false;
    uint32_t _recentFails = 0;                  // one bit per block, newest in bit 0, 1 = failed checksum
    bool     _trusted = true;                   // see LinkTrusted()
    uint8_t	mChecksum;                          // checksum value

    char * mTextPointer;                        // pointer to the private buffer we're writing to, name or value

    char mName[9];                              // buffer for the field name
    char mValue[33];                            // buffer for the field value
    char tempName[frameLen][nameLen];           // private buffer for received names
    char tempValue[frameLen][valueLen];         // private buffer for received values

    void textRxEvent(char *, char *);
    void frameEndEvent(bool);
    void logE(char *, char *);
    bool hexRxEvent(uint8_t);
    TaskHandle_t tHandle = NULL;

};

#endif // FRAMEHANDLER_H_