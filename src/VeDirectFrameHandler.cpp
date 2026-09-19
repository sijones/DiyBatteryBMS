/* framehandler.cpp
 *
 * The following Licence and permission is only for VeDirectFrameHandler.ccp and .h files.
 * 
 * Arduino library to read from Victron devices using VE.Direct protocol.
 * Derived from Victron framehandler reference implementation.
 *  
 * The MIT License
 * 
 * Copyright (c) 2019 Victron Energy BV
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *  
 * 2020.05.05 - 0.2 - initial release
 * 2020.06.21 - 0.2 - add MIT license, no code changes
 * 2020.08.20 - 0.3 - corrected #include reference
 * 
 */
 
#include <Arduino.h>
#include "VeDirectFrameHandler.h"
#include "WebLog.h"
// Parser health counters, kept in RTC memory so a panic does not take them with it
#include "Diagnostics.h"


void VETaskHandler(void * pointer)
{

    VeDirectFrameHandler *veTask = (VeDirectFrameHandler *) pointer;
    log_d("Entering Read Loop.");
    //veTask->mRun = true;
    //veTask->mRun
    while(true)
    {
		if(Serial1.available() > 0)
        	veTask->rxData(Serial1.read());
		else
			vTaskDelay(1 / portTICK_PERIOD_MS);
    }
   // Serial1.end();
   // log_d("Shutting Down Serial Task.");    
}

//#define MODULE "VE.Frame"	// Victron seems to use this to find out where logging messages were generated

// The name of the record that contains the checksum.
static constexpr char checksumTagName[] = "CHECKSUM";

// On the UART driver's event task, not loop() - it only bumps a counter
static void onUartError(hardwareSerial_error_t err)
{
	switch (err) {
	case UART_FIFO_OVF_ERROR:
	case UART_BUFFER_FULL_ERROR:
		Diag.VeUartOverrun();
		break;
	case UART_FRAME_ERROR:
	case UART_BREAK_ERROR:
	case UART_PARITY_ERROR:
		Diag.VeUartLineError();
		break;
	default:
		break;
	}
}

bool VeDirectFrameHandler::OpenSerial(uint8_t _rxPin,uint8_t _txPin)
{
    Serial1.end();

	// RX is the only pin we need - the shunt transmits unprompted and we never
	// talk back. TX is optional, and 0 means "not wired", giving a receive-only UART.
	if (_rxPin == 0) {
		log_e("VE.Direct RX pin not configured. Please configure via web interface.");
		return false;
	}

	#ifdef ESP32
	if (_txPin > 33) // ESP32 can't use pins higher than 34 for output.
		return false;
	#endif
    int8_t txArg = _txPin ? (int8_t) _txPin : (int8_t) -1;
    log_d("Opening Serial Port rxPin: %i, txPin %i",_rxPin,txArg);

    /* Sized against a real frame, not a guess - measured 141-165 bytes on a
       real shunt (a one-off byte-count log at frame-complete, since removed),
       well under the 256-byte Arduino-ESP32 default. That default still
       leaves little slack for loop() being briefly busy elsewhere before it
       drains Serial1 - a stall past the buffer's free space corrupts that
       second's frame. Must be set before begin(). */
#ifdef BOARD_HAS_PSRAM
    /* CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL on this platform is 4096 bytes -
       anything at or above that is eligible to come from PSRAM instead of the
       internal budget every other board here has to share (confirmed against
       esp-idf's uart_driver_install: UART_MALLOC_CAPS is MALLOC_CAP_DEFAULT,
       not forced internal, because CONFIG_UART_ISR_IN_IRAM is unset on this
       platform's sdkconfig). Generous on purpose - this is close to free on
       a board with 8MB of PSRAM to spare. */
    size_t heapBefore = ESP.getFreeHeap();
    Serial1.setRxBufferSize(8192);
#else
    // ~2.3x the largest frame measured on real hardware - internal RAM is
    // scarce on these boards, so this stays modest rather than padded for a
    // worst case that does not happen here.
    Serial1.setRxBufferSize(384);
#endif

    Serial1.begin(19200, SERIAL_8N1, _rxPin, txArg);
    Serial1.flush();
    /* A failed checksum says a byte went wrong, never where. The UART driver
       knows: a framing or break error is the wire (noise, a missing ground, a
       level problem), an overflow is this end not draining the port in time.
       Those need opposite fixes, so they are counted apart. After begin(),
       because end() clears the callback, and this starts the driver's event
       task (2KB of stack) to deliver it. */
    Serial1.onReceiveError(onUartError);

#ifdef BOARD_HAS_PSRAM
    // One-off confirmation that the 8KB buffer actually came from PSRAM
    // rather than the internal heap it was sized specifically to avoid -
    // remove alongside the frame-size logging in rxData() once confirmed.
    long delta = (long) heapBefore - (long) ESP.getFreeHeap();
    WS_LOG_I("VE.Direct RX buffer: internal heap dropped by %ld bytes opening it (expect near 0 if it landed in PSRAM)", delta);
#endif

    return true;
}

VeDirectFrameHandler::VeDirectFrameHandler() :
	//mStop(false),	// don't know what Victron uses this for, not using
	mState(IDLE),
	mChecksum(0),
	mTextPointer(0),
    tempName(),
    tempValue(),
	frameIndex(0),
	veName(),
	veValue(),
	veEnd(0)
{
}


/*
 *	rxData
 *  This function is called by the application which passes a byte of serial data
 *  It is unchanged from Victron's example code
 */
void VeDirectFrameHandler::rxData(uint8_t inbyte)
{
	if (mStop) return;
	if ( (inbyte == ':') && (mState != CHECKSUM) ) {
		/* Count the message, and separately whether it arrived partway through
		   a text block - the guard is what makes the two different, since a ':'
		   while already in RECORD_HEX is payload, not a new message. A mid-frame
		   arrival is the case Victron documents and the one that used to do the
		   damage; between blocks it is harmless. */
		if (mState != RECORD_HEX)
			Diag.VeHexMessage(frameIndex > 0 || mState != IDLE);
		mState = RECORD_HEX;
	}
	if (mState != RECORD_HEX) {
		mChecksum += inbyte;
	}
	inbyte = toupper(inbyte);

	switch(mState) {
	case IDLE:
		/* wait for \n of the start of an record */
		switch(inbyte) {
		case '\n':
			mState = RECORD_BEGIN;
			break;
		case '\r': /* Skip */
		default:
			break;
		}
		break;
	case RECORD_BEGIN:
		mTextPointer = mName;
		*mTextPointer++ = inbyte;
		mState = RECORD_NAME;
		break;
	case RECORD_NAME:
		// The record name is being received, terminated by a \t
		switch(inbyte) {
		case '\t':
			// the Checksum record indicates a EOR
			if ( mTextPointer < (mName + sizeof(mName)) ) {
				*mTextPointer = 0; /* Zero terminate */
				if (strcmp(mName, checksumTagName) == 0) {
					mState = CHECKSUM;
					break;
				}
			} else {
				/* A name that filled mName exactly left no room for the
				   terminator above, so it was never written. textRxEvent's
				   strcpy would then read past mName into mValue and beyond,
				   looking for a zero that is not there, and write all of it
				   into a 9-byte tempName slot. Terminate it here instead. An
				   over-long name matches no field this parser wants, so the
				   record is junk either way - the point is that it is junk of
				   a bounded length. */
				mName[sizeof(mName) - 1] = 0;
				Diag.VeNameOverflow();
			}
			mTextPointer = mValue; /* Reset value pointer */
			mState = RECORD_VALUE;
			break;
		default:
			// add byte to name, but do no overflow
			if ( mTextPointer < (mName + sizeof(mName)) )
				*mTextPointer++ = inbyte;
			break;
		}
		break;
	case RECORD_VALUE:
		// The record value is being received.  The \r indicates a new record.
		switch(inbyte) {
		case '\n':
			// forward record, only if it could be stored completely
			if ( mTextPointer < (mValue + sizeof(mValue)) ) {
				*mTextPointer = 0; // make zero ended
				textRxEvent(mName, mValue);
			}
			mState = RECORD_BEGIN;
			break;
		case '\r': /* Skip */
			break;
		default:
			// add byte to value, but do no overflow
			if ( mTextPointer < (mValue + sizeof(mValue)) )
				*mTextPointer++ = inbyte;
			break;
		}
		break;
	case CHECKSUM:
	{
		bool valid = mChecksum == 0;
		if (!valid)
			log_d("[CHECKSUM] Invalid frame from VE");
		mChecksum = 0;
		mState = IDLE;
		frameEndEvent(valid);
		break;
	}
	case RECORD_HEX:
		if (hexRxEvent(inbyte)) {
			mChecksum = 0;
			mState = IDLE;
			/* A device sends asynchronous HEX messages unprompted, and Victron
			   documents that they can interrupt a text frame mid-record. The
			   half-built block left behind can never validate - its checksum
			   accumulation was abandoned partway - so drop it here rather than
			   leave frameIndex standing for the next block's records to be
			   appended to it. That merge was the route to writing past the end
			   of the temp buffers. */
			frameIndex = 0;
		}
		break;
	}
}

void VeDirectFrameHandler::startReadTask()
{

    if (mRun)
    {
        mStop = true;
        delay(10);
        mRun = false;
        delay(10);
    }

    mStop = false;
    mRun = true;

//    log_d("Creating VE Read Task");
//    xTaskCreatePinnedToCore(
//    &VETaskHandler,   /* Task function. */
//    "VETaskHandler",  /* String with name of task. */
//    5000,             /* Stack size in bytes. */
//    this,             /* Parameter passed as input of the task */
//    4,                /* Priority of the task. */
//    &tHandle,         /* Task handle. */
//    1);  

}

bool VeDirectFrameHandler::dataavailable()
{
    if (_newdata)
        {
            _newdata = false;
            return true;
        }
    else
        return false;
}
/*
 * textRxEvent
 * This function is called every time a new name/value is successfully parsed.  It writes the values to the temporary buffer.
 */
void VeDirectFrameHandler::textRxEvent(char * mName, char * mValue) {
	/* frameIndex is only ever reset by frameEndEvent, which is only reached
	   through the CHECKSUM state - so a block that loses its Checksum tag to a
	   single corrupted or dropped byte never resets it, and the next block's
	   records are appended to this one's. frameLen is the protocol's stated 22
	   fields per block, so no single compliant block can overflow - but a merge
	   trivially does: a published BMV-702 capture sends 12 fields in one block
	   and 18 in the next, which merged is 30. Past the end these were unbounded
	   strcpys running off tempValue into whatever the linker put after this
	   object - a global here - which is the kind of corruption that panics
	   minutes later and nowhere near the cause.

	   Dropping the surplus is the whole fix. A merged block carries the residue
	   of the checksum it missed, so it almost always fails validation and
	   frameEndEvent discards it and clears frameIndex - one second of stale
	   data instead of a silently corrupted heap. In the 1-in-256 case where the
	   residue happens to land on zero it is accepted, but frameEndEvent already
	   bounds veEnd, so the worst of that is a few junk fields for one second. */
	if ( frameIndex >= frameLen ) {
		Diag.VeRecordDropped();
		return;
	}
	strcpy(tempName[frameIndex], mName);    // copy name to temporary buffer
	strcpy(tempValue[frameIndex], mValue);  // copy value to temporary buffer
	frameIndex++;
}

/*
 *	frameEndEvent
 *  This function is called at the end of the received frame.  If the checksum is valid, the temp buffer is read line by line.
 *  If the name exists in the public buffer, the new value is copied to the public buffer.	If not, a new name/value entry
 *  is created in the public buffer.
 */
void VeDirectFrameHandler::frameEndEvent(bool valid) {
	// Every block that reached a checksum, good or bad - see LinkTrusted()
	_recentFails = (_recentFails << 1) | (valid ? 0u : 1u);
	const uint8_t fails = RecentFailures();
	if (_trusted && fails >= LINK_DISTRUST_FAILS)
		_trusted = false;
	else if (!_trusted && fails == 0)
		_trusted = true;

	if ( valid ) {
        taskENTER_CRITICAL(&_VEmutex);
		for ( int i = 0; i < frameIndex; i++ ) {				// read each name already in the temp buffer
			bool nameExists = false;
			for ( int j = 0; j <= veEnd; j++ ) {				// compare to existing names in the public buffer
				if ( strcmp(tempName[i], veName[j]) == 0 ) {	
					strcpy(veValue[j], tempValue[i]);			// overwrite tempValue in the public buffer
					nameExists = true;
					break;
				}
			}
			if ( !nameExists ) {
				strcpy(veName[veEnd], tempName[i]);				// write new Name to public buffer
				strcpy(veValue[veEnd], tempValue[i]);			// write new Value to public buffer
				// Read-modify-write spelled out: ++ on a volatile is deprecated in
				// C++20 because the order of the parts is unspecified.
				veEnd = veEnd + 1;								// increment end of public buffer
				if ( veEnd >= buffLen ) {						// stop any buffer overrun
					veEnd = buffLen - 1;
				}
			}
		}
        _newdata = true;
        taskEXIT_CRITICAL(&_VEmutex);
		log_d("Frame Index Value %d",frameIndex);
	}
	else {
		/* A failed checksum is not by itself a fault - it is the normal outcome
		   of a block that was interrupted, and the parser recovering as it
		   should. It only means something as a rate: a handful a day is a link
		   working, a steady stream is a wiring or baud problem. */
		Diag.VeBlockDiscarded();
	}
	frameIndex = 0;	// reset frame
}

/*
 *	logE
 *  This function included for continuity and possible future use.	
 */
void VeDirectFrameHandler::logE(char * _module, char * _error) {
    log_e("Module: %c - Error: %c",_module,_error);
	//Serial.print("MODULE: ");
    //Serial.println(module);
    //Serial.print("ERROR: ");
    //Serial.println(error);
	return;
}

/*
 *	hexRxEvent
 *  Consumes an asynchronous HEX message. True means "this message has ended",
 *  at which point rxData clears the checksum and returns to text parsing.
 */
bool VeDirectFrameHandler::hexRxEvent(uint8_t inbyte) {
	/* A HEX message runs from ':' to '\n', so only the newline ends it. The
	   previous stub returned true on the very first byte, which handed the
	   parser back to text mode in the middle of a hex payload - the rest of
	   that payload was then read as record names and values. Nothing here
	   decodes the message; it only has to be stepped over cleanly, which is
	   all this project needs, since it never asks for one. */
	return (inbyte == '\n');
}