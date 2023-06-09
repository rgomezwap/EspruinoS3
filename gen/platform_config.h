
// Automatically generated header file for ESP32
// Generated by scripts/build_platform_config.py

#ifndef _PLATFORM_CONFIG_H
#define _PLATFORM_CONFIG_H


#define PC_BOARD_ID "ESP32"
#define PC_BOARD_CHIP "ESP32"
#define PC_BOARD_CHIP_FAMILY "ESP32"

#define LINKER_END_VAR _end
#define LINKER_ETEXT_VAR _etext


// SYSTICK is the counter that counts up and that we use as the real-time clock
// The smaller this is, the longer we spend in interrupts, but also the more we can sleep!
#define SYSTICK_RANGE 0x1000000 // the Maximum (it is a 24 bit counter) - on Olimexino this is about 0.6 sec
#define SYSTICKS_BEFORE_USB_DISCONNECT 2

#define DEFAULT_BUSY_PIN_INDICATOR (Pin)-1 // no indicator
#define DEFAULT_SLEEP_PIN_INDICATOR (Pin)-1 // no indicator

// When to send the message that the IO buffer is getting full
#define IOBUFFER_XOFF ((TXBUFFERMASK)*6/8)
// When to send the message that we can start receiving again
#define IOBUFFER_XON ((TXBUFFERMASK)*3/8)



#define RAM_TOTAL (512*1024)
#define FLASH_TOTAL (0*1024)

#define JSVAR_CACHE_SIZE                2500 // Number of JavaScript variables in RAM
#define FLASH_AVAILABLE_FOR_CODE        1572864
#define FLASH_PAGE_SIZE                 4096
#define FLASH_START                     0x8000000

#define FLASH_SAVED_CODE_START            3276800
#define FLASH_SAVED_CODE_LENGTH           917504

#define CLOCK_SPEED_MHZ                      160
#define USART_COUNT                          3
#define SPI_COUNT                            2
#define I2C_COUNT                            2
#define ADC_COUNT                            2
#define DAC_COUNT                            0
#define EXTI_COUNT                           40

#define DEFAULT_CONSOLE_DEVICE              EV_SERIAL1
#define DEFAULT_CONSOLE_BAUDRATE 115200

#define IOBUFFERMASK 255 // (max 255) amount of items in event buffer - events take 5 bytes each
#define TXBUFFERMASK 127 // (max 255) amount of items in the transmit buffer - 2 bytes each
#define UTILTIMERTASK_TASKS (16) // Must be power of 2 - and max 256


// definition to avoid compilation when Pin/platform config is not defined
#define IS_PIN_USED_INTERNALLY(PIN) ((false))
#define IS_PIN_A_LED(PIN) ((false))
#define IS_PIN_A_BUTTON(PIN) ((false))

#endif // _PLATFORM_CONFIG_H

