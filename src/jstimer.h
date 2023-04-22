/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2013 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Common utility timer handling functions
 * ----------------------------------------------------------------------------
 */
#ifndef JSTIMER_H_
#define JSTIMER_H_

#include "jsutils.h"
#include "jshardware.h"
#include "jspin.h"

typedef enum {
  UET_WAKEUP, ///< Does nothing except wake the device up!
  UET_SET, ///< Set a pin to a value
  UET_EXECUTE, ///< Execute something
#ifndef SAVE_ON_FLASH
  UET_WRITE_BYTE, ///< Write a byte to a DAC/Timer
  UET_READ_BYTE, ///< Read a byte from an analog input
  UET_WRITE_SHORT, ///< Write a short to a DAC/Timer
  UET_READ_SHORT, ///< Read a short from an analog input
#endif
} PACKED_FLAGS UtilTimerEventType;

#define UET_IS_SET_EVENT(T) (\
  ((T)==UET_SET))

#ifndef SAVE_ON_FLASH
#define UET_IS_BUFFER_EVENT(T) (\
  ((T)==UET_WRITE_BYTE) || \
  ((T)==UET_READ_BYTE) || \
  ((T)==UET_WRITE_SHORT) || \
  ((T)==UET_READ_SHORT))

#define UET_IS_BUFFER_READ_EVENT(T) (\
  ((T)==UET_READ_BYTE) || \
  ((T)==UET_READ_SHORT))

#define UET_IS_BUFFER_WRITE_EVENT(T) (\
  ((T)==UET_WRITE_BYTE) || \
  ((T)==UET_WRITE_SHORT))
#endif

#define UTILTIMERTASK_PIN_COUNT (4)

typedef struct UtilTimerTaskSet {
  Pin pins[UTILTIMERTASK_PIN_COUNT]; ///< pins to set
  uint8_t value; ///< value to set pins to
} PACKED_FLAGS UtilTimerTaskSet;

/** Task to write to a specific pin function - eg. a DAC or Timer or to read from an Analog
 * To send once, set var=buffer1, currentBuffer==nextBuffer==0
 * To repeat, set var=buffer1, currentBuffer==nextBuffer==buffer
 * To repeat, flipping between 2 buffers, set var=buffer1, currentBuffer==buffer1, nextBuffer=buffer2
 */
typedef struct UtilTimerTaskBuffer {
  JsVar *var; ///< variable to get data from
  JsVarRef currentBuffer; ///< The current buffer we're reading from (or 0)
  JsVarRef nextBuffer; ///< Subsequent buffer to read from (or 0)
  unsigned short currentValue; ///< current value being written (for writes)
  unsigned short charIdx; ///< Index of character in variable
  unsigned short endIdx; ///< Final index before we skip to the next var
  union {
    JshPinFunction pinFunction; ///< Pin function to write to
    Pin pin; ///< Pin to read from
  };
} PACKED_FLAGS UtilTimerTaskBuffer;

typedef void (*UtilTimerTaskExecFn)(JsSysTime time, void* userdata);

typedef struct UtilTimerTaskExec {
  UtilTimerTaskExecFn fn;
  void *userdata;
} PACKED_FLAGS UtilTimerTaskExec;


typedef union UtilTimerTaskData {
  UtilTimerTaskSet set;
  UtilTimerTaskBuffer buffer;
  UtilTimerTaskExec execute;
} UtilTimerTaskData;

typedef struct UtilTimerTask {
  int time; // time in future (not system time) at which to set pins (JshSysTime scaling, cropped to 32 bits)
  unsigned int repeatInterval; // if nonzero, repeat the timer
  UtilTimerTaskData data; // data used when timer is hit
  UtilTimerEventType type; // the type of this task - do we set pin(s) or read/write data
} PACKED_FLAGS UtilTimerTask;

void jstUtilTimerInterruptHandler();

/// Wait until the utility timer is totally empty (use with care as timers can repeat)
void jstUtilTimerWaitEmpty();

/// Return true if the utility timer is running
bool jstUtilTimerIsRunning();

/// Get the current timer offset - supply this when adding >1 timer task to ensure they are all executed at the same time relative to each other
uint32_t jstGetUtilTimerOffset();

/// Return true if a timer task for the given pin exists (and set 'task' to it)
bool jstGetLastPinTimerTask(Pin pin, UtilTimerTask *task);

/// Return true if a timer task for the given variable exists (and set 'task' to it)
bool jstGetLastBufferTimerTask(JsVar *var, UtilTimerTask *task);

/** returns false if timer queue was full... Changes the state of one or more pins at a certain time in the future (using a timer)
 * See utilTimerInsertTask for notes on timerOffset
 */
bool jstPinOutputAtTime(JsSysTime time, uint32_t *timerOffset, Pin *pins, int pinCount, uint8_t value);

// Do software PWM on the given pin, using the timer IRQs
bool jstPinPWM(JsVarFloat freq, JsVarFloat dutyCycle, Pin pin);

/** Execute the given function repeatedly after the given time period. If period=0, don't repeat. True on success or false on failure to schedule
 * See utilTimerInsertTask for notes on timerOffset
 */
bool jstExecuteFn(UtilTimerTaskExecFn fn, void *userdata, JsSysTime startTime, uint32_t period, uint32_t *timerOffset);

/// Stop executing the given function
bool jstStopExecuteFn(UtilTimerTaskExecFn fn, void *userdata);

/// Set the utility timer so we're woken up in whatever time period
bool jstSetWakeUp(JsSysTime period);

/** If the first timer task is a wakeup task, remove it. This stops
 * us filling the timer full of wakeup events if we wake up from sleep
 * before the wakeup event */
void jstClearWakeUp();

/// Start writing a string out at the given period between samples
bool jstStartSignal(JsSysTime startTime, JsSysTime period, Pin pin, JsVar *currentData, JsVar *nextData, UtilTimerEventType type);

/// Stop a timer task
bool jstStopBufferTimerTask(JsVar *var);

/// Stop ALL timer tasks (including digitalPulse - use this when resetting the VM)
void jstReset();

/** when system time is changed, also change the time in the timers.
This should be done with interrupts off */
void jstSystemTimeChanged(JsSysTime diff);

/// Dump the current list of timers
void jstDumpUtilityTimers();

/* Restart the utility timer with the right period. This should not normally
need to be called by anything outside jstimer.c */
void  jstRestartUtilTimer();

/** Queue a task up to be executed when a timer fires... return false on failure.
 * task.time is the delay at which to execute the task. If timerOffset!==NULL then
 * task.time is relative to the time at which timerOffset=jstGetUtilTimerOffset().
 * This allows pulse trains/etc to be scheduled in sync.
 */
bool utilTimerInsertTask(UtilTimerTask *task, uint32_t *timerOffset);

/// Remove the task that that 'checkCallback' returns true for. Returns false if none found
bool utilTimerRemoveTask(bool (checkCallback)(UtilTimerTask *task, void* data), void *checkCallbackData);

/// If 'checkCallback' returns true for a task, set 'task' to it and return true. Returns false if none found
bool utilTimerGetLastTask(bool (checkCallback)(UtilTimerTask *task, void* data), void *checkCallbackData, UtilTimerTask *task);


#endif /* JSTIMER_H_ */

