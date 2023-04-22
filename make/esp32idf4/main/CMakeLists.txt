idf_component_register(
						SRCS
							"../../../targets/esp32/main.c"
							"../../../targets/esp32/jshardwareESP32.c"
							"../../../targets/esp32/jshardwareUart.c"
							"../../../targets/esp32/jshardwareSpi.c"
							"../../../targets/esp32/jshardwarePulse.c"
							"../../../targets/esp32/jshardwareAnalog.c"
							"../../../targets/esp32/jshardwarePWM.c"
							"../../../targets/esp32/jshardwareTimer.c"
							"../../../targets/esp32/rtosutil.c"
							"../../../targets/esp32/jshardware.c"
							"../../../targets/esp32/jshardwareI2c.c"
							"../../../targets/esp32/jswrap_esp32.c"
							"../../../targets/esp32/jswrap_rtos.c"
							"../../../src/jsutils.c"
							"../../../src/jsdevices.c"
							"../../../src/jsinteractive.c"
							"../../../src/jsflags.c"
							"../../../src/jsflash.c"
							"../../../src/jslex.c"
							"../../../src/jsnative.c"
							"../../../src/jsparse.c"
							"../../../src/jspin.c"
							"../../../src/jstimer.c"
							"../../../src/jsvar.c"
							"../../../src/jsvariterator.c"
							"../../../src/jswrap_error.c"
							"../../../src/jswrap_espruino.c"
							"../../../src/jswrap_io.c"
							"../../../src/jswrap_json.c"
							"../../../src/jswrap_object.c"
							"../../../src/jswrap_stream.c"
							"../../../src/jswrap_arraybuffer.c"
							"../../../src/jswrap_dataview.c"
							"../../../src/jswrap_functions.c"
							"../../../src/jswrap_regexp.c"
							"../../../src/jswrap_array.c"
							"../../../src/jswrap_date.c"
							"../../../src/jswrap_flash.c"
							"../../../src/jswrap_interactive.c"
							"../../../src/jswrap_modules.c"
							"../../../src/jswrap_number.c"
							"../../../src/jswrap_onewire.c"
							"../../../src/jswrap_pin.c"
							"../../../src/jswrap_pipe.c"
							"../../../src/jswrap_process.c"
							"../../../src/jswrap_promise.c"
							"../../../src/jswrap_serial.c"
							"../../../src/jswrap_spi_i2c.c"
							"../../../src/jswrap_storage.c"
							"../../../src/jswrap_string.c"
							"../../../src/jswrap_waveform.c"
							"../../../src/jsi2c.c"
							"../../../src/jsserial.c"
							"../../../src/jsspi.c"
							"../../../src/jshardware_common.c"
							"../../../libs/math/jswrap_math.c"
							"../../../libs/compression/compress_rle.c"
							"../../../libs/compression/compress_heatshrink.c"
							"../../../libs/compression/jswrap_heatshrink.c"
							"../../../libs/compression/heatshrink/heatshrink_decoder.c"
							"../../../libs/compression/heatshrink/heatshrink_encoder.c"
							"../../../libs/crypto/jswrap_crypto.c"
							"../../../libs/network/jswrap_net.c"
							"../../../libs/network/jswrap_wifi.c"
							"../../../libs/network/network.c"
							"../../../libs/network/socketerrors.c"
							"../../../libs/network/socketserver.c"
							"../../../libs/network/esp32/jswrap_esp32_network.c"
							"../../../libs/network/esp32/network_esp32.c"
							"../../../libs/network/telnet/jswrap_telnet.c"
							"../../../libs/network/js/jswrap_jsnetwork.c"
							"../../../libs/network/js/network_js.c"
							"../../../libs/network/http/jswrap_http.c"
							"../../../libs/filesystem/jswrap_file.c"
							"../../../libs/filesystem/jswrap_fs.c"
							"../../../libs/filesystem/fat_sd/ff.c"
							"../../../libs/filesystem/fat_sd/spi_diskio.c"
							"../../../libs/filesystem/fat_sd/option/ccsbcs.c"
							"../../../libs/graphics/graphics.c"
							"../../../libs/graphics/jswrap_graphics.c"
							"../../../libs/graphics/lcd_js.c"
							"../../../libs/graphics/lcd_arraybuffer.c"
							"../../../libs/graphics/bitmap_font_4x6.c"
							"../../../libs/graphics/bitmap_font_6x8.c"
							"../../../libs/graphics/vector_font.c"
							"../../../gen/jspininfo.c"
							"../../../gen/jswrapper.c"
						INCLUDE_DIRS
							"../../../src/"
							"../../../gen/"
							"../../../targets/esp32/"
							"../../../libs/compression/"
							"../../../libs/compression/heatshrink"
							"../../../libs/math/"
							"../../../libs/network/"
							"../../../libs/network/esp32/"
							"../../../libs/network/js/"
							"../../../libs/network/http/"
							"../../../libs/crypto/"
							"../../../libs/filesystem"
							"../../../libs/graphics"
					  )

	target_compile_options(${COMPONENT_TARGET} PUBLIC -DESP_PLATFORM -DESP32=1 -DJSVAR_MALLOC -DUSE_ESP32 -DESP_STACK_SIZE=25000)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_DEBUGGER -DUSE_TAB_COMPLETE -DUSE_HEATSHRINK)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_MATH -DESP32 -DEMBEDDED)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_FILESYSTEM)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_GRAPHICS -DUSE_FONT_6X8)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_NET -DUSE_TELNET -DUSE_CRYPTO -DMBEDTLS_CIPHER_MODE_CTR)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DMBEDTLS_CIPHER_MODE_CBC -DMBEDTLS_CIPHER_MODE_CFB -DUSE_SHA256)
	target_compile_options(${COMPONENT_TARGET} PUBLIC -DUSE_SHA512 -DUSE_TLS -DUSE_AES)

target_compile_options(${COMPONENT_TARGET} PUBLIC -Og -fno-strict-aliasing -ffunction-sections -fdata-sections -fstrict-volatile-bitfields -fgnu89-inline -mlongcalls -nostdlib -MMD -MP -std=gnu99 -mfix-esp32-psram-cache-issue)

# esta configuracion en el ESP-IDF por defecto convierte estos warnings en errores, analizarlos y corregir el codigo mejor
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-pointer-sign)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-int)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-maybe-uninitialized)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-return-type)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-switch)

# estos defines los quite para que la salida en consola sea mas breve para identificar errores, pero luego comentarlos de nuevo
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-cast-function-type)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
