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
 * JavaScript Serial Port Functions
 * ----------------------------------------------------------------------------
 */
#include "jsvar.h"

JsVar *jswrap_serial_constructor();
void jswrap_serial_setConsole(JsVar *parent, bool force);
void jswrap_serial_setup(JsVar *parent, JsVar *baud, JsVar *options);
void jswrap_serial_unsetup(JsVar *parent);
bool jswrap_serial_idle();
void jswrap_serial_print(JsVar *parent, JsVar *str);
void jswrap_serial_println(JsVar *parent, JsVar *str);
void jswrap_serial_write(JsVar *parent, JsVar *data);
void jswrap_serial_inject(JsVar *parent, JsVar *args);
