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
 * Misc utils and cheapskate stdlib implementation
 * ----------------------------------------------------------------------------
 */
#include "jsutils.h"
#include "jslex.h"
#include "jshardware.h"
#include "jsinteractive.h"
#include "jswrapper.h"
#include "jswrap_error.h"
#include "jswrap_json.h"

/** Error flags for things that we don't really want to report on the console,
 * but which are good to know about */
volatile JsErrorFlags jsErrorFlags;


bool isWhitespace(char ch) {
    return isWhitespaceInline(ch);
}

bool isHexadecimal(char ch) {
    return ((ch>='0') && (ch<='9')) ||
           ((ch>='a') && (ch<='f')) ||
           ((ch>='A') && (ch<='F'));
}
bool isAlpha(char ch) {
    return isAlphaInline(ch);
}
bool isNumeric(char ch) {
    return isNumericInline(ch);
}


bool isIDString(const char *s) {
  if (!isAlpha(*s))
    return false;
  while (*s) {
    if (!(isAlpha(*s) || isNumeric(*s)))
      return false;
    s++;
  }
  return true;
}

char charToUpperCase(char ch) {
  return (char)(((ch>=97 && ch<=122) || (ch>=224 && ch<=246) || (ch>=248 && ch<=254)) ? ch - 32 : ch);
} // a-z, à-ö, ø-þ

char charToLowerCase(char ch) {
  return (char)(((ch>=65 && ch<=90) || (ch>=192 && ch<=214) || (ch>=216 && ch<=222))  ? ch + 32 : ch);
} // A-Z, À-Ö, Ø-Þ

/** escape a character - if it is required. This may return a reference to a static array,
so you can't store the value it returns in a variable and call it again.
If jsonStyle=true, only string escapes supported by JSON are used */
const char *escapeCharacter(char ch, char nextCh, bool jsonStyle) {
  if (ch=='\b') return "\\b"; // 8
  if (ch=='\t') return "\\t"; // 9
  if (ch=='\n') return "\\n"; // A
  if (ch=='\v' && !jsonStyle) return "\\v"; // B
  if (ch=='\f') return "\\f"; // C
  if (ch=='\r') return "\\r"; // D
  if (ch=='\\') return "\\\\";
  if (ch=='"') return "\\\"";
  static char buf[7];
  unsigned char uch = (unsigned char)ch;
  if (uch<8 && !jsonStyle && (nextCh<'0' || nextCh>'7')) {
    // encode less than 8 as \#
    buf[0]='\\';
    buf[1] = (char)('0'+uch);
    buf[2] = 0;
    return buf;
  } else if (uch<32 || uch>=127) {
    /** just encode as hex - it's more understandable
     * and doesn't have the issue of "\16"+"1" != "\161" */
    buf[0]='\\';
    int o=2;
    if (jsonStyle) {
      buf[1]='u';
      buf[o++] = '0';
      buf[o++] = '0';
    } else {
      buf[1]='x';
    }
    int n = (uch>>4)&15;
    buf[o++] = (char)((n<10)?('0'+n):('A'+n-10));
    n=uch&15;
    buf[o++] = (char)((n<10)?('0'+n):('A'+n-10));
    buf[o++] = 0;
    return buf;
  }
  buf[1] = 0;
  buf[0] = ch;
  return buf;
}

/** Parse radix prefixes, or return 0 */
NO_INLINE int getRadix(const char **s, bool *hasError) {
  int radix = 10;
  if (**s == '0') {
    radix = 8;
    (*s)++;
    // OctalIntegerLiteral: 0o01, 0O01
    if (**s == 'o' || **s == 'O') {
      radix = 8;
      (*s)++;
      // HexIntegerLiteral: 0x01, 0X01
    } else if (**s == 'x' || **s == 'X') {
      radix = 16;
      (*s)++;
      // BinaryIntegerLiteral: 0b01, 0B01
    } else if (**s == 'b' || **s == 'B') {
      radix = 2;
      (*s)++;
    } else {
      // check for '.' or digits 8 or 9 - if so it's decimal
      const char *p;
      for (p=*s;*p;p++)
        if (*p=='.' || *p=='8' || *p=='9')
           radix = 10;
        else if (*p<'0' || *p>'9') break;
    }
  }
  return radix;
}

// Convert a character to the hexadecimal equivalent (or -1)
int chtod(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  else if (ch >= 'a' && ch <= 'z')
    return 10 + ch - 'a';
  else if (ch >= 'A' && ch <= 'Z')
    return 10 + ch - 'A';
  else return -1;
}

/// Convert 2 characters to the hexadecimal equivalent (or -1)
int hexToByte(char hi, char lo) {
  int a = chtod(hi);
  int b = chtod(lo);
  if (a<0 || b<0) return -1;
  return (a<<4)|b;
}

/* convert a number in the given radix to an int */
long long stringToIntWithRadix(const char *s,
               int forceRadix, //!< if radix=0, autodetect. if radix
               bool *hasError, //!< If nonzero, set to whether there was an error or not
               const char **endOfInteger //!<  If nonzero, this is set to the point at which the integer finished in the string
               ) {
  // skip whitespace (strange parseInt behaviour)
  while (isWhitespace(*s)) s++;

  bool isNegated = false;
  long long v = 0;
  if (*s == '-') {
    isNegated = true;
    s++;
  } else if (*s == '+') {
    s++;
  }

  const char *numberStart = s;
  if (endOfInteger) (*endOfInteger)=s;


  int radix = forceRadix ? forceRadix : getRadix(&s, hasError);
  if (!radix) return 0;

  while (*s) {
    int digit = chtod(*s);
    if (digit<0 || digit>=radix)
      break;
    v = v*radix + digit;
    s++;
  }

  if (hasError)
    *hasError = s==numberStart; // we had an error if we didn't manage to parse any chars at all
  if (endOfInteger) (*endOfInteger)=s;

  if (isNegated) return -v;
  return v;
}

/**
 * Convert hex, binary, octal or decimal string into an int.
 */
long long stringToInt(const char *s) {
  return stringToIntWithRadix(s,0,NULL,NULL);
}

#ifndef USE_FLASH_MEMORY

// JsError, jsWarn, jsExceptionHere implementations that expect the format string to be in normal
// RAM where is can be accessed normally.

NO_INLINE void jsError(const char *fmt, ...) {
  jsiConsoleRemoveInputLine();
  jsiConsolePrint("ERROR: ");
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrintString,0, fmt, argp);
  va_end(argp);
  jsiConsolePrint("\n");
}

NO_INLINE void jsWarn(const char *fmt, ...) {
  jsiConsoleRemoveInputLine();
  jsiConsolePrint("WARNING: ");
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrintString,0, fmt, argp);
  va_end(argp);
  jsiConsolePrint("\n");
}

NO_INLINE void jsExceptionHere(JsExceptionType type, const char *fmt, ...) {
  // If we already had an exception, forget this
  if (jspHasError()) return;

  jsiConsoleRemoveInputLine();

  JsVar *var = jsvNewFromEmptyString();
  if (!var) {
    jspSetError(false);
    return; // out of memory
  }

  JsvStringIterator it;
  jsvStringIteratorNew(&it, var, 0);
  jsvStringIteratorGotoEnd(&it);

  vcbprintf_callback cb = (vcbprintf_callback)jsvStringIteratorPrintfCallback;

  va_list argp;
  va_start(argp, fmt);
  vcbprintf(cb,&it, fmt, argp);
  va_end(argp);

  jsvStringIteratorFree(&it);

  if (type != JSET_STRING) {
    JsVar *obj = 0;
    if (type == JSET_ERROR) obj = jswrap_error_constructor(var);
    else if (type == JSET_SYNTAXERROR) obj = jswrap_syntaxerror_constructor(var);
    else if (type == JSET_TYPEERROR) obj = jswrap_typeerror_constructor(var);
    else if (type == JSET_INTERNALERROR) obj = jswrap_internalerror_constructor(var);
    else if (type == JSET_REFERENCEERROR) obj = jswrap_referenceerror_constructor(var);
    jsvUnLock(var);
    var = obj;
  }

  jspSetException(var);
  jsvUnLock(var);
}

#else

// JsError, jsWarn, jsExceptionHere implementations that expect the format string to be in FLASH
// and first copy it into RAM in order to prevent issues with byte access, this is necessary on
// platforms, like the esp8266, where data flash can only be accessed using word-aligned reads.

NO_INLINE void jsError_flash(const char *fmt, ...) {
  size_t len = flash_strlen(fmt);
  char buff[len+1];
  flash_strncpy(buff, fmt, len+1);

  jsiConsoleRemoveInputLine();
  jsiConsolePrint("ERROR: ");
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrintString,0, buff, argp);
  va_end(argp);
  jsiConsolePrint("\n");
}

NO_INLINE void jsWarn_flash(const char *fmt, ...) {
  size_t len = flash_strlen(fmt);
  char buff[len+1];
  flash_strncpy(buff, fmt, len+1);

  jsiConsoleRemoveInputLine();
  jsiConsolePrint("WARNING: ");
  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsiConsolePrintString,0, buff, argp);
  va_end(argp);
  jsiConsolePrint("\n");
}

NO_INLINE void jsExceptionHere_flash(JsExceptionType type, const char *ffmt, ...) {
  size_t len = flash_strlen(ffmt);
  char fmt[len+1];
  flash_strncpy(fmt, ffmt, len+1);

  // If we already had an exception, forget this
  if (jspHasError()) return;

  jsiConsoleRemoveInputLine();

  JsVar *var = jsvNewFromEmptyString();
  if (!var) {
    jspSetError(false);
    return; // out of memory
  }

  JsvStringIterator it;
  jsvStringIteratorNew(&it, var, 0);
  jsvStringIteratorGotoEnd(&it);

  vcbprintf_callback cb = (vcbprintf_callback)jsvStringIteratorPrintfCallback;

  va_list argp;
  va_start(argp, ffmt);
  vcbprintf(cb,&it, fmt, argp);
  va_end(argp);

  jsvStringIteratorFree(&it);

  if (type != JSET_STRING) {
    JsVar *obj = 0;
    if (type == JSET_ERROR) obj = jswrap_error_constructor(var);
    else if (type == JSET_SYNTAXERROR) obj = jswrap_syntaxerror_constructor(var);
    else if (type == JSET_TYPEERROR) obj = jswrap_typeerror_constructor(var);
    else if (type == JSET_INTERNALERROR) obj = jswrap_internalerror_constructor(var);
    else if (type == JSET_REFERENCEERROR) obj = jswrap_referenceerror_constructor(var);
    jsvUnLock(var);
    var = obj;
  }

  jspSetException(var);
  jsvUnLock(var);
}

#endif

NO_INLINE void jsAssertFail(const char *file, int line, const char *expr) {
  static bool inAssertFail = false;
  bool wasInAssertFail = inAssertFail;
  inAssertFail = true;
  jsiConsoleRemoveInputLine();
  if (expr) {
#ifndef USE_FLASH_MEMORY
    jsiConsolePrintf("ASSERT(%s) FAILED AT ", expr);
#else
    jsiConsolePrintString("ASSERT(");
    // string is in flash and requires word access, thus copy it onto the stack
    size_t len = flash_strlen(expr);
    char buff[len+1];
    flash_strncpy(buff, expr, len+1);
    jsiConsolePrintString(buff);
    jsiConsolePrintString(") FAILED AT ");
#endif
  } else {
    jsiConsolePrint("ASSERT FAILED AT ");
  }
  jsiConsolePrintf("%s:%d\n",file,line);
  if (!wasInAssertFail) {
    jsvTrace(jsvFindOrCreateRoot(), 2);
  }
#if defined(ARM)
  jsiConsolePrint("REBOOTING.\n");
  jshTransmitFlush();
  NVIC_SystemReset();
#elif defined(ESP8266)
  // typically the Espruino console is over telnet, in which case nothing we do here will ever
  // show up, so we instead jump through some hoops to print to UART
  int os_printf_plus(const char *format, ...)  __attribute__((format(printf, 1, 2)));
  os_printf_plus("ASSERT FAILED AT %s:%d\n", file,line);
  jsiConsolePrint("---console end---\n");
  int c, console = jsiGetConsoleDevice();
  while ((c=jshGetCharToTransmit(console)) >= 0)
    os_printf_plus("%c", c);
  os_printf_plus("CRASHING.\n");
  *(int*)0xdead = 0xbeef;
  extern void jswrap_ESP8266_reboot(void);
  jswrap_ESP8266_reboot();
  while(1) ;
#elif defined(LINUX)
  jsiConsolePrint("EXITING.\n");
  exit(1);
#else
  jsiConsolePrint("HALTING.\n");
  while (1);
#endif
  inAssertFail = false;
}

#ifdef USE_FLASH_MEMORY
// Helpers to deal with constant strings stored in flash that have to be accessed using word-aligned
// and word-sized reads

// Get the length of a string in flash
size_t flash_strlen(const char *str) {
  size_t len = 0;
  uint32_t *s = (uint32_t *)str;

  while (1) {
    uint32_t w = *s++;
    if ((w & 0xff) == 0) break;
    len++; w >>= 8;
    if ((w & 0xff) == 0) break;
    len++; w >>= 8;
    if ((w & 0xff) == 0) break;
    len++; w >>= 8;
    if ((w & 0xff) == 0) break;
    len++;
  }
  return len;
}

// Copy a string from flash
char *flash_strncpy(char *dst, const char *src, size_t c) {
  char *d = dst;
  uint32_t *s = (uint32_t *)src;
  size_t slen = flash_strlen(src);
  size_t len = slen > c ? c : slen;

  // copy full words from source string
  while (len >= 4) {
    uint32_t w = *s++;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff;
    len -= 4;
  }
  // copy any remaining bytes
  if (len > 0) {
    uint32_t w = *s++;
    while (len-- > 0) {
      *d++ = w & 0xff; w >>= 8;
    }
  }
  // terminating null
  if (slen < c) *d = 0;
  return dst;
}

// Compare a string in memory with a string in flash
int flash_strcmp(const char *mem, const char *flash) {
  while (1) {
    char m = *mem++;
    char c = READ_FLASH_UINT8(flash++);
    if (m == 0) return c != 0 ? -1 : 0;
    if (c == 0) return 1;
    if (c > m) return -1;
    if (m > c) return 1;
  }
}

// memcopy a string from flash
unsigned char *flash_memcpy(unsigned char *dst, const unsigned char *src, size_t c) {
  unsigned char *d = dst;
  uint32_t *s = (uint32_t *)src;
  size_t len = c;

  // copy full words from source string
  while (len >= 4) {
    uint32_t w = *s++;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff; w >>= 8;
    *d++ = w & 0xff;
    len -= 4;
  }
  // copy any remaining bytes
  if (len > 0) {
    uint32_t w = *s++;
    while (len-- > 0) {
      *d++ = w & 0xff; w >>= 8;
    }
  }
  return dst;
}

#endif


/** Convert a string to a JS float variable where the string is of a specific radix. */
JsVarFloat stringToFloatWithRadix(
    const char *s, //!< The string to be converted to a float
  	int forceRadix, //!< The radix of the string data, or 0 to guess
  	const char **endOfFloat //!<  If nonzero, this is set to the point at which the float finished in the string
  ) {
  // skip whitespace (strange parseFloat behaviour)
  while (isWhitespace(*s)) s++;

  bool isNegated = false;
  if (*s == '-') {
    isNegated = true;
    s++;
  } else if (*s == '+') {
    s++;
  }

  const char *numberStart = s;
  if (endOfFloat) (*endOfFloat)=s;

  int radix = forceRadix ? forceRadix : getRadix(&s, 0);
  if (!radix) return NAN;


  JsVarFloat v = 0;
  JsVarFloat mul = 0.1;

  // handle integer part
  while (*s) {
    int digit = chtod(*s);
    if (digit<0 || digit>=radix)
      break;
    v = (v*radix) + digit;
    s++;
  }

  if (radix == 10) {
    // handle decimal point
    if (*s == '.') {
      s++; // skip .

      while (*s) {
        if (*s >= '0' && *s <= '9')
          v += mul*(*s - '0');
        else break;
        mul /= 10;
        s++;
      }
    }

    // handle exponentials
    if (*s == 'e' || *s == 'E') {
      s++;  // skip E
      bool isENegated = false;
      if (*s == '-' || *s == '+') {
        isENegated = *s=='-';
        s++;
      }
      int e = 0;
      while (*s) {
        if (*s >= '0' && *s <= '9')
          e = (e*10) + (*s - '0');
        else break;
        s++;
      }
      if (isENegated) e=-e;
      // TODO: faster INTEGER pow? Normal pow has floating point inaccuracies
      while (e>0) {
        v*=10;
        e--;
      }
      while (e<0) {
        v/=10;
        e++;
      }
    }
  }

  if (endOfFloat) (*endOfFloat)=s;
  // check that we managed to parse something at least
  if (numberStart==s || // nothing
      (numberStart[0]=='.' && s==&numberStart[1]) // just a '.'
      ) return NAN;

  if (isNegated) return -v;
  return v;
}


/** convert a string to a floating point JS variable. */
JsVarFloat stringToFloat(const char *s) {
  return stringToFloatWithRadix(s, 0, NULL); // don't force the radix to anything in particular
}


char itoch(int val) {
  if (val<10) return (char)('0'+val);
  return (char)('a'+val-10);
}

void itostr_extra(JsVarInt vals,char *str,bool signedVal, unsigned int base) {
  JsVarIntUnsigned val;
  // handle negative numbers
  if (signedVal && vals<0) {
    *(str++)='-';
    val = (JsVarIntUnsigned)(-vals);
  } else {
    val = (JsVarIntUnsigned)vals;
  }
  // work out how many digits
  JsVarIntUnsigned tmp = val;
  int digits = 1;
  while (tmp>=base) {
    digits++;
    tmp /= base;
  }
  // for each digit...
  int i;
  for (i=digits-1;i>=0;i--) {
    str[i] = itoch((int)(val % base));
    val /= base;
  }
  str[digits] = 0;
}

void ftoa_bounded_extra(JsVarFloat val,char *str, size_t len, int radix, int fractionalDigits) {
  assert(len>9); // in case if strcpy
  const JsVarFloat stopAtError = 0.0000001;
  if (isnan(val)) strcpy(str,"NaN");
  else if (!isfinite(val)) {
    if (val<0) strcpy(str,"-Infinity");
    else strcpy(str,"Infinity");
  } else {
    if (val<0) {
      if (--len <= 0) { *str=0; return; } // bounds check
      *(str++) = '-';
      val = -val;
    }

#ifndef USE_NO_FLOATS
    // check for exponents - if fractionalDigits we're using 'toFixed' so don't want exponentiation
    int exponent = 0;
    if (radix == 10 && val>0.0 && fractionalDigits<0) {
      // use repeated mul/div for ease, but to
      // improve accuracy we multiply by 1e5 first
      if (val >= 1E21) {
        while (val>100000) {
          val /= 100000;
          exponent += 5;
        }
        while (val>10) {
          val /= 10;
          exponent ++;
        }
      } else if (val < 1E-6) {
        while (val<1E-5) {
          val *= 100000;
          exponent -= 5;
        }
        while (val<1) {
          val *= 10;
          exponent --;
        }
      }
    }
 #endif


    // what if we're really close to an integer? Just use that...
    if (((JsVarInt)(val+stopAtError)) == (1+(JsVarInt)val))
      val = (JsVarFloat)(1+(JsVarInt)val);

    JsVarFloat d = 1;
    while (d*radix <= val) d*=radix;
    while (d >= 1) {
      int v = (int)(val / d);
      val -= v*d;
      if (--len <= 0) { *str=0; return; } // bounds check
      *(str++) = itoch(v);
      d /= radix;
    }
#ifndef USE_NO_FLOATS
    if (((fractionalDigits<0) && val>0) || fractionalDigits>0) {
      bool hasPt = false;
      val*=radix;
      while (((fractionalDigits<0) && (fractionalDigits>-12) && (val > stopAtError)) || (fractionalDigits > 0)) {
        int v = (int)(val+((fractionalDigits==1) ? 0.5 : 0.00000001) );
        val = (val-v)*radix;
	if (v==radix) v=radix-1;
        if (!hasPt) {	
	  hasPt = true;
          if (--len <= 0) { *str=0; return; } // bounds check
          *(str++)='.';
        }
        if (--len <= 0) { *str=0; return; } // bounds check
        *(str++)=itoch(v);
        fractionalDigits--;
      }
    }
    // write exponent if enough buffer length left (> 5)
    if (exponent && len > 5) {
      *str++ = 'e';
      if (exponent>0) *str++ = '+';
      itostr(exponent, str, 10);
      return;
    }
#endif

    *(str++)=0;
  }
}

void ftoa_bounded(JsVarFloat val,char *str, size_t len) {
  ftoa_bounded_extra(val, str, len, 10, -1);
}


/// Wrap a value so it is always between 0 and size (eg. wrapAround(angle, 360))
JsVarFloat wrapAround(JsVarFloat val, JsVarFloat size) {
  if (size<0.0) return 0.0;
  val = val / size;
  val = val - (int)val;
  return val * size;
}

/**
 * Espruino-special printf with a callback.
 *
 * The supported format specifiers are:
 * * `%d` = int
 * * `%0#d` or `%0#x` = int padded to length # with 0s
 * * `%x` = int as hex
 * * `%L` = JsVarInt
 * * `%Lx`= JsVarInt as hex
 * * `%f` = JsVarFloat
 * * `%s` = string (char *)
 * * `%c` = char
 * * `%v` = JsVar * (doesn't have to be a string - it'll be converted)
 * * `%q` = JsVar * (in quotes, and escaped)
 * * `%Q` = JsVar * (in quotes, and escaped the JSON subset of escape chars)
 * * `%j` = Variable printed as JSON
 * * `%t` = Type of variable
 * * `%p` = Pin
 *
 * Anything else will assert
 */
void vcbprintf(
    vcbprintf_callback user_callback, //!< Unknown
    void *user_data,                  //!< Unknown
    const char *fmt,                  //!< The format specified
    va_list argp                      //!< List of parameter values
  ) {
  char buf[32];
  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      char fmtChar = *fmt++;
      switch (fmtChar) {
      case ' ':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      {
        const char *pad = " ";
        if (!*fmt) break;
        if (fmtChar=='0') {
          pad = "0";
          fmtChar = *fmt++;
          if (!*fmt) break;
        }
        int digits = fmtChar - '0';
         // of the form '%02d'
        int v = va_arg(argp, int);
        if (*fmt=='x') itostr_extra(v, buf, false, 16);
        else { assert('d' == *fmt); itostr(v, buf, 10); }
        fmt++; // skip over 'd'
        int len = (int)strlen(buf);
        while (len < digits) {
          user_callback(pad,user_data);
          len++;
        }
        user_callback(buf,user_data);
        break;
      }
      case 'd': itostr(va_arg(argp, int), buf, 10); user_callback(buf,user_data); break;
      case 'x': itostr_extra(va_arg(argp, int), buf, false, 16); user_callback(buf,user_data); break;
      case 'L': {
        unsigned int rad = 10;
        bool signedVal = true;
        if (*fmt=='x') { rad=16; fmt++; signedVal = false; }
        itostr_extra(va_arg(argp, JsVarInt), buf, signedVal, rad); user_callback(buf,user_data);
      } break;
      case 'f': ftoa_bounded(va_arg(argp, JsVarFloat), buf, sizeof(buf)); user_callback(buf,user_data);  break;
      case 's': user_callback(va_arg(argp, char *), user_data); break;
      case 'c': buf[0]=(char)va_arg(argp, int/*char*/);buf[1]=0; user_callback(buf, user_data); break;
      case 'q':
      case 'Q':
      case 'v': {
        bool quoted = fmtChar!='v';
        bool isJSONStyle = fmtChar=='Q';
        if (quoted) user_callback("\"",user_data);
        JsVar *v = jsvAsString(va_arg(argp, JsVar*));
        buf[1] = 0;
        if (jsvIsString(v)) {
          JsvStringIterator it;
          jsvStringIteratorNew(&it, v, 0);
          // OPT: this could be faster than it is (sending whole blocks at once)
          while (jsvStringIteratorHasChar(&it)) {
            buf[0] = jsvStringIteratorGetCharAndNext(&it);
            char nextCh = jsvStringIteratorGetChar(&it);
            if (quoted) {
              user_callback(escapeCharacter(buf[0], nextCh, isJSONStyle), user_data);
            } else {
              user_callback(buf,user_data);
            }
          }
          jsvStringIteratorFree(&it);
          jsvUnLock(v);
        }
        if (quoted) user_callback("\"",user_data);
      } break;
      case 'j': {
        JsVar *v = va_arg(argp, JsVar*);
        jsfGetJSONWithCallback(v, NULL, JSON_SOME_NEWLINES | JSON_PRETTY | JSON_SHOW_DEVICES | JSON_ALLOW_TOJSON, 0, user_callback, user_data);
        break;
      }
      case 't': {
        JsVar *v = va_arg(argp, JsVar*);
        const char *n = jsvIsNull(v)?"null":jswGetBasicObjectName(v);
        if (!n) n = jsvGetTypeOf(v);
        user_callback(n, user_data);
        break;
      }
      case 'p': jshGetPinString(buf, (Pin)va_arg(argp, int/*Pin*/)); user_callback(buf, user_data); break;
      default: assert(0); return; // eep
      }
    } else {
      buf[0] = *(fmt++);
      buf[1] = 0;
      user_callback(&buf[0], user_data);
    }
  }
}


void cbprintf(vcbprintf_callback user_callback, void *user_data, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  vcbprintf(user_callback,user_data, fmt, argp);
  va_end(argp);
}

typedef struct {
  char *outPtr;
  size_t idx;
  size_t len;
} espruino_snprintf_data;

void espruino_snprintf_cb(const char *str, void *userdata) {
  espruino_snprintf_data *d = (espruino_snprintf_data*)userdata;

  while (*str) {
    if (d->idx < d->len) d->outPtr[d->idx] = *str;
    d->idx++;
    str++;
  }
}

/// a snprintf replacement so mbedtls doesn't try and pull in the whole stdlib to cat two strings together
int espruino_snprintf_va( char * s, size_t n, const char * fmt, va_list argp ) {
  espruino_snprintf_data d;
  d.outPtr = s;
  d.idx = 0;
  d.len = n;

  vcbprintf(espruino_snprintf_cb,&d, fmt, argp);

  if (d.idx < d.len) d.outPtr[d.idx] = 0;
  else d.outPtr[d.len-1] = 0;

  return (int)d.idx;
}

/// a snprintf replacement so mbedtls doesn't try and pull in the whole stdlib to cat two strings together
int espruino_snprintf( char * s, size_t n, const char * fmt, ... ) {
  va_list argp;
  va_start(argp, fmt);
  int l = espruino_snprintf_va(s,n,fmt,argp);
  va_end(argp);
  return l;
}



#ifdef ARM
extern uint32_t LINKER_END_VAR; // should be 'void', but 'int' avoids warnings
#endif

/** get the amount of free stack we have, in bytes */
size_t jsuGetFreeStack() {
#ifdef ARM
  void *frame = __builtin_frame_address(0);
  size_t stackPos = (size_t)((char*)frame);
  size_t stackEnd = (size_t)((char*)&LINKER_END_VAR);
  if (stackPos < stackEnd) return 0; // should never happen, but just in case of overflow!
  return  stackPos - stackEnd;
#elif defined(LINUX)
  // On linux, we set STACK_BASE from `main`.
  char ptr; // this is on the stack
  extern void *STACK_BASE;
  uint32_t count =  (uint32_t)((size_t)STACK_BASE - (size_t)&ptr);
  const uint32_t max_stack = 1000000; // give it 1 megabyte of stack
  if (count>max_stack) return 0;
  return max_stack - count;
#elif defined(ESP32)
  char ptr; // this is on the stack

  //RTOS task stacks work the opposite way to what you may expect.
  //Early entries are in higher memory locations.
  //Later entries are in lower memory locations.

  
  uint32_t stackPos   = (uint32_t)&ptr;
  uint32_t stackStart = (uint32_t)espruino_stackHighPtr - ESP_STACK_SIZE;

  if (stackPos < stackStart) return 0; // should never happen, but just in case of overflow!
  
  return stackPos - stackStart;
#else
  // stack depth seems pretty platform-specific :( Default to a value that disables it
  return 1000000; // no stack depth check on this platform
#endif
}

unsigned int rand_m_w = 0xDEADBEEF;    /* must not be zero */
unsigned int rand_m_z = 0xCAFEBABE;    /* must not be zero */

int rand() {
  rand_m_z = 36969 * (rand_m_z & 65535) + (rand_m_z >> 16);
  rand_m_w = 18000 * (rand_m_w & 65535) + (rand_m_w >> 16);
  return (int)RAND_MAX & (int)((rand_m_z << 16) + rand_m_w);  /* 32-bit result */
}

void srand(unsigned int seed) {
  rand_m_w = (seed&0xFFFF) | (seed<<16);
  rand_m_z = (seed&0xFFFF0000) | (seed>>16);
}

/// Clip X between -128 and 127
char clipi8(int x) {
  if (x<-128) return -128;
  if (x>127) return 127;
  return (char)x;
}

/// Convert the given value to a signed integer assuming it has the given number of bits
int twosComplement(int val, unsigned char bits) {
  if (val & ((unsigned int)1 << (bits - 1)))
    val -= (unsigned int)1 << bits;
  return val;
}

// quick integer square root
// https://stackoverflow.com/questions/31117497/fastest-integer-square-root-in-the-least-amount-of-instructions
unsigned short int int_sqrt32(unsigned int x) {
  unsigned short int res=0;
  unsigned short int add= 0x8000;
  int i;
  for(i=0;i<16;i++) {
    unsigned short int temp=res | add;
    unsigned int g2=temp*temp;
    if (x>=g2)
      res=temp;
    add>>=1;
  }
  return res;
}
