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
 * JavaScript SPI and I2C Functions
 * ----------------------------------------------------------------------------
 */
#include "jsvar.h"
#include "jspin.h"

JsVar *jswrap_spi_constructor();
void jswrap_spi_setup(JsVar *parent, JsVar *options);
JsVar *jswrap_spi_send(JsVar *parent, JsVar *data, Pin nss_pin);
void jswrap_spi_send4bit(JsVar *parent, JsVar *srcdata, int bit0, int bit1, Pin nss_pin);
void jswrap_spi_send8bit(JsVar *parent, JsVar *srcdata, int bit0, int bit1, Pin nss_pin);
void jswrap_spi_write(JsVar *parent, JsVar *args);

JsVar *jswrap_i2c_constructor();
void jswrap_i2c_setup(JsVar *parent, JsVar *options);
void jswrap_i2c_writeTo(JsVar *parent, JsVar *addressVar, JsVar *data);
JsVar *jswrap_i2c_readFrom(JsVar *parent, JsVar *addressVar, int nBytes);
