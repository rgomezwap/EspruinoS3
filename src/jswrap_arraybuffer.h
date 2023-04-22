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
 * JavaScript methods and functions in the global namespace
 * ----------------------------------------------------------------------------
 */
#include "jsvar.h"

JsVar *jswrap_arraybuffer_constructor(JsVarInt byteLength);
JsVar *jswrap_typedarray_constructor(JsVarDataArrayBufferViewType type, JsVar *arr, JsVarInt byteOffset, JsVarInt length);
void jswrap_arraybufferview_set(JsVar *parent, JsVar *arr, int offset);
JsVar *jswrap_arraybufferview_map(JsVar *parent, JsVar *funcVar, JsVar *thisVar);
JsVar *jswrap_arraybufferview_subarray(JsVar *parent, JsVarInt begin, JsVar *endVar);
JsVar *jswrap_arraybufferview_sort(JsVar *array, JsVar *compareFn);
