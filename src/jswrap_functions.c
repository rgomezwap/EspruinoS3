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
 * This file is designed to be parsed during the build process
 *
 * JavaScript methods and functions in the global namespace
 * ----------------------------------------------------------------------------
 */
#include <math.h>
#include "jswrap_functions.h"
#include "jslex.h"
#include "jsparse.h"
#include "jsinteractive.h"


/*JSON{
  "type" : "variable",
  "name" : "arguments",
  "generate" : "jswrap_arguments",
  "return" : ["JsVar","An array containing all the arguments given to the function"]
}
A variable containing the arguments given to the function:

```
function hello() {
  console.log(arguments.length, JSON.stringify(arguments));
}
hello()        // 0 []
hello("Test")  // 1 ["Test"]
hello(1,2,3)   // 3 [1,2,3]
```

**Note:** Due to the way Espruino works this is doesn't behave exactly the same
as in normal JavaScript. The length of the arguments array will never be less
than the number of arguments specified in the function declaration:
`(function(a){ return arguments.length; })() == 1`. Normal JavaScript
interpreters would return `0` in the above case.

 */
extern JsExecInfo execInfo;
JsVar *jswrap_arguments() {
  JsVar *scope = 0;
#ifdef ESPR_NO_LET_SCOPING
  if (execInfo.scopesVar) // if no let scoping, the top of the scopes list is the function
    scope = jsvGetLastArrayItem(execInfo.scopesVar);
#else
  if (execInfo.baseScope) // if let scoping, the top of the scopes list may just be a scope. Use baseScope instead
    scope = jsvLockAgain(execInfo.baseScope);
#endif
  if (!jsvIsFunction(scope)) {
    jsExceptionHere(JSET_ERROR, "Can only use 'arguments' variable inside a function");
    return 0;
  }
  JsVar *result = jsvGetFunctionArgumentLength(scope);
  /* save 'arguments' as a variable into the scope. This stops us having
  to recreate the array each time, but also stops #1691 - where using an
  undefined argument after 'arguments' is freed leaves an unreferenced
  NAME with an undefined value, which causes a ReferenceError. */
  jsvObjectSetChild(scope,"arguments",result);
  jsvUnLock(scope);
  return result;
}



/*JSON{
  "type" : "constructor",
  "class" : "Function",
  "name" : "Function",
  "generate" : "jswrap_function_constructor",
  "params" : [
    ["args","JsVarArray","Zero or more arguments (as strings), followed by a string representing the code to run"]
  ],
  "return" : ["JsVar","A Number object"]
}
Creates a function
 */
JsVar *jswrap_function_constructor(JsVar *args) {
  JsVar *fn = jsvNewWithFlags(JSV_FUNCTION);
  if (!fn) return 0;

  /* Slightly odd form because we want to iterate
   * over all items, but leave the final one as
   * that will be for code. */
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, args);
  JsVar *v = jsvObjectIteratorGetValue(&it);
  jsvObjectIteratorNext(&it);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *s = jsvAsString(v);
    if (s) {
      // copy the string - if a string was supplied already we want to make
      // sure we have a new (unreferenced) string
      JsVar *paramName = jsvNewFromString("\xFF");
      if (paramName) {
        jsvAppendStringVarComplete(paramName, s);
        jsvAddFunctionParameter(fn, paramName, 0);
      }
      jsvUnLock(s);
    }

    jsvUnLock(v);
    v = jsvObjectIteratorGetValue(&it);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  jsvObjectSetChildAndUnLock(fn, JSPARSE_FUNCTION_CODE_NAME, v);
  return fn;
}

/*JSON{
  "type" : "function",
  "name" : "eval",
  "generate" : "jswrap_eval",
  "params" : [
    ["code","JsVar",""]
  ],
  "return" : ["JsVar","The result of evaluating the string"]
}
Evaluate a string containing JavaScript code
 */
JsVar *jswrap_eval(JsVar *v) {
  if (!v) return 0;
  JsVar *s = jsvAsString(v); // get as a string
  JsVar *result = jspEvaluateVar(s, execInfo.thisVar, 0);
  jsvUnLock(s);
  return result;
}

/*JSON{
  "type" : "function",
  "name" : "parseInt",
  "generate" : "jswrap_parseInt",
  "params" : [
    ["string","JsVar",""],
    ["radix","JsVar","The Radix of the string (optional)"]
  ],
  "return" : ["JsVar","The integer value of the string (or NaN)"]
}
Convert a string representing a number into an integer
 */
JsVar *jswrap_parseInt(JsVar *v, JsVar *radixVar) {
  int radix = 0;
  if (jsvIsNumeric(radixVar))
    radix = (int)jsvGetInteger(radixVar);

  if (jsvIsFloat(v) && !isfinite(jsvGetFloat(v)))
    return jsvNewFromFloat(NAN);

  // otherwise convert to string
  char buffer[JS_NUMBER_BUFFER_SIZE];
  char *bufferStart = buffer;
  jsvGetString(v, buffer, JS_NUMBER_BUFFER_SIZE);
  bool hasError = false;
  if (((!radix) || (radix==16)) &&
      buffer[0]=='0' && (buffer[1]=='x' || buffer[1]=='X')) { // special-case for '0x' for parseInt
    radix = 16;
    bufferStart += 2;
  }
  if (!radix) {
    radix = 10; // default to radix 10
  }
  const char *endOfInteger;
  long long i = stringToIntWithRadix(bufferStart, radix, &hasError, &endOfInteger);
  if (hasError) return jsvNewFromFloat(NAN);
  // If the integer went right to the end of our buffer then we
  // probably had to miss some stuff off the end of the string
  // in jsvGetString
  if (endOfInteger == &buffer[sizeof(buffer)-1]) {
    jsExceptionHere(JSET_ERROR, "String too big to convert to integer\n");
    return jsvNewFromFloat(NAN);
  }
  return jsvNewFromLongInteger(i);
}

/*JSON{
  "type" : "function",
  "name" : "parseFloat",
  "generate" : "jswrap_parseFloat",
  "params" : [
    ["string","JsVar",""]
  ],
  "return" : ["float","The value of the string"]
}
Convert a string representing a number into an float
 */
JsVarFloat jswrap_parseFloat(JsVar *v) {
  char buffer[JS_NUMBER_BUFFER_SIZE];
  jsvGetString(v, buffer, JS_NUMBER_BUFFER_SIZE);
  if (!strcmp(buffer, "Infinity")) return INFINITY;
  if (!strcmp(buffer, "-Infinity")) return -INFINITY;
  const char *endOfFloat;
  JsVarFloat f = stringToFloatWithRadix(buffer,0,&endOfFloat);
  // If the float went right to the end of our buffer then we
  // probably had to miss some stuff off the end of the string
  // in jsvGetString
  if (endOfFloat == &buffer[sizeof(buffer)-1]) {
    jsExceptionHere(JSET_ERROR, "String too big to convert to float\n");
    return NAN;
  }
  return f;
}

/*JSON{
  "type" : "function",
  "name" : "isFinite",
  "generate" : "jswrap_isFinite",
  "params" : [
    ["x","JsVar",""]
  ],
  "return" : ["bool","True is the value is a Finite number, false if not."]
}
Is the parameter a finite number or not? If needed, the parameter is first
converted to a number.
 */
bool jswrap_isFinite(JsVar *v) {
  JsVarFloat f = jsvGetFloat(v);
  return !isnan(f) && f!=INFINITY && f!=-INFINITY;
}

/*JSON{
  "type" : "function",
  "name" : "isNaN",
  "generate" : "jswrap_isNaN",
  "params" : [
    ["x","JsVar",""]
  ],
  "return" : ["bool","True is the value is NaN, false if not."]
}
Whether the x is NaN (Not a Number) or not
 */
bool jswrap_isNaN(JsVar *v) {
  if (jsvIsUndefined(v) ||
      jsvIsObject(v) ||
      ((jsvIsFloat(v)||jsvIsArray(v)) && isnan(jsvGetFloat(v)))) return true;
  if (jsvIsString(v)) {
    // this is where it can get a bit crazy
    bool allWhiteSpace = true;
    JsvStringIterator it;
    jsvStringIteratorNew(&it,v,0);
    while (jsvStringIteratorHasChar(&it)) {
      if (!isWhitespace(jsvStringIteratorGetCharAndNext(&it))) {
        allWhiteSpace = false;
        break;
      }
    }
    jsvStringIteratorFree(&it);
    if (allWhiteSpace) return false;
    return isnan(jsvGetFloat(v));
  }
  return false;
}


NO_INLINE static int jswrap_btoa_encode(int c) {
  c = c & 0x3F;
  if (c<26) return 'A'+c;
  if (c<52) return 'a'+c-26;
  if (c<62) return '0'+c-52;
  if (c==62) return '+';
  return '/'; // c==63
}

NO_INLINE static int jswrap_atob_decode(int c) {
  c = c & 0xFF;
  if (c>='A' && c<='Z') return c-'A';
  if (c>='a' && c<='z') return 26+c-'a';
  if (c>='0' && c<='9') return 52+c-'0';
  if (c=='+') return 62;
  if (c=='/') return 63;
  return -1; // not found
}

/*JSON{
  "type" : "function",
  "name" : "btoa",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_btoa",
  "params" : [
    ["binaryData","JsVar","A string of data to encode"]
  ],
  "return" : ["JsVar","A base64 encoded string"]
}
Encode the supplied string (or array) into a base64 string
 */
JsVar *jswrap_btoa(JsVar *binaryData) {
  if (!jsvIsIterable(binaryData)) {
    jsExceptionHere(JSET_ERROR, "Expecting a string or array, got %t", binaryData);
    return 0;
  }
  size_t inputLength = jsvGetLength(binaryData);
  size_t outputLength = ((inputLength+2)/3)*4;
  JsVar* base64Data = jsvNewStringOfLength((unsigned int)outputLength, NULL);
  if (!base64Data) return 0;
  JsvIterator itsrc;
  JsvStringIterator itdst;
  jsvIteratorNew(&itsrc, binaryData, JSIF_EVERY_ARRAY_ELEMENT);
  jsvStringIteratorNew(&itdst, base64Data, 0);


  int padding = 0;
  while (jsvIteratorHasElement(&itsrc) && !jspIsInterrupted()) {
    int octet_a = (unsigned char)jsvIteratorGetIntegerValue(&itsrc)&255;
    jsvIteratorNext(&itsrc);
    int octet_b = 0, octet_c = 0;
    if (jsvIteratorHasElement(&itsrc)) {
      octet_b = jsvIteratorGetIntegerValue(&itsrc)&255;
      jsvIteratorNext(&itsrc);
      if (jsvIteratorHasElement(&itsrc)) {
        octet_c = jsvIteratorGetIntegerValue(&itsrc)&255;
        jsvIteratorNext(&itsrc);
        padding = 0;
      } else
        padding = 1;
    } else
      padding = 2;

    int triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

    jsvStringIteratorSetCharAndNext(&itdst, (char)jswrap_btoa_encode(triple >> 18));
    jsvStringIteratorSetCharAndNext(&itdst, (char)jswrap_btoa_encode(triple >> 12));
    jsvStringIteratorSetCharAndNext(&itdst, (char)((padding>1)?'=':jswrap_btoa_encode(triple >> 6)));
    jsvStringIteratorSetCharAndNext(&itdst, (char)((padding>0)?'=':jswrap_btoa_encode(triple)));
  }

  jsvIteratorFree(&itsrc);
  jsvStringIteratorFree(&itdst);

  return base64Data;
}

/*JSON{
  "type" : "function",
  "name" : "atob",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_atob",
  "params" : [
    ["base64Data","JsVar","A string of base64 data to decode"]
  ],
  "return" : ["JsVar","A string containing the decoded data"]
}
Decode the supplied base64 string into a normal string
 */
JsVar *jswrap_atob(JsVar *base64Data) {
  if (!jsvIsString(base64Data)) {
    jsExceptionHere(JSET_ERROR, "Expecting a string, got %t", base64Data);
    return 0;
  }
  // work out input length (ignoring whitespace)
  size_t inputLength = 0;
  JsvStringIterator itsrc;
  jsvStringIteratorNew(&itsrc, base64Data, 0);
  char prevCh = 0, prevPrevCh = 0;
  while (jsvStringIteratorHasChar(&itsrc)) {
    char ch = jsvStringIteratorGetChar(&itsrc);
    if (!isWhitespace(ch)) {
      prevPrevCh = prevCh;
      prevCh = ch;
      inputLength++;
    }
    jsvStringIteratorNext(&itsrc);
  }
  jsvStringIteratorFree(&itsrc);
  // work out output length and allocate buffer
  size_t outputLength = inputLength*3/4;
  if (prevCh=='=') outputLength--;
  if (prevPrevCh=='=') outputLength--;
  JsVar* binaryData = jsvNewStringOfLength((unsigned int)outputLength, NULL);
  if (!binaryData) return 0;
  // decode...
  JsvStringIterator itdst;
  jsvStringIteratorNew(&itsrc, base64Data, 0);
  jsvStringIteratorNew(&itdst, binaryData, 0);
  while (jsvStringIteratorHasChar(&itsrc) && !jspIsInterrupted()) {
    uint32_t triple = 0;
    int i, valid=0;
    for (i=0;i<4;i++) {
      if (jsvStringIteratorHasChar(&itsrc)) {
        char ch = ' '; // get char, skip whitespace. If string ends, ch=0 and we break out
        while (ch && isWhitespace(ch)) ch=jsvStringIteratorGetCharAndNext(&itsrc);
        int sextet = jswrap_atob_decode(ch);
        if (sextet>=0) {
          triple |= (unsigned int)(sextet) << ((3-i)*6);
          valid=i;
        }
      }
    }

    if (valid>0) jsvStringIteratorSetCharAndNext(&itdst, (char)(triple >> 16));
    if (valid>1) jsvStringIteratorSetCharAndNext(&itdst, (char)(triple >> 8));
    if (valid>2) jsvStringIteratorSetCharAndNext(&itdst, (char)(triple));
  }

  jsvStringIteratorFree(&itsrc);
  jsvStringIteratorFree(&itdst);

  return binaryData;
}

/*JSON{
  "type" : "function",
  "name" : "encodeURIComponent",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_encodeURIComponent",
  "params" : [
    ["str","JsVar","A string to encode as a URI"]
  ],
  "return" : ["JsVar","A string containing the encoded data"]
}
Convert a string with any character not alphanumeric or `- _ . ! ~ * ' ( )`
converted to the form `%XY` where `XY` is its hexadecimal representation
 */
JsVar *jswrap_encodeURIComponent(JsVar *arg) {
  JsVar *v = jsvAsString(arg);
  if (!v) return 0;
  JsVar *result = jsvNewFromEmptyString();
  if (result) {
    JsvStringIterator it;
    jsvStringIteratorNew(&it, v, 0);
    JsvStringIterator dst;
    jsvStringIteratorNew(&dst, result, 0);
    while (jsvStringIteratorHasChar(&it)) {
      char ch = jsvStringIteratorGetCharAndNext(&it);
      if (isAlpha(ch) || isNumeric(ch) ||
          ch=='-' || // _ in isAlpha
          ch=='.' ||
          ch=='!' ||
          ch=='~' ||
          ch=='*' ||
          ch=='\'' ||
          ch=='(' ||
          ch==')') {
        jsvStringIteratorAppend(&dst, ch);
      } else {
        jsvStringIteratorAppend(&dst, '%');
        unsigned int d = ((unsigned)ch)>>4;
        jsvStringIteratorAppend(&dst, (char)((d>9)?('A'+d-10):('0'+d)));
        d = ((unsigned)ch)&15;
        jsvStringIteratorAppend(&dst, (char)((d>9)?('A'+d-10):('0'+d)));
      }
    }
    jsvStringIteratorFree(&dst);
    jsvStringIteratorFree(&it);
  }
  jsvUnLock(v);
  return result;
}

/*JSON{
  "type" : "function",
  "name" : "decodeURIComponent",
  "ifndef" : "SAVE_ON_FLASH",
  "generate" : "jswrap_decodeURIComponent",
  "params" : [
    ["str","JsVar","A string to decode from a URI"]
  ],
  "return" : ["JsVar","A string containing the decoded data"]
}
Convert any groups of characters of the form '%ZZ', into characters with hex
code '0xZZ'
 */
JsVar *jswrap_decodeURIComponent(JsVar *arg) {
  JsVar *v = jsvAsString(arg);
  if (!v) return 0;
  JsVar *result = jsvNewFromEmptyString();
  if (result) {
    JsvStringIterator it;
    jsvStringIteratorNew(&it, v, 0);
    JsvStringIterator dst;
    jsvStringIteratorNew(&dst, result, 0);
    while (jsvStringIteratorHasChar(&it)) {
      char ch = jsvStringIteratorGetCharAndNext(&it);
      if (ch>>7) {
        jsExceptionHere(JSET_ERROR, "ASCII only\n");
        break;
      }
      if (ch!='%') {
        jsvStringIteratorAppend(&dst, ch);
      } else {
        int hi = jsvStringIteratorGetCharAndNext(&it);
        int lo = jsvStringIteratorGetCharAndNext(&it);
        int v = (char)hexToByte(hi,lo);
        if (v<0) {
          jsExceptionHere(JSET_ERROR, "Invalid URI\n");
          break;
        }
        ch = (char)v;
        jsvStringIteratorAppend(&dst, ch);
      }
    }
    jsvStringIteratorFree(&dst);
    jsvStringIteratorFree(&it);
  }
  jsvUnLock(v);
  return result;
}
