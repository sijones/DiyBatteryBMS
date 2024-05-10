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
    volatile bool dataavailable();
    bool OpenSerial(uint8_t rxPin, uint8_t txPin);
    char veName[buffLen][nameLen] = { };        // public buffer for received names
    char veValue[buffLen][valueLen] = { };      // public buffer for received values

    int frameIndex;                             // which line of the frame are we on
    volatile int veEnd;                         // current size (end) of the public buffer

    uint8_t rxPin;
    uint8_t txPin;
    bool mStop = false;   
    bool mRun = false;     
    byte FrameLength() {return veEnd;}                     

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