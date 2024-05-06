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


void VETaskHandler(void * pointer)
{
    log_d("Getting Pointer.");
    VeDirectFrameHandler *veTask = (VeDirectFrameHandler *) pointer;
    log_d("Entering Read Loop.");
    veTask->mRun = true;
    
    while(veTask->mRun)
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

bool VeDirectFrameHandler::OpenSerial(uint8_t _rxPin,uint8_t _txPin)
{
    Serial1.end();

	//if (_txPin > 33)
	//	return false;
    log_d("Opening Serial Port rxPin: %i, txPin %i",_rxPin,_txPin);
    Serial1.begin(19200, SERIAL_8N1, _rxPin, _txPin);
    Serial1.flush();
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

    log_d("Creating VE Read Task");
    xTaskCreatePinnedToCore(
    &VETaskHandler,   /* Task function. */
    "VETaskHandler",  /* String with name of task. */
    4096,             /* Stack size in bytes. */
    this,             /* Parameter passed as input of the task */
    4,                /* Priority of the task. */
    &tHandle,         /* Task handle. */
    1);  

}

volatile bool VeDirectFrameHandler::dataavailable()
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
				veEnd++;										// increment end of public buffer
				if ( veEnd >= buffLen ) {						// stop any buffer overrun
					veEnd = buffLen - 1;
				}
			}
		}
        _newdata = true;
        taskEXIT_CRITICAL(&_VEmutex);
		log_d("Frame Index Value %d",frameIndex);
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
 *  This function included for continuity and possible future use.	
 */
bool VeDirectFrameHandler::hexRxEvent(uint8_t inbyte) {
	return true;		// stubbed out for future
}