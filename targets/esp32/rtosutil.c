/*
 * This file is designed to support FREERTOS functions in Espruino,
 * a JavaScript interpreter for Microcontrollers designed by Gordon Williams
 *
 * Copyright (C) 2016 by Juergen Marsch 
 *
 * This Source Code Form is subject to the terms of the Mozilla Publici
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * This file is designed to be parsed during the build process
 *
 * Task, queue and timer specific exposed components.
 * ----------------------------------------------------------------------------
 */

#include "jsinteractive.h"
#include "jstimer.h"
 
#include "rom/uart.h"
#include "rtosutil.h"

#include "soc/timer_group_struct.h"
#include "driver/timer.h"

#include <stdio.h>
#include <string.h>

#define TIMER_INTR_SEL TIMER_INTR_LEVEL  /*!< Timer level interrupt */
#define TIMER_GROUP    TIMER_GROUP_0     /*!< Test on timer group 0 */
#define TIMER_DIVIDER  80               /*!< Hardware timer clock divider */
#define TIMER_SCALE    (TIMER_BASE_CLK / TIMER_DIVIDER)  /*!< used to calculate counter value */
#define TIMER_FINE_ADJ   (1.4*(TIMER_BASE_CLK / TIMER_DIVIDER)/1000000) /*!< used to compensate alarm value */
BaseType_t xHigherPriorityTaskWoken = pdFALSE;
void IRAM_ATTR espruino_isr(void *para)
{
	int idx=(int)para;

#if CONFIG_IDF_TARGET_ESP32
	TIMERG0.hw_timer[TIMER_0].update = 1;
	TIMERG0.int_clr_timers.t0 = 1;
#elif CONFIG_IDF_TARGET_ESP32S3
	TIMERG0.hw_timer[TIMER_0].update.tn_update=1;
	TIMERG0.int_clr_timers.t0_int_clr=1;
#else
	#error Not an ESP32 or ESP32-S3
#endif	
	jstUtilTimerInterruptHandler();
}

void timers_Init(){
  int i;
  for(i = 0; i < timerMax; i++){
	ESP32Timers[i].name = NULL;
  }
}
int timer_indexByName(char *timerName){
  int i;
  for(i = 0; i < timerMax; i++){
	if(ESP32Timers[i].name == NULL) return -1;
	if(strcmp(timerName,ESP32Timers[i].name) == 0){
	  return i;
	}
  }
  return -1;
}
int timer_Init(char *timerName,int group,int index,int isr_idx){
  int i;
  for(i = 0; i < timerMax; i++){
	if(ESP32Timers[i].name == NULL){
	  ESP32Timers[i].name = timerName;
	  ESP32Timers[i].group = group;
	  ESP32Timers[i].index = index;
      timer_config_t config;
      config.alarm_en = 1;
      config.auto_reload = 1;
      config.counter_dir = TIMER_COUNT_UP;
      config.divider = TIMER_DIVIDER;
      config.intr_type = TIMER_INTR_SEL;
      config.counter_en = TIMER_PAUSE;
      timer_init(group, index, &config);/*Configure timer*/
      timer_pause(group, index);/*Stop timer counter*/
      timer_set_counter_value(group, index, 0x00000000ULL);/*Load counter value */
      timer_enable_intr(group, index);
      if(isr_idx == 0){
	    //ESP32Timers[i].taskToNotifyIdx = task_indexByName("TimerTask");
        timer_isr_register(group, index, espruino_isr, (void*) i, ESP_INTR_FLAG_IRAM, NULL);
      }
      else{
	    //timer_isr_register(group, index, test_isr, (void*) i, ESP_INTR_FLAG_IRAM, NULL);  
      }
      return i;
	}
  }
  return -1;
}


void timer_Start(int idx,uint64_t duration)
{
	timer_enable_intr(ESP32Timers[idx].group, ESP32Timers[idx].index);
	
	timer_set_alarm_value(ESP32Timers[idx].group, ESP32Timers[idx].index, duration - TIMER_FINE_ADJ);
	
#if CONFIG_IDF_TARGET_ESP32
	TIMERG0.hw_timer[idx].config.alarm_en = 1;
#elif CONFIG_IDF_TARGET_ESP32S3
	TIMERG0.hw_timer[idx].config.tn_alarm_en=1;
#else
	#error Not an ESP32 or ESP32-S3
#endif
	
	timer_start(ESP32Timers[idx].group, ESP32Timers[idx].index);
}
void timer_Reschedule(int idx,uint64_t duration)
{
	timer_set_alarm_value(ESP32Timers[idx].group, ESP32Timers[idx].index, duration - TIMER_FINE_ADJ);

#if CONFIG_IDF_TARGET_ESP32
	TIMERG0.hw_timer[idx].config.alarm_en = 1;
#elif CONFIG_IDF_TARGET_ESP32S3
	TIMERG0.hw_timer[idx].config.tn_alarm_en=1;
#else
	#error Not an ESP32 or ESP32-S3
#endif
	
}
void timer_List()
{
}
