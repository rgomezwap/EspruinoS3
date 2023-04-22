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
 * Variables
 * ----------------------------------------------------------------------------
 */
#include "jsvar.h"
#include "jslex.h"
#include "jsparse.h"
#include "jswrap_json.h"
#include "jsinteractive.h"
#include "jswrapper.h"
#include "jswrap_math.h" // for jswrap_math_mod
#include "jswrap_object.h" // for jswrap_object_toString
#include "jswrap_arraybuffer.h" // for jsvNewTypedArray
#include "jswrap_dataview.h" // for jsvNewDataViewWithData
#if defined(ESPR_JIT) && defined(LINUX)
#include <sys/mman.h>
#endif

#ifdef DEBUG
  /** When freeing, clear the references (nextChild/etc) in the JsVar.
   * This means we can assert at the end of jsvFreePtr to make sure
   * everything really is free. */
  #define CLEAR_MEMORY_ON_FREE
#endif

/** Basically, JsVars are stored in one big array, so save the need for
 * lots of memory allocation. On Linux, the arrays are in blocks, so that
 * more blocks can be allocated. We can't use realloc on one big block as
 * this may change the address of vars that are already locked!
 *
 */

#ifdef RESIZABLE_JSVARS
JsVar **jsVarBlocks = 0;
unsigned int jsVarsSize = 0;
#define JSVAR_BLOCK_SIZE 4096
#define JSVAR_BLOCK_SHIFT 12
#else
#ifdef JSVAR_MALLOC
unsigned int jsVarsSize = 0;
JsVar *jsVars = NULL;
#else
JsVar jsVars[JSVAR_CACHE_SIZE] __attribute__((aligned(4)));
const unsigned int jsVarsSize = JSVAR_CACHE_SIZE;
#endif
#endif

typedef enum {
  MEM_NOT_BUSY,
  MEMBUSY_SYSTEM,
  MEMBUSY_GC
} MemBusyType;

volatile bool touchedFreeList = false;
volatile JsVarRef jsVarFirstEmpty; ///< reference of first unused variable (variables are in a linked list)
volatile MemBusyType isMemoryBusy; ///< Are we doing garbage collection or similar, so can't access memory?

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

JsVarRef jsvGetFirstChild(const JsVar *v) { return v->varData.ref.firstChild; }
JsVarRefSigned jsvGetFirstChildSigned(const JsVar *v) {
  if (v->varData.ref.firstChild > JSVARREF_MAX)
    return ((JsVarRefSigned)v->varData.ref.firstChild) + JSVARREF_MIN*2;
  return (JsVarRefSigned)v->varData.ref.firstChild;
}
JsVarRef jsvGetLastChild(const JsVar *v) { return v->varData.ref.lastChild; }
JsVarRef jsvGetNextSibling(const JsVar *v) { return v->varData.ref.nextSibling; }
JsVarRef jsvGetPrevSibling(const JsVar *v) { return v->varData.ref.prevSibling; }
void jsvSetFirstChild(JsVar *v, JsVarRef r) { v->varData.ref.firstChild = r; }
void jsvSetLastChild(JsVar *v, JsVarRef r) { v->varData.ref.lastChild = r; }
void jsvSetNextSibling(JsVar *v, JsVarRef r) { v->varData.ref.nextSibling = r; }
void jsvSetPrevSibling(JsVar *v, JsVarRef r) { v->varData.ref.prevSibling = r; }

JsVarRefCounter jsvGetRefs(JsVar *v) { return v->varData.ref.refs; }
void jsvSetRefs(JsVar *v, JsVarRefCounter refs) { v->varData.ref.refs = refs; }
unsigned char jsvGetLocks(JsVar *v) { return (unsigned char)((v->flags>>JSV_LOCK_SHIFT) & JSV_LOCK_MAX); }

// These defines take 'f' as 'v->flags&JSV_VARTYPEMASK
#define JSV_IS_ROOT(f) (f)==JSV_ROOT
#define JSV_IS_NULL(f) (f)==JSV_NULL
#define JSV_IS_PIN(f) (f)==JSV_PIN
#define JSV_IS_BOOL(f) ((f)==JSV_BOOLEAN || (f)==JSV_NAME_INT_BOOL)
#define JSV_IS_INT(f) ((f)==JSV_INTEGER || (f)==JSV_PIN || (f)==JSV_NAME_INT || (f)==JSV_NAME_INT_INT || (f)==JSV_NAME_INT_BOOL)
#define JSV_IS_NUMERIC(f) ((f)>=_JSV_NUMERIC_START && (f)<=_JSV_NUMERIC_END)
#define JSV_IS_STRING(f) ((f)>=_JSV_STRING_START && (f)<=_JSV_STRING_END)
#define JSV_IS_STRING_EXT(f) ((f)>=JSV_STRING_EXT_0 && (f)<=JSV_STRING_EXT_MAX)
#define JSV_IS_FLAT_STRING(f) (f)==JSV_FLAT_STRING
#define JSV_IS_NATIVE_STRING(f) (f)==JSV_NATIVE_STRING
#define JSV_IS_ARRAY(f) (f)==JSV_ARRAY
#define JSV_IS_OBJECT(f) ((f)==JSV_OBJECT || (f)==JSV_ROOT)
#define JSV_IS_FUNCTION(f) ((f)==JSV_FUNCTION || (f)==JSV_FUNCTION_RETURN || (f)==JSV_NATIVE_FUNCTION)
#define JSV_IS_ARRAYBUFFER(f) (f)==JSV_ARRAYBUFFER
#define JSV_IS_NAME(f) ((f)>=_JSV_NAME_START && (f)<=_JSV_NAME_END)
#define JSV_IS_NAME_WITH_VALUE(f) ((f)>=_JSV_NAME_WITH_VALUE_START && (f)<=_JSV_NAME_WITH_VALUE_END)
#ifdef ESPR_NO_GET_SET
#define JSV_IS_GETTER_OR_SETTER(f) false
#else
#define JSV_IS_GETTER_OR_SETTER(f) (f)==JSV_GET_SET
#endif
#ifdef SPIFLASH_BASE
#define JSV_IS_FLASH_STRING(f) (f)==JSV_FLASH_STRING
#else
#define JSV_IS_FLASH_STRING(f) false
#endif
#define JSV_IS_NONAPPENDABLE_STRING(f) (JSV_IS_FLAT_STRING(f) || JSV_IS_NATIVE_STRING(f) || JSV_IS_FLASH_STRING(f))

bool jsvIsRoot(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_ROOT(f); }
bool jsvIsPin(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_PIN(f); }
bool jsvIsSimpleInt(const JsVar *v) { return v && (v->flags&JSV_VARTYPEMASK)==JSV_INTEGER; } // is just a very basic integer value
bool jsvIsInt(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_INT(f); }
bool jsvIsFloat(const JsVar *v) { return v && (v->flags&JSV_VARTYPEMASK)==JSV_FLOAT; }
bool jsvIsBoolean(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_BOOL(f); }
bool jsvIsString(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_STRING(f); } ///< String, or a NAME too
bool jsvIsBasicString(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return f>=JSV_STRING_0 && f<=JSV_STRING_MAX; } ///< Just a string (NOT a name/flatstr/nativestr or flashstr)
bool jsvIsStringExt(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_STRING_EXT(f); } ///< The extra bits dumped onto the end of a string to store more data
bool jsvIsFlatString(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_FLAT_STRING(f); }
bool jsvIsNativeString(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_NATIVE_STRING(f); }
bool jsvIsFlashString(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_FLASH_STRING(f); }
bool jsvIsConstant(const JsVar *v) { return v && (v->flags&JSV_CONSTANT)==JSV_CONSTANT; }
bool jsvIsNumeric(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return f>=_JSV_NUMERIC_START && f<=_JSV_NUMERIC_END; }
bool jsvIsFunction(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return JSV_IS_FUNCTION(f); }
bool jsvIsFunctionReturn(const JsVar *v) { return v && ((v->flags&JSV_VARTYPEMASK)==JSV_FUNCTION_RETURN); } ///< Is this a function with an implicit 'return' at the start?
bool jsvIsFunctionParameter(const JsVar *v) { return v && (v->flags&JSV_NATIVE) && jsvIsString(v); }
bool jsvIsObject(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_OBJECT(f); }
bool jsvIsArray(const JsVar *v) { return v && JSV_IS_ARRAY(v->flags&JSV_VARTYPEMASK); }
bool jsvIsArrayBuffer(const JsVar *v) { return v && JSV_IS_ARRAYBUFFER(v->flags&JSV_VARTYPEMASK); }
bool jsvIsArrayBufferName(const JsVar *v) { return v && (v->flags&(JSV_VARTYPEMASK))==JSV_ARRAYBUFFERNAME; }
bool jsvIsNativeFunction(const JsVar *v) { return v && (v->flags&(JSV_VARTYPEMASK))==JSV_NATIVE_FUNCTION; }
bool jsvIsUndefined(const JsVar *v) { return v==0; }
bool jsvIsNull(const JsVar *v) { return v && JSV_IS_NULL(v->flags&JSV_VARTYPEMASK); }
bool jsvIsNullish(const JsVar *v) { return !v || JSV_IS_NULL(v->flags&JSV_VARTYPEMASK); } // jsvIsUndefined(v) || jsvIsNull(v);
bool jsvIsBasic(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return JSV_IS_NUMERIC(f) || JSV_IS_STRING(f); } ///< Is this *not* an array/object/etc
bool jsvIsName(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return JSV_IS_NAME(f); } ///< NAMEs are what's used to name a variable (it is not the data itself)
bool jsvIsBasicName(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return f>=JSV_NAME_STRING_0 && f<=JSV_NAME_STRING_MAX; } ///< Simple NAME that links to a variable via firstChild
/// Names with values have firstChild set to a value - AND NOT A REFERENCE
bool jsvIsNameWithValue(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return JSV_IS_NAME_WITH_VALUE(f); }
bool jsvIsNameInt(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK;  return f==JSV_NAME_INT_INT || (f>=JSV_NAME_STRING_INT_0 && f<=JSV_NAME_STRING_INT_MAX); } ///< Is this a NAME pointing to an Integer value
bool jsvIsNameIntInt(const JsVar *v) { return v && (v->flags&JSV_VARTYPEMASK)==JSV_NAME_INT_INT; }
bool jsvIsNameIntBool(const JsVar *v) { return v && (v->flags&JSV_VARTYPEMASK)==JSV_NAME_INT_BOOL; }
/// What happens when we access a variable that doesn't exist. We get a NAME where the next + previous siblings point to the object that may one day contain them
bool jsvIsNewChild(const JsVar *v) { return jsvIsName(v) && jsvGetNextSibling(v) && jsvGetNextSibling(v)==jsvGetPrevSibling(v); }
/// Returns true if v is a getter/setter
bool jsvIsGetterOrSetter(const JsVar *v) { if (!v) return false; char f = v->flags&JSV_VARTYPEMASK; return JSV_IS_GETTER_OR_SETTER(f); }
/// Are var.varData.ref.* (excl pad) used for data (so we expect them not to be empty)
bool jsvIsRefUsedForData(const JsVar *v) {
  // Used for assert - no need to optimise
  return jsvIsStringExt(v) || (jsvIsString(v)&&!jsvIsName(v)) ||  jsvIsFloat(v) ||
         jsvIsNativeFunction(v) || jsvIsArrayBuffer(v) || jsvIsArrayBufferName(v);
}

/// Can the given variable be converted into an integer without loss of precision
bool jsvIsIntegerish(const JsVar *v) {  
  //return jsvIsInt(v) || jsvIsPin(v) || jsvIsBoolean(v) || jsvIsNull(v); 
  if (!v) return false;
  char f = v->flags&JSV_VARTYPEMASK;
  return JSV_IS_INT(f) || JSV_IS_PIN(f) || JSV_IS_BOOL(f) || JSV_IS_NULL(f);
}

bool jsvIsIterable(const JsVar *v) {
  //return jsvIsArray(v) || jsvIsObject(v) || jsvIsFunction(v) || jsvIsString(v) || jsvIsArrayBuffer(v);
  if (!v) return false;
  char f = v->flags&JSV_VARTYPEMASK;
  return
      JSV_IS_ARRAY(f) || JSV_IS_OBJECT(f) || JSV_IS_FUNCTION(f) ||
      JSV_IS_STRING(f) || JSV_IS_ARRAYBUFFER(f);
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

bool jsvHasCharacterData(const JsVar *v) {
  if (!v) return false;
  char f = v->flags&JSV_VARTYPEMASK;
  return JSV_IS_STRING(f) || JSV_IS_STRING_EXT(f);
}

bool jsvHasStringExt(const JsVar *v) {
  if (!v) return false;
    char f = v->flags&JSV_VARTYPEMASK;
  return (JSV_IS_STRING(f) || JSV_IS_STRING_EXT(f)) && !JSV_IS_NONAPPENDABLE_STRING(f);
}

bool jsvHasChildren(const JsVar *v) {
  if (!v) return false;
  char f = v->flags&JSV_VARTYPEMASK;
  return JSV_IS_FUNCTION(f) || JSV_IS_OBJECT(f) || JSV_IS_ARRAY(f) ||
         JSV_IS_ROOT(f) || JSV_IS_GETTER_OR_SETTER(f);
}

/// Is this variable a type that uses firstChild to point to a single Variable (ie. it doesn't have multiple children)
bool jsvHasSingleChild(const JsVar *v) {
  if (!v) return false;
  char f = v->flags&JSV_VARTYPEMASK;
  return JSV_IS_ARRAYBUFFER(f) ||
      (JSV_IS_NAME(f) && !JSV_IS_NAME_WITH_VALUE(f));
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------


/** Return a pointer - UNSAFE for null refs.
 * This is effectively a Lock without locking! */
static ALWAYS_INLINE JsVar *jsvGetAddressOf(JsVarRef ref) {
  assert(ref);
#ifdef RESIZABLE_JSVARS
  assert(ref <= jsVarsSize);
  JsVarRef t = ref-1;
  return &jsVarBlocks[t>>JSVAR_BLOCK_SHIFT][t&(JSVAR_BLOCK_SIZE-1)];
#elif defined(JSVAR_MALLOC)
  assert(ref <= jsVarsSize);
  return &jsVars[ref-1];
#else
  assert(ref <= JSVAR_CACHE_SIZE);
  return &jsVars[ref-1];
#endif
}

JsVar *_jsvGetAddressOf(JsVarRef ref) {
  return jsvGetAddressOf(ref);
}

// For debugging/testing ONLY - maximum # of vars we are allowed to use
void jsvSetMaxVarsUsed(unsigned int size) {
#ifdef RESIZABLE_JSVARS
  assert(size < JSVAR_BLOCK_SIZE); // remember - this is only for DEBUGGING - as such it doesn't use multiple blocks
  jsVarsSize = size;
#else
#ifdef JSVAR_MALLOC
  assert(size < JSVAR_CACHE_SIZE);
  jsVarsSize = size;
#else
  assert(0);
#endif
#endif

}

// maps the empty variables in...
void jsvCreateEmptyVarList() {
  assert(!isMemoryBusy);
  isMemoryBusy = MEMBUSY_SYSTEM;
  jsVarFirstEmpty = 0;
  JsVar firstVar; // temporary var to simplify code in the loop below
  jsvSetNextSibling(&firstVar, 0);
  JsVar *lastEmpty = &firstVar;

  JsVarRef i;
  for (i=1;i<=jsVarsSize;i++) {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags&JSV_VARTYPEMASK) == JSV_UNUSED) {
      jsvSetNextSibling(lastEmpty, i);
      lastEmpty = var;
    } else if (jsvIsFlatString(var)) {
      // skip over used blocks for flat strings
      i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
    }
  }
  jsvSetNextSibling(lastEmpty, 0);
  jsVarFirstEmpty = jsvGetNextSibling(&firstVar);
  isMemoryBusy = MEM_NOT_BUSY;
}

/* Removes the empty variable counter, cleaving clear runs of 0s
 where no data resides. This helps if compressing the variables
 for storage. */
void jsvClearEmptyVarList() {
  assert(!isMemoryBusy);
  isMemoryBusy = MEMBUSY_SYSTEM;
  jsVarFirstEmpty = 0;
  JsVarRef i;
  for (i=1;i<=jsVarsSize;i++) {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags&JSV_VARTYPEMASK) == JSV_UNUSED) {
      // completely zero it (JSV_UNUSED==0, so it still stays the same)
      memset((void*)var,0,sizeof(JsVar));
    } else if (jsvIsFlatString(var)) {
      // skip over used blocks for flat strings
      i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
    }
  }
  isMemoryBusy = MEM_NOT_BUSY;
}

void jsvSoftInit() {
  jsvCreateEmptyVarList();
}

void jsvSoftKill() {
  jsvClearEmptyVarList();
}

/** This links all JsVars together, so we can have our nice
 * linked list of free JsVars. It returns the ref of the first
 * item - that we should set jsVarFirstEmpty to (if it is 0) */
static JsVarRef jsvInitJsVars(JsVarRef start, unsigned int count) {
  JsVarRef i;
  for (i=start;i<start+count;i++) {
    JsVar *v = jsvGetAddressOf(i);
    v->flags = JSV_UNUSED;
    // v->locks = 0; // locks is 0 anyway because it is stored in flags
    jsvSetNextSibling(v, (JsVarRef)(i+1)); // link to next
  }
  jsvSetNextSibling(jsvGetAddressOf((JsVarRef)(start+count-1)), (JsVarRef)0); // set the final one to 0
  return start;
}

void jsvInit(unsigned int size) {
#ifdef RESIZABLE_JSVARS
  assert(size==0);
  jsVarsSize = JSVAR_BLOCK_SIZE;
  jsVarBlocks = malloc(sizeof(JsVar*)); // just 1
#if defined(ESPR_JIT) && defined(LINUX)
  jsVarBlocks[0] = (JsVar *)mmap(NULL, sizeof(JsVar) * JSVAR_BLOCK_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
#else
  jsVarBlocks[0] = malloc(sizeof(JsVar) * JSVAR_BLOCK_SIZE);
#endif
#elif defined(JSVAR_MALLOC)
  if (size) jsVarsSize = size;
#if defined(ESPR_JIT) && defined(LINUX)
  if (!jsVars)
    jsVars = (JsVar *)mmap(NULL, sizeof(JsVar) * jsVarsSize, PROT_EXEC | PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
#else
  if(!jsVars) jsVars = (JsVar *)malloc(sizeof(JsVar) * jsVarsSize);
#endif
#else
  assert(size==0);
#endif

  jsVarFirstEmpty = jsvInitJsVars(1/*first*/, jsVarsSize);
  jsvSoftInit();
}

void jsvKill() {
#ifdef RESIZABLE_JSVARS
  unsigned int i;
  for (i=0;i<jsVarsSize>>JSVAR_BLOCK_SHIFT;i++) {
#if defined(ESPR_JIT) && defined(LINUX)
    munmap(jsVarBlocks[i], sizeof(JsVar) * JSVAR_BLOCK_SIZE);
#else
    free(jsVarBlocks[i]);
#endif
  }
  free(jsVarBlocks);
  jsVarBlocks = 0;
  jsVarsSize = 0;
#endif
}

/** Find or create the ROOT variable item - used mainly
 * if recovering from a saved state. */
JsVar *jsvFindOrCreateRoot() {
  JsVarRef i;
  for (i=1;i<=jsVarsSize;i++)
    if (jsvIsRoot(jsvGetAddressOf(i)))
      return jsvLock(i);

  return jsvRef(jsvNewWithFlags(JSV_ROOT));
}

/// Get number of memory records (JsVars) used
unsigned int jsvGetMemoryUsage() {
  unsigned int usage = 0;
  for (unsigned int i=1;i<=jsVarsSize;i++) {
    JsVar *v = jsvGetAddressOf((JsVarRef)i);
    if ((v->flags&JSV_VARTYPEMASK) != JSV_UNUSED) {
      usage++;
      if (jsvIsFlatString(v)) {
        unsigned int b = (unsigned int)jsvGetFlatStringBlocks(v);
        i+=b;
        usage+=b;
      }
    }
  }
  return usage;
}

/// Get total amount of memory records
unsigned int jsvGetMemoryTotal() {
  return jsVarsSize;
}

/// Try and allocate more memory - only works if RESIZABLE_JSVARS is defined
void jsvSetMemoryTotal(unsigned int jsNewVarCount) {
#ifdef RESIZABLE_JSVARS
  assert(!isMemoryBusy);
  if (jsNewVarCount <= jsVarsSize) return; // never allow us to have less!
  isMemoryBusy = MEMBUSY_SYSTEM;
  // When resizing, we just allocate a bunch more
  unsigned int oldSize = jsVarsSize;
  unsigned int oldBlockCount = jsVarsSize >> JSVAR_BLOCK_SHIFT;
  unsigned int newBlockCount = (jsNewVarCount+JSVAR_BLOCK_SIZE-1) >> JSVAR_BLOCK_SHIFT;
  jsVarsSize = newBlockCount << JSVAR_BLOCK_SHIFT;
  // resize block table
  jsVarBlocks = realloc(jsVarBlocks, sizeof(JsVar*)*newBlockCount);
  // allocate more blocks
  unsigned int i;
  for (i=oldBlockCount;i<newBlockCount;i++)
    jsVarBlocks[i] = malloc(sizeof(JsVar) * JSVAR_BLOCK_SIZE);
  /** and now reset all the newly allocated vars. We know jsVarFirstEmpty
   * is 0 (because jsiFreeMoreMemory returned 0) so we can just assign it.  */
  assert(!jsVarFirstEmpty);
  jsVarFirstEmpty = jsvInitJsVars(oldSize+1, jsVarsSize-oldSize);
  // jsiConsolePrintf("Resized memory from %d blocks to %d\n", oldBlockCount, newBlockCount);
  touchedFreeList = true;
  isMemoryBusy = MEM_NOT_BUSY;
#else
  NOT_USED(jsNewVarCount);
  assert(0);
#endif
}

/// Scan memory to find any JsVar that references a specific memory range, and if so update what it points to to point to the new address
void jsvUpdateMemoryAddress(size_t oldAddr, size_t length, size_t newAddr) {
  for (unsigned int i=1;i<=jsVarsSize;i++) {
    JsVar *v = jsvGetAddressOf((JsVarRef)i);
    if (jsvIsNativeString(v) || jsvIsFlashString(v)) {
      size_t p = (size_t)v->varData.nativeStr.ptr;
      if (p>=oldAddr && p<oldAddr+length)
        v->varData.nativeStr.ptr = (char*)(p+newAddr-oldAddr);
    } else if (jsvIsFlatString(v)) {
      i += (unsigned int)jsvGetFlatStringBlocks(v);
    }
  }
}

bool jsvMoreFreeVariablesThan(unsigned int vars) {
  if (!vars) return false;
  JsVarRef r = jsVarFirstEmpty;
  while (r) {
    if (!vars--) return true;
    r = jsvGetNextSibling(jsvGetAddressOf(r));
  }
  return false;
}

/// Get whether memory is full or not
bool jsvIsMemoryFull() {
  return !jsVarFirstEmpty;
}

// Show what is still allocated, for debugging memory problems
void jsvShowAllocated() {
  JsVarRef i;
  for (i=1;i<=jsVarsSize;i++) {
    if ((jsvGetAddressOf(i)->flags&JSV_VARTYPEMASK) != JSV_UNUSED) {
      jsiConsolePrintf("USED VAR #%d:",i);
      jsvTrace(jsvGetAddressOf(i), 2);
    }
  }
}

/** Return the is the number of characters this one JsVar can contain, NOT string length (eg, a chain of JsVars)
 * This will return an invalid length when applied to Flat Strings */
size_t jsvGetMaxCharactersInVar(const JsVar *v) {
  // see jsvCopy - we need to know about this in there too
  if (jsvIsStringExt(v)) return JSVAR_DATA_STRING_MAX_LEN;
  assert(jsvHasCharacterData(v));
  if (jsvIsName(v)) return JSVAR_DATA_STRING_NAME_LEN;
  if (jsvIsFlatString(v) || jsvIsFlashString(v) || jsvIsNativeString(v)) return jsvGetCharactersInVar(v);
  return JSVAR_DATA_STRING_LEN;
}

/// This is the number of characters a JsVar can contain, NOT string length
size_t jsvGetCharactersInVar(const JsVar *v) {
  unsigned int f = v->flags&JSV_VARTYPEMASK;
  if (f == JSV_FLAT_STRING)
    return (size_t)v->varData.integer;
  if ((f == JSV_NATIVE_STRING)
#ifdef SPIFLASH_BASE
  || (f == JSV_FLASH_STRING)
#endif
      )
    return (size_t)v->varData.nativeStr.len;

  if (f < JSV_NAME_STRING_INT_0) jsiConsolePrintf("F %d\n", f);
  assert(f >= JSV_NAME_STRING_INT_0);
  assert((JSV_NAME_STRING_INT_0 < JSV_NAME_STRING_0) &&
         (JSV_NAME_STRING_0 < JSV_STRING_0) &&
         (JSV_STRING_0 < JSV_STRING_EXT_0)); // this relies on ordering
  if (f<=JSV_NAME_STRING_MAX) {
    if (f<=JSV_NAME_STRING_INT_MAX)
      return f-JSV_NAME_STRING_INT_0;
    else
      return f-JSV_NAME_STRING_0;
  } else {
    if (f<=JSV_STRING_MAX) return f-JSV_STRING_0;
    assert(f <= JSV_STRING_EXT_MAX);
    return f - JSV_STRING_EXT_0;
  }
}

/// This is the number of characters a JsVar can contain, NOT string length
void jsvSetCharactersInVar(JsVar *v, size_t chars) {
  unsigned int f = v->flags&JSV_VARTYPEMASK;
  assert(!(jsvIsFlatString(v) || jsvIsNativeString(v) || jsvIsFlashString(v)));

  JsVarFlags m = (JsVarFlags)(v->flags&~JSV_VARTYPEMASK);
  assert(f >= JSV_NAME_STRING_INT_0);
  assert((JSV_NAME_STRING_INT_0 < JSV_NAME_STRING_0) &&
         (JSV_NAME_STRING_0 < JSV_STRING_0) &&
         (JSV_STRING_0 < JSV_STRING_EXT_0)); // this relies on ordering
  if (f<=JSV_NAME_STRING_MAX) {
    assert(chars <= JSVAR_DATA_STRING_NAME_LEN);
    if (f<=JSV_NAME_STRING_INT_MAX)
      v->flags = (JsVarFlags)(m | (JSV_NAME_STRING_INT_0+chars));
    else
      v->flags = (JsVarFlags)(m | (JSV_NAME_STRING_0+chars));
  } else {
    if (f<=JSV_STRING_MAX) {
      assert(chars <= JSVAR_DATA_STRING_LEN);
      v->flags = (JsVarFlags)(m | (JSV_STRING_0+chars));
    } else {
      assert(chars <= JSVAR_DATA_STRING_MAX_LEN);
      assert(f <= JSV_STRING_EXT_MAX);
      v->flags = (JsVarFlags)(m | (JSV_STRING_EXT_0+chars));
    }
  }
}

void jsvResetVariable(JsVar *v, JsVarFlags flags) {
  assert((v->flags&JSV_VARTYPEMASK) == JSV_UNUSED);
  // make sure we clear all data...
  /* Force a proper zeroing of all data. We don't use
   * memset because that'd create a function call. This
   * should just generate a bunch of STR instructions */
  // FIXME: this does not generate STR instructions - it's just a tight loop with STRB. We should be able to do better?
  unsigned int i;
  if ((sizeof(JsVar)&3) == 0) {
    for (i=0;i<sizeof(JsVar)/sizeof(uint32_t);i++)
      ((uint32_t*)v)[i] = 0;
  } else { // just fall back to bytes and hope it's smart enough
    for (i=0;i<sizeof(JsVar);i++)
      ((uint8_t*)v)[i] = 0;
  }
  // set flags
  assert(!(flags & JSV_LOCK_MASK));
  v->flags = flags | JSV_LOCK_ONE;
}

JsVar *jsvNewWithFlags(JsVarFlags flags) {
  if (isMemoryBusy) {
    jsErrorFlags |= JSERR_MEMORY_BUSY;
    return 0;
  }
  JsVar *v = 0;
  jshInterruptOff(); // to allow this to be used from an IRQ
  if (jsVarFirstEmpty!=0) {
    v = jsvGetAddressOf(jsVarFirstEmpty); // jsvResetVariable will lock
    jsVarFirstEmpty = jsvGetNextSibling(v); // move our reference to the next in the free list
    touchedFreeList = true;
  }
  jshInterruptOn();
  if (v) {
    assert(v->flags == JSV_UNUSED);
    // Cope with IRQs/multi-threading when getting a new free variable
 /*   JsVarRef empty;
    JsVarRef next;
    JsVar *v;
    do {
      empty = jsVarFirstEmpty;
      v = jsvGetAddressOf(empty); // jsvResetVariable will lock
      next = jsvGetNextSibling(v); // move our reference to the next in the free list
      touchedFreeList = true;
    } while (!__sync_bool_compare_and_swap(&jsVarFirstEmpty, empty, next));
    assert(v->flags == JSV_UNUSED);*/
    jsvResetVariable(v, flags); // setup variable, and add one lock
    // return pointer
    return v;
  }
  jsErrorFlags |= JSERR_LOW_MEMORY;
  /* If we're calling from an IRQ, do NOT try and do fancy
   * stuff to free memory */
  if (jshIsInInterrupt()) {
    return 0;
  }
  /* we don't have memory - second last hope - run garbage collector */
  if (jsvGarbageCollect()) {
    return jsvNewWithFlags(flags); // if it freed something, continue
  }
  /* we don't have memory - last hope - ask jsInteractive to try and free some it
   may have kicking around */
  if (jsiFreeMoreMemory()) {
    return jsvNewWithFlags(flags);
  }
  /* We couldn't claim any more memory by Garbage collecting... */
#ifdef RESIZABLE_JSVARS
  jsvSetMemoryTotal(jsVarsSize*2);
  return jsvNewWithFlags(flags);
#else
  // On a micro, we're screwed.
  jsErrorFlags |= JSERR_MEMORY;
  jspSetInterrupted(true);
  return 0;
#endif
}

static void jsvFreePtrInternal(JsVar *var) {
  assert(jsvGetLocks(var)==0);
  var->flags = JSV_UNUSED;
  // add this to our free list
  jshInterruptOff(); // to allow this to be used from an IRQ
  jsvSetNextSibling(var, jsVarFirstEmpty);
  jsVarFirstEmpty = jsvGetRef(var);
  touchedFreeList = true;
  jshInterruptOn();
}

ALWAYS_INLINE void jsvFreePtr(JsVar *var) {
  /* To be here, we're not supposed to be part of anything else. If
   * we were, we'd have been freed by jsvGarbageCollect */
  assert((!jsvGetNextSibling(var) && !jsvGetPrevSibling(var)) || // check that next/prevSibling are not set
      jsvIsRefUsedForData(var) ||  // UNLESS we're part of a string and nextSibling/prevSibling are used for string data
      (jsvIsName(var) && (jsvGetNextSibling(var)==jsvGetPrevSibling(var)))); // UNLESS we're signalling that we're jsvild

  // Names that Link to other things
  if (jsvIsNameWithValue(var)) {
#ifdef CLEAR_MEMORY_ON_FREE
    jsvSetFirstChild(var, 0); // it just contained random data - zero it
#endif // CLEAR_MEMORY_ON_FREE
  } else if (jsvHasSingleChild(var)) {
    if (jsvGetFirstChild(var)) {
      if (jsuGetFreeStack() > 256) {
        // we have to check stack here in case someone allocates some huge linked list.
        // if we just unreference the rest hopefully it'll be cleaned on the next GC pass
        // https://github.com/espruino/Espruino/issues/2136
        JsVar *child = jsvLock(jsvGetFirstChild(var));
        jsvUnRef(child);
        jsvUnLock(child); // unlock should trigger a free
      }
#ifdef CLEAR_MEMORY_ON_FREE
      jsvSetFirstChild(var, 0); // unlink the child
#endif // CLEAR_MEMORY_ON_FREE
    }
  }
  /* No else, because a String Name may have a single child, but
   * also StringExts  */

  /* Now, free children - see jsvar.h comments for how! */
  if (jsvHasStringExt(var)) {
    // Free the string without recursing
    JsVarRef stringDataRef = jsvGetLastChild(var);
#ifdef CLEAR_MEMORY_ON_FREE
    jsvSetLastChild(var, 0);
#endif // CLEAR_MEMORY_ON_FREE
    while (stringDataRef) {
      JsVar *child = jsvGetAddressOf(stringDataRef);
      assert(jsvIsStringExt(child));
      stringDataRef = jsvGetLastChild(child);
      jsvFreePtrInternal(child);
    }
    if (jsvIsBasicString(var)) {
  #ifdef CLEAR_MEMORY_ON_FREE
      jsvSetFirstChild(var, 0); // firstchild could have had string data in
  #endif // CLEAR_MEMORY_ON_FREE
    }
  } else if (jsvIsFlatString(var)) { // We might be a flat string (we're not allowing strings to be added to flat strings yet)
    // in which case we need to free all the blocks.
    size_t count = jsvGetFlatStringBlocks(var);
    JsVarRef i = (JsVarRef)(jsvGetRef(var)+count);
    // Because this is a whole bunch of blocks, try
    // and insert it in the right place in the free list
    // So, iterate along free list to figure out where we
    // need to insert the free items
    jshInterruptOff(); // to allow this to be used from an IRQ
    JsVarRef insertBefore = jsVarFirstEmpty;
    JsVarRef insertAfter = 0;
    while (insertBefore && insertBefore<i) {
      insertAfter = insertBefore;
      insertBefore = jsvGetNextSibling(jsvGetAddressOf(insertBefore));
    }
    // free in reverse, so the free list ends up in kind of the right order
    while (count--) {
      JsVar *p = jsvGetAddressOf(i--);
      p->flags = JSV_UNUSED; // set locks to 0 so the assert in jsvFreePtrInternal doesn't get fed up
      // add this to our free list
      jsvSetNextSibling(p, insertBefore);
      insertBefore = jsvGetRef(p);
    }
    // patch up jsVarFirstEmpty/rejoin the list
    if (insertAfter)
      jsvSetNextSibling(jsvGetAddressOf(insertAfter), insertBefore);
    else
      jsVarFirstEmpty = insertBefore;
    touchedFreeList = true;
    jshInterruptOn();
  }

  /* NO ELSE HERE - because jsvIsNewChild stuff can be for Names, which
    can be ints or strings */

  if (jsvHasChildren(var)) {
    JsVarRef childref = jsvGetFirstChild(var);
#ifdef CLEAR_MEMORY_ON_FREE
    jsvSetFirstChild(var, 0);
    jsvSetLastChild(var, 0);
#endif // CLEAR_MEMORY_ON_FREE
    while (childref) {
      JsVar *child = jsvLock(childref);
      assert(jsvIsName(child));
      childref = jsvGetNextSibling(child);
      jsvSetPrevSibling(child, 0);
      jsvSetNextSibling(child, 0);
      jsvUnRef(child);
      jsvUnLock(child);
    }
  } else {
#ifdef CLEAR_MEMORY_ON_FREE
    assert(jsvIsFloat(var) || !jsvGetFirstChild(var));
    assert(jsvIsFloat(var) || !jsvGetLastChild(var));
#endif // CLEAR_MEMORY_ON_FREE
    if (jsvIsName(var)) {
      assert(jsvGetNextSibling(var)==jsvGetPrevSibling(var)); // the case for jsvIsNewChild
      if (jsvGetNextSibling(var)) {
        jsvUnRefRef(jsvGetNextSibling(var));
        jsvUnRefRef(jsvGetPrevSibling(var));
      }
    }
  }

  // free!
  jsvFreePtrInternal(var);
}

/// Get a reference from a var - SAFE for null vars
JsVarRef jsvGetRef(JsVar *var) {
  if (!var) return 0;
#ifdef RESIZABLE_JSVARS
  unsigned int i, c = jsVarsSize>>JSVAR_BLOCK_SHIFT;
  for (i=0;i<c;i++) {
    if (var>=jsVarBlocks[i] && var<&jsVarBlocks[i][JSVAR_BLOCK_SIZE]) {
      JsVarRef r = (JsVarRef)(1 + (i<<JSVAR_BLOCK_SHIFT) + (var - jsVarBlocks[i]));
      return r;
    }
  }
  return 0;
#else
  return (JsVarRef)(1 + (var - jsVars));
#endif
}

/// Lock this reference and return a pointer - UNSAFE for null refs
JsVar *jsvLock(JsVarRef ref) {
  JsVar *var = jsvGetAddressOf(ref);
  //var->locks++;
  assert(jsvGetLocks(var) < JSV_LOCK_MAX);
  var->flags += JSV_LOCK_ONE;
#ifdef DEBUG
  if (jsvGetLocks(var)==0) {
    jsError("Too many locks to Variable!");
    //jsPrint("Var #");jsPrintInt(ref);jsPrint("\n");
  }
#endif
  return var;
}

/// Lock this reference and return a pointer - UNSAFE for null refs
JsVar *jsvLockSafe(JsVarRef ref) {
  if (!ref) return 0;
  return jsvLock(ref);
}

/// Lock this pointer and return a pointer - UNSAFE for null pointer
JsVar *jsvLockAgain(JsVar *var) {
  assert(var);
  assert(jsvGetLocks(var) < JSV_LOCK_MAX);
  var->flags += JSV_LOCK_ONE;
  return var;
}

/// Lock this pointer and return a pointer - UNSAFE for null pointer
JsVar *jsvLockAgainSafe(JsVar *var) {
  return var ? jsvLockAgain(var) : 0;
}

// CALL ONLY FROM jsvUnlock
// jsvGetLocks(var) must == 0
static NO_INLINE void jsvUnLockFreeIfNeeded(JsVar *var) {
  assert(jsvGetLocks(var) == 0);
  /* if we know we're free, then we can just free this variable right now.
   * Loops of variables are handled by the Garbage Collector.
   * Note: we checked locks already in jsvUnLock as it is fastest to check */
  if (jsvGetRefs(var) == 0 &&
      jsvHasRef(var) &&
      (var->flags&JSV_VARTYPEMASK)!=JSV_UNUSED) { // we might be in an IRQ now, with GC in the main thread. If so, don't free!
    jsvFreePtr(var);
  }
}

/** Unlock this variable - this is SAFE for null variables
 * This version is used so we can ensure that the relatively
 * simple code here gets inlined for jsvUnLock2/etc */
static ALWAYS_INLINE void jsvUnLockInline(JsVar *var) {
  if (!var) return;
  assert(jsvGetLocks(var)>0);
  /* Reduce lock count. Since ->flags is volatile
   * it helps to explicitly save it to a var to avoid a
   * load-store-load */
  JsVarFlags f = var->flags -= JSV_LOCK_ONE;
  // Now see if we can properly free the data
  // Note: we check locks first as they are already in a register
  if ((f & JSV_LOCK_MASK) == 0) jsvUnLockFreeIfNeeded(var);
}

/// Unlock this variable - this is SAFE for null variables
void jsvUnLock(JsVar *var) {
  jsvUnLockInline(var);
}

/// Unlock 2 variables in one go
void jsvUnLock2(JsVar *var1, JsVar *var2) {
  jsvUnLockInline(var1);
  jsvUnLockInline(var2);
}
/// Unlock 3 variables in one go
void jsvUnLock3(JsVar *var1, JsVar *var2, JsVar *var3) {
  jsvUnLockInline(var1);
  jsvUnLockInline(var2);
  jsvUnLockInline(var3);
}
/// Unlock 4 variables in one go
void jsvUnLock4(JsVar *var1, JsVar *var2, JsVar *var3, JsVar *var4) {
  jsvUnLockInline(var1);
  jsvUnLockInline(var2);
  jsvUnLockInline(var3);
  jsvUnLockInline(var4);
}

/// Unlock an array of variables
NO_INLINE void jsvUnLockMany(unsigned int count, JsVar **vars) {
  while (count) jsvUnLockInline(vars[--count]);
}

/// Reference - set this variable as used by something
JsVar *jsvRef(JsVar *var) {
  assert(var && jsvHasRef(var));
  if (jsvGetRefs(var) < JSVARREFCOUNT_MAX) // if we hit max refcounts, just keep them - GC will fix it later
    jsvSetRefs(var, (JsVarRefCounter)(jsvGetRefs(var)+1));
  assert(jsvGetRefs(var));
  return var;
}

/// Unreference - set this variable as not used by anything
void jsvUnRef(JsVar *var) {
  assert(var && jsvGetRefs(var)>0 && jsvHasRef(var));
  if (jsvGetRefs(var) < JSVARREFCOUNT_MAX) // if we hit max refcounts, just keep them - GC will fix it later
    jsvSetRefs(var, (JsVarRefCounter)(jsvGetRefs(var)-1));
}

/// Helper fn, Reference - set this variable as used by something
JsVarRef jsvRefRef(JsVarRef ref) {
  JsVar *v;
  assert(ref);
  v = jsvLock(ref);
  assert(!jsvIsStringExt(v));
  jsvRef(v);
  jsvUnLock(v);
  return ref;
}

/// Helper fn, Unreference - set this variable as not used by anything
JsVarRef jsvUnRefRef(JsVarRef ref) {
  JsVar *v;
  assert(ref);
  v = jsvLock(ref);
  assert(!jsvIsStringExt(v));
  jsvUnRef(v);
  jsvUnLock(v);
  return 0;
}

JsVar *jsvNewFlatStringOfLength(unsigned int byteLength) {
  bool firstRun = true;
  // Work out how many blocks we need. One for the header, plus some for the characters
  size_t requiredBlocks = 1 + ((byteLength+sizeof(JsVar)-1) / sizeof(JsVar));
  JsVar *flatString = 0;
  if (isMemoryBusy) {
    jsErrorFlags |= JSERR_MEMORY_BUSY;
    return 0;
  }
  while (true) {
    /* Now try and find a contiguous set of 'requiredBlocks' blocks by
    searching the free list. This can be done as long as nobody's
    messed with the free list in the mean time (which we check for with
    touchedFreeList). If someone has messed with it, we restart.*/
    bool memoryTouched = true;
    while (memoryTouched) {
      memoryTouched = false;
      touchedFreeList = false;
      JsVarRef beforeStartBlock = 0;
      JsVarRef curr = jsVarFirstEmpty;
      JsVarRef startBlock = curr;
      unsigned int blockCount = 0;
      while (curr && !touchedFreeList) {
        JsVar *currVar = jsvGetAddressOf(curr);
        JsVarRef next = jsvGetNextSibling(currVar);
  #ifdef RESIZABLE_JSVARS
        if (blockCount && next && (jsvGetAddressOf(next)==currVar+1)) {
  #else
        if (blockCount && (next == curr+1)) {
  #endif
          blockCount++;
          if (blockCount>=requiredBlocks) {
            JsVar *nextVar = jsvGetAddressOf(next);
            JsVarRef nextFree = jsvGetNextSibling(nextVar);
            jshInterruptOff();
            if (!touchedFreeList) {
              // we're there! Quickly re-link free list
              if (beforeStartBlock) {
                jsvSetNextSibling(jsvGetAddressOf(beforeStartBlock),nextFree);
              } else {
                jsVarFirstEmpty = nextFree;
              }
              flatString = jsvGetAddressOf(startBlock);
              // Set up the header block (including one lock)
              jsvResetVariable(flatString, JSV_FLAT_STRING);
              flatString->varData.integer = (JsVarInt)byteLength;
            }
            jshInterruptOn();
            // if success, break out!
            if (flatString) break;
          }
        } else {
          // this block is not immediately after the last - restart run
          beforeStartBlock = curr;
          startBlock = next;
          // Check to see if the next block is aligned on a 4 byte boundary or not
          if (startBlock==jsVarsSize || ((size_t)(jsvGetAddressOf(startBlock+1)))&3)
            blockCount = 0; // this block is not aligned
          else
            blockCount = 1; // all ok - start block here
        }
        // move to next!
        curr = next;
      }
      // memory list has been touched - restart!
      if (touchedFreeList) {
        memoryTouched = true;
      }
    }

    // all good
    if (flatString || !firstRun)
      break;
    /* Nope... we couldn't find a free string. It could be because
     * the free list is fragmented, so GCing might well fix it - which
     * we'll try - but only ONCE */
    firstRun = false;
    jsvGarbageCollect();
  };
  if (!flatString) return 0;
  /* We now have the string! All that's left is to clear it */
  // clear data
  memset((char*)&flatString[1], 0, sizeof(JsVar)*(requiredBlocks-1));
  /* We did mess with the free list - set it here in case we
  are trying to create a flat string in an IRQ while trying to
  make one outside the IRQ too */
  touchedFreeList = true;
  // and we're done
  return flatString;
}

JsVar *jsvNewFromString(const char *str) {
  // Create a var
  JsVar *first = jsvNewWithFlags(JSV_STRING_0);
  if (!first) return 0; // out of memory
  // Now we copy the string, but keep creating new jsVars if we go
  // over the end
  JsVar *var = jsvLockAgain(first);
  while (*str) {
    // copy data in
    size_t i, l = jsvGetMaxCharactersInVar(var);
    for (i=0;i<l && *str;i++)
      var->varData.str[i] = *(str++);
    // we already set the variable data to 0, so no need for adding one

    // we've stopped if the string was empty
    jsvSetCharactersInVar(var, i);

    // if there is still some left, it's because we filled up our var...
    // make a new one, link it in, and unlock the old one.
    if (*str) {
      JsVar *next = jsvNewWithFlags(JSV_STRING_EXT_0);
      if (!next) {
        // Truncating string as not enough memory
        jsvUnLock(var);
        return first;
      }
      // we don't ref, because  StringExts are never reffed as they only have one owner (and ALWAYS have an owner)
      jsvSetLastChild(var, jsvGetRef(next));
      jsvUnLock(var);
      var = next;
    }
  }
  jsvUnLock(var);
  // return
  return first;
}

JsVar *jsvNewStringOfLength(unsigned int byteLength, const char *initialData) {
  // if string large enough, try and make a flat string instead
  if (byteLength > JSV_FLAT_STRING_BREAK_EVEN) {
    JsVar *v = jsvNewFlatStringOfLength(byteLength);
    if (v) {
      if (initialData) jsvSetString(v, initialData, byteLength);
      return v;
    }
  }
  // Create a var
  JsVar *first = jsvNewWithFlags(JSV_STRING_0);
  if (!first) return 0; // out of memory, will have already set flag
  // Now keep creating enough new jsVars
  JsVar *var = jsvLockAgain(first);
  while (true) {
    // copy data in
    unsigned int l = (unsigned int)jsvGetMaxCharactersInVar(var);
    if (l>=byteLength) {
      if (initialData)
        memcpy(var->varData.str, initialData, byteLength);
      // we've got enough
      jsvSetCharactersInVar(var, byteLength);
      break;
    } else {
      if (initialData) {
        memcpy(var->varData.str, initialData, l);
        initialData+=l;
      }
      // We need more
      jsvSetCharactersInVar(var, l);
      byteLength -= l;
      // Make a new one, link it in, and unlock the old one.
      JsVar *next = jsvNewWithFlags(JSV_STRING_EXT_0);
      if (!next) break; // out of memory, will have already set flag
      // we don't ref, because  StringExts are never reffed as they only have one owner (and ALWAYS have an owner)
      jsvSetLastChild(var, jsvGetRef(next));
      jsvUnLock(var);
      var = next;
    }
  }
  jsvUnLock(var);
  // return
  return first;
}

JsVar *jsvNewFromInteger(JsVarInt value) {
  JsVar *var = jsvNewWithFlags(JSV_INTEGER);
  if (!var) return 0; // no memory
  var->varData.integer = value;
  return var;
}
JsVar *jsvNewFromBool(bool value) {
  JsVar *var = jsvNewWithFlags(JSV_BOOLEAN);
  if (!var) return 0; // no memory
  var->varData.integer = value ? 1 : 0;
  return var;
}
JsVar *jsvNewFromFloat(JsVarFloat value) {
  JsVar *var = jsvNewWithFlags(JSV_FLOAT);
  if (!var) return 0; // no memory
  var->varData.floating = value;
  return var;
}
JsVar *jsvNewFromLongInteger(long long value) {
  if (value>=-2147483648LL && value<=2147483647LL)
    return jsvNewFromInteger((JsVarInt)value);
  else
    return jsvNewFromFloat((JsVarFloat)value);
}

JsVar *jsvNewFromPin(int pin) {
  JsVar *v = jsvNewFromInteger((JsVarInt)pin);
  if (v) {
    v->flags = (JsVarFlags)((v->flags & ~JSV_VARTYPEMASK) | JSV_PIN);
  }
  return v;
}

JsVar *jsvNewObject() {
  return jsvNewWithFlags(JSV_OBJECT);
}

JsVar *jsvNewEmptyArray() {
  return jsvNewWithFlags(JSV_ARRAY);
}

/// Create an array containing the given elements
JsVar *jsvNewArray(JsVar **elements, int elementCount) {
  JsVar *arr = jsvNewEmptyArray();
  if (!arr) return 0;
  int i;
  for (i=0;i<elementCount;i++)
    jsvArrayPush(arr, elements[i]);
  return arr;
}

JsVar *jsvNewArrayFromBytes(uint8_t *elements, int elementCount) {
  JsVar *arr = jsvNewEmptyArray();
  if (!arr) return 0;
  int i;
  for (i=0;i<elementCount;i++)
    jsvArrayPushAndUnLock(arr, jsvNewFromInteger(elements[i]));
  return arr;
}

JsVar *jsvNewNativeFunction(void (*ptr)(void), unsigned short argTypes) {
  JsVar *func = jsvNewWithFlags(JSV_NATIVE_FUNCTION);
  if (!func) return 0;
  func->varData.native.ptr = ptr;
  func->varData.native.argTypes = argTypes;
  return func;
}

JsVar *jsvNewNativeString(char *ptr, size_t len) {
  if (len>JSV_NATIVE_STR_MAX_LENGTH) len=JSV_NATIVE_STR_MAX_LENGTH; // crop string to what we can store in nativeStr.len
  JsVar *str = jsvNewWithFlags(JSV_NATIVE_STRING);
  if (!str) return 0;
  str->varData.nativeStr.ptr = ptr;
  str->varData.nativeStr.len = (JsVarDataNativeStrLength)len;
  return str;
}

#ifdef SPIFLASH_BASE
JsVar *jsvNewFlashString(char *ptr, size_t len) {
  if (len>JSV_NATIVE_STR_MAX_LENGTH) len=JSV_NATIVE_STR_MAX_LENGTH; // crop string to what we can store in nativeStr.len
    JsVar *str = jsvNewWithFlags(JSV_FLASH_STRING);
    if (!str) return 0;
    str->varData.nativeStr.ptr = ptr;
    str->varData.nativeStr.len = (JsVarDataNativeStrLength)len;
    return str;
}
#endif

/// Create a new ArrayBuffer backed by the given string. If length is not specified, it will be worked out
JsVar *jsvNewArrayBufferFromString(JsVar *str, unsigned int lengthOrZero) {
  JsVar *arr = jsvNewWithFlags(JSV_ARRAYBUFFER);
  if (!arr) return 0;
  jsvSetFirstChild(arr, jsvGetRef(jsvRef(str)));
  arr->varData.arraybuffer.type = ARRAYBUFFERVIEW_ARRAYBUFFER;
  assert(arr->varData.arraybuffer.byteOffset == 0);
  if (lengthOrZero==0) lengthOrZero = (unsigned int)jsvGetStringLength(str);
  arr->varData.arraybuffer.length = (unsigned short)lengthOrZero;
  return arr;
}

JsVar *jsvMakeIntoVariableName(JsVar *var, JsVar *valueOrZero) {
  if (!var) return 0;
  assert(jsvGetRefs(var)==0); // make sure it's unused
  assert(jsvIsSimpleInt(var) || jsvIsString(var));
  JsVarFlags varType = (var->flags & JSV_VARTYPEMASK);
  if (varType==JSV_INTEGER) {
    int t = JSV_NAME_INT;
    if ((jsvIsInt(valueOrZero) || jsvIsBoolean(valueOrZero)) && !jsvIsPin(valueOrZero)) {
      JsVarInt v = valueOrZero->varData.integer;
      if (v>=JSVARREF_MIN && v<=JSVARREF_MAX) {
        t = jsvIsInt(valueOrZero) ? JSV_NAME_INT_INT : JSV_NAME_INT_BOOL;
        jsvSetFirstChild(var, (JsVarRef)v);
        valueOrZero = 0;
      }
    }
    var->flags = (JsVarFlags)(var->flags & ~JSV_VARTYPEMASK) | t;
  } else if (varType>=_JSV_STRING_START && varType<=_JSV_STRING_END) {
    if (jsvGetCharactersInVar(var) > JSVAR_DATA_STRING_NAME_LEN) {
      /* Argh. String is too large to fit in a JSV_NAME! We must chomp make
       * new STRINGEXTs to put the data in
       */
      JsvStringIterator it;
      jsvStringIteratorNew(&it, var, JSVAR_DATA_STRING_NAME_LEN);
      JsVar *startExt = jsvNewWithFlags(JSV_STRING_EXT_0);
      JsVar *ext = jsvLockAgainSafe(startExt);
      size_t nChars = 0;
      while (ext && jsvStringIteratorHasChar(&it)) {
        if (nChars >= JSVAR_DATA_STRING_MAX_LEN) {
          jsvSetCharactersInVar(ext, nChars);
          JsVar *ext2 = jsvNewWithFlags(JSV_STRING_EXT_0);
          if (ext2) {
            jsvSetLastChild(ext, jsvGetRef(ext2));
          }
          jsvUnLock(ext);
          ext = ext2;
          nChars = 0;
        }
        ext->varData.str[nChars++] = jsvStringIteratorGetCharAndNext(&it);
      }
      jsvStringIteratorFree(&it);
      if (ext) {
        jsvSetCharactersInVar(ext, nChars);
        jsvUnLock(ext);
      }
      jsvSetCharactersInVar(var, JSVAR_DATA_STRING_NAME_LEN);
      // Free any old stringexts
      JsVarRef oldRef = jsvGetLastChild(var);
      while (oldRef) {
        JsVar *v = jsvGetAddressOf(oldRef);
        oldRef = jsvGetLastChild(v);
        jsvFreePtrInternal(v);
      }
      // set up new stringexts
      jsvSetLastChild(var, jsvGetRef(startExt));
      jsvSetNextSibling(var, 0);
      jsvSetPrevSibling(var, 0);
      jsvSetFirstChild(var, 0);
      jsvUnLock(startExt);
    }

    size_t t = JSV_NAME_STRING_0;
    if (jsvIsInt(valueOrZero) && !jsvIsPin(valueOrZero)) {
      JsVarInt v = valueOrZero->varData.integer;
      if (v>=JSVARREF_MIN && v<=JSVARREF_MAX) {
        t = JSV_NAME_STRING_INT_0;
        jsvSetFirstChild(var, (JsVarRef)v);
        valueOrZero = 0;
      }
    } else
      jsvSetFirstChild(var, 0);
    var->flags = (var->flags & (JsVarFlags)~JSV_VARTYPEMASK) | (t+jsvGetCharactersInVar(var));
  } else assert(0);

  if (valueOrZero)
    jsvSetFirstChild(var, jsvGetRef(jsvRef(valueOrZero)));
  return var;
}

void jsvMakeFunctionParameter(JsVar *v) {
  assert(jsvIsString(v));
  if (!jsvIsName(v)) jsvMakeIntoVariableName(v,0);
  v->flags = (JsVarFlags)(v->flags | JSV_NATIVE);
}

/// Add a new unnamed function parameter to a function - use this when binding function arguments. This unlocks paramName if specified, but not value.
void jsvAddFunctionParameter(JsVar *fn, JsVar *paramName, JsVar *value) {
  assert(jsvIsFunction(fn));
  if (!paramName) paramName = jsvNewFromEmptyString();
  assert(jsvIsString(paramName));
  if (paramName) {
    jsvMakeFunctionParameter(paramName); // force this to be called a function parameter
    jsvSetValueOfName(paramName, value);
    jsvAddName(fn, paramName);
    jsvUnLock(paramName);
  }
}

void *jsvGetNativeFunctionPtr(const JsVar *function) {
  /* see descriptions in jsvar.h. If we have a child called JSPARSE_FUNCTION_CODE_NAME
   * then we execute code straight from that */
  JsVar *flatString = jsvFindChildFromString((JsVar*)function, JSPARSE_FUNCTION_CODE_NAME, 0);
  if (flatString) {
    flatString = jsvSkipNameAndUnLock(flatString);
    void *v = (void*)((size_t)function->varData.native.ptr + (char*)jsvGetFlatStringPointer(flatString));
    jsvUnLock(flatString);
    return v;
  } else
    return (void *)function->varData.native.ptr;
}


bool jsvIsBasicVarEqual(JsVar *a, JsVar *b) {
  // quick checks
  if (a==b) return true;
  if (!a || !b) return false; // one of them is undefined
  // OPT: would this be useful as compare instead?
  assert(jsvIsBasic(a) && jsvIsBasic(b));
  if (jsvIsNumeric(a) && jsvIsNumeric(b)) {
    if (jsvIsIntegerish(a)) {
      if (jsvIsIntegerish(b)) {
        return a->varData.integer == b->varData.integer;
      } else {
        assert(jsvIsFloat(b));
        return a->varData.integer == b->varData.floating;
      }
    } else {
      assert(jsvIsFloat(a));
      if (jsvIsIntegerish(b)) {
        return a->varData.floating == b->varData.integer;
      } else {
        assert(jsvIsFloat(b));
        return a->varData.floating == b->varData.floating;
      }
    }
  } else if (jsvIsString(a) && jsvIsString(b)) {
    // OPT: could we do a fast check here with data?
    JsvStringIterator ita, itb;
    jsvStringIteratorNew(&ita, a, 0);
    jsvStringIteratorNew(&itb, b, 0);
    while (true) {
      int a = jsvStringIteratorGetCharOrMinusOne(&ita);
      jsvStringIteratorNext(&ita);
      int b = jsvStringIteratorGetCharOrMinusOne(&itb);
      jsvStringIteratorNext(&itb);
      if (a != b) {
        jsvStringIteratorFree(&ita);
        jsvStringIteratorFree(&itb);
        return false;
      }
      if (a < 0) { // equal, but end of string
        jsvStringIteratorFree(&ita);
        jsvStringIteratorFree(&itb);
        return true;
      }
    }
    // we never get here
    return false; // make compiler happy
  } else {
    //TODO: are there any other combinations we should check here?? String v int?
    return false;
  }
}

bool jsvIsEqual(JsVar *a, JsVar *b) {
  if (jsvIsBasic(a) && jsvIsBasic(b))
    return jsvIsBasicVarEqual(a,b);
  return jsvGetRef(a)==jsvGetRef(b);
}

/// Get a const string representing this variable - if we can. Otherwise return 0
const char *jsvGetConstString(const JsVar *v) {
  if (jsvIsUndefined(v)) {
    return "undefined";
  } else if (jsvIsNull(v)) {
    return "null";
  } else if (jsvIsBoolean(v) && !jsvIsNameIntBool(v)) {
    return jsvGetBool(v) ? "true" : "false";
  }
  return 0;
}

/// Return the 'type' of the JS variable (eg. JS's typeof operator)
const char *jsvGetTypeOf(const JsVar *v) {
  if (jsvIsUndefined(v)) return "undefined";
  if (jsvIsNull(v) || jsvIsObject(v) ||
      jsvIsArray(v) || jsvIsArrayBuffer(v)) return "object";
  if (jsvIsFunction(v)) return "function";
  if (jsvIsString(v)) return "string";
  if (jsvIsBoolean(v)) return "boolean";
  if (jsvIsNumeric(v)) return "number";
  return "?";
}

/// Return the JsVar, or if it's an object and has a valueOf function, call that
JsVar *jsvGetValueOf(JsVar *v) {
  if (!jsvIsObject(v)) return jsvLockAgainSafe(v);
  JsVar *valueOf = jspGetNamedField(v, "valueOf", false);
  if (!jsvIsFunction(valueOf)) {
    jsvUnLock(valueOf);
    return jsvLockAgain(v);
  }
  v = jspeFunctionCall(valueOf, 0, v, false, 0, 0);
  jsvUnLock(valueOf);
  return v;
}

/** Save this var as a string to the given buffer, and return how long it was (return val doesn't include terminating 0)
If the buffer length is exceeded, the returned value will == len */
size_t jsvGetString(const JsVar *v, char *str, size_t len) {
  assert(len>0);
  const char *s = jsvGetConstString(v);
  if (s) {
    /* don't use strncpy here because we don't
     * want to pad the entire buffer with zeros */
    len--;
    size_t l = 0;
    while (s[l] && l<len) {
      str[l] = s[l];
      l++;
    }
    str[l] = 0;
    return l;
  } else if (jsvIsInt(v)) {
    itostr(v->varData.integer, str, 10);
    return strlen(str);
  } else if (jsvIsFloat(v)) {
    ftoa_bounded(v->varData.floating, str, len);
    return strlen(str);
  } else if (jsvHasCharacterData(v)) {
    assert(!jsvIsStringExt(v));
    size_t l = len;
    JsvStringIterator it;
    jsvStringIteratorNewConst(&it, v, 0);
    while (jsvStringIteratorHasChar(&it)) {
      if (l--<=1) {
        *str = 0;
        jsvStringIteratorFree(&it);
        return len;
      }
      *(str++) = jsvStringIteratorGetChar(&it);
      jsvStringIteratorNext(&it);
    }
    jsvStringIteratorFree(&it);
    *str = 0;
    return len-l;
  } else {
    // Try and get as a JsVar string, and try again
    JsVar *stringVar = jsvAsString((JsVar*)v); // we know we're casting to non-const here
    if (stringVar) {
      size_t l = jsvGetStringChars(stringVar, 0, str, len); // call again - but this time with converted var
      jsvUnLock(stringVar);
      return l;
    } else {
      str[0] = 0;
      jsExceptionHere(JSET_INTERNALERROR, "Variable type cannot be converted to string");
      return 0;
    }
  }
}

/// Get len bytes of string data from this string. Does not error if string len is not equal to len, no terminating 0
size_t jsvGetStringChars(const JsVar *v, size_t startChar, char *str, size_t len) {
  assert(jsvHasCharacterData(v));
  size_t l = len;
  JsvStringIterator it;
  jsvStringIteratorNewConst(&it, v, startChar);
  while (jsvStringIteratorHasChar(&it)) {
    if (l--<=0) {
      jsvStringIteratorFree(&it);
      return len;
    }
    *(str++) = jsvStringIteratorGetCharAndNext(&it);
  }
  jsvStringIteratorFree(&it);
  return len-l;
}

/// Set the Data in this string. This must JUST overwrite - not extend or shrink
void jsvSetString(JsVar *v, const char *str, size_t len) {
  assert(jsvHasCharacterData(v));
  // the iterator checks, so it is safe not to assert if the length is different
  //assert(len == jsvGetStringLength(v));


  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, 0);
  size_t i;
  for (i=0;i<len;i++) {
    jsvStringIteratorSetCharAndNext(&it, str[i]);
  }
  jsvStringIteratorFree(&it);
}

/** If var is a string, lock and return it, else
 * create a new string. unlockVar means this will auto-unlock 'var'  */
JsVar *jsvAsString(JsVar *v) {
  JsVar *str = 0;
  // If it is string-ish, but not quite a string, copy it
  if (jsvHasCharacterData(v) && jsvIsName(v)) {
    str = jsvNewFromStringVar(v,0,JSVAPPENDSTRINGVAR_MAXLENGTH);
  } else if (jsvIsString(v)) { // If it is a string - just return a reference
    str = jsvLockAgain(v);
  } else if (jsvIsObject(v)) { // If it is an object and we can call toString on it
    JsVar *toStringFn = jspGetNamedField(v, "toString", false);
    if (toStringFn && toStringFn->varData.native.ptr != (void (*)(void))jswrap_object_toString) {
      // Function found and it's not the default one - execute it
      JsVar *result = jspExecuteFunction(toStringFn,v,0,0);
      jsvUnLock(toStringFn);
      str = jsvAsStringAndUnLock(result);
    } else {
      jsvUnLock(toStringFn);
      str = jsvNewFromString("[object Object]");
    }
  } else {
    const char *constChar = jsvGetConstString(v);
    assert(JS_NUMBER_BUFFER_SIZE>=10);
    char buf[JS_NUMBER_BUFFER_SIZE];
    if (constChar) {
      // if we could get this as a simple const char, do that..
      str = jsvNewFromString(constChar);
    } else if (jsvIsPin(v)) {
      jshGetPinString(buf, (Pin)v->varData.integer);
      str = jsvNewFromString(buf);
    } else if (jsvIsInt(v)) {
      itostr(v->varData.integer, buf, 10);
      str = jsvNewFromString(buf);
    } else if (jsvIsFloat(v)) {
      ftoa_bounded(v->varData.floating, buf, sizeof(buf));
      str = jsvNewFromString(buf);
    } else if (jsvIsArray(v) || jsvIsArrayBuffer(v)) {
      JsVar *filler = jsvNewFromString(",");
      str = jsvArrayJoin(v, filler, true/*ignoreNull*/);
      jsvUnLock(filler);
    } else if (jsvIsFunction(v)) {
      str = jsvNewFromEmptyString();
      if (str) jsfGetJSON(v, str, JSON_NONE);
    } else {
      jsExceptionHere(JSET_INTERNALERROR, "Variable type cannot be converted to string");
    }
  }
  return str;
}

JsVar *jsvAsStringAndUnLock(JsVar *var) {
  JsVar *s = jsvAsString(var);
  jsvUnLock(var);
  return s;
}

JsVar *jsvAsFlatString(JsVar *var) {
  if (jsvIsFlatString(var)) return jsvLockAgain(var);
  JsVar *str = jsvAsString(var);
  size_t len = jsvGetStringLength(str);
  JsVar *flat = jsvNewFlatStringOfLength((unsigned int)len);
  if (flat) {
    JsvStringIterator src;
    JsvStringIterator dst;
    jsvStringIteratorNew(&src, str, 0);
    jsvStringIteratorNew(&dst, flat, 0);
    while (len--) {
      jsvStringIteratorSetCharAndNext(&dst, jsvStringIteratorGetCharAndNext(&src));
    }
    jsvStringIteratorFree(&src);
    jsvStringIteratorFree(&dst);
  }
  jsvUnLock(str);
  return flat;
}

/** Given a JsVar meant to be an index to an array, convert it to
 * the actual variable type we'll use to access the array. For example
 * a["0"] is actually translated to a[0]
 */
JsVar *jsvAsArrayIndex(JsVar *index) {
  if (jsvIsSimpleInt(index) && jsvGetInteger(index)>=0) {
    return jsvLockAgain(index); // we're ok!
  } else if (jsvIsString(index)) {
    /* Index filtering (bug #19) - if we have an array index A that is:
     is_string(A) && int_to_string(string_to_int(A)) == A
     then convert it to an integer. Shouldn't be too nasty for performance
     as we only do this when accessing an array with a string */
    if (jsvIsStringNumericStrict(index)) {
      JsVar *i = jsvNewFromInteger(jsvGetInteger(index));
      JsVar *is = jsvAsString(i);
      if (jsvCompareString(index,is,0,0,false)==0) {
        // two items are identical - use the integer
        jsvUnLock(is);
        return i;
      } else {
        // not identical, use as a string
        jsvUnLock2(i,is);
      }
    }
  } else if (jsvIsFloat(index)) {
    // if it's a float that is actually integral, return an integer...
    JsVarFloat v = jsvGetFloat(index);
    JsVarInt vi = jsvGetInteger(index);
    if (v == vi) return jsvNewFromInteger(vi);
  }

  // else if it's not a simple numeric type, convert it to a string
  return jsvAsString(index);
}

/** Same as jsvAsArrayIndex, but ensures that 'index' is unlocked */
JsVar *jsvAsArrayIndexAndUnLock(JsVar *a) {
  JsVar *b = jsvAsArrayIndex(a);
  jsvUnLock(a);
  return b;
}

/// Returns true if the string is empty - faster than jsvGetStringLength(v)==0
bool jsvIsEmptyString(JsVar *v) {
  if (!jsvHasCharacterData(v)) return true;
  return jsvGetCharactersInVar(v)==0;
}

size_t jsvGetStringLength(const JsVar *v) {
  size_t strLength = 0;
  const JsVar *var = v;
  JsVar *newVar = 0;
  if (!jsvHasCharacterData(v)) return 0;

  while (var) {
    JsVarRef ref = jsvGetLastChild(var);
    strLength += jsvGetCharactersInVar(var);

    // Go to next
    jsvUnLock(newVar); // note use of if (ref), not var
    var = newVar = jsvLockSafe(ref);
  }
  jsvUnLock(newVar); // note use of if (ref), not var
  return strLength;
}

size_t jsvGetFlatStringBlocks(const JsVar *v) {
  assert(jsvIsFlatString(v));
  return ((size_t)v->varData.integer+sizeof(JsVar)-1) / sizeof(JsVar);
}

char *jsvGetFlatStringPointer(JsVar *v) {
  assert(jsvIsFlatString(v));
  if (!jsvIsFlatString(v)) return 0;
  return (char*)(v+1); // pointer to the next JsVar
}

JsVar *jsvGetFlatStringFromPointer(char *v) {
  JsVar *secondVar = (JsVar*)v;
  JsVar *flatStr = secondVar-1;
  assert(jsvIsFlatString(flatStr));
  return flatStr;
}

/// If the variable points to a *flat* area of memory, return a pointer (and set length). Otherwise return 0.
char *jsvGetDataPointer(JsVar *v, size_t *len) {
  assert(len);
  if (jsvIsArrayBuffer(v)) {
    /* Arraybuffers generally use some kind of string to store their data.
     * Find it, then call ourselves again to figure out if we can get a
     * raw pointer to it.  */
    JsVar *d = jsvGetArrayBufferBackingString(v, NULL);
    char *r = jsvGetDataPointer(d, len);
    jsvUnLock(d);
    if (r) {
      r += v->varData.arraybuffer.byteOffset;
      *len = v->varData.arraybuffer.length;
    }
    return r;
  }
  if (jsvIsNativeString(v)) {
    *len = v->varData.nativeStr.len;
    return (char*)v->varData.nativeStr.ptr;
  }
  if (jsvIsFlatString(v)) {
    *len = jsvGetStringLength(v);
    return jsvGetFlatStringPointer(v);
  }
  if (jsvIsBasicString(v) && !jsvGetLastChild(v)) {
    // It's a normal string but is small enough to have all the data in
    *len = jsvGetCharactersInVar(v);
    return (char*)v->varData.str;
  }
  return 0;
}

//  IN A STRING  get the number of lines in the string (min=1)
size_t jsvGetLinesInString(JsVar *v) {
  size_t lines = 1;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, 0);
  while (jsvStringIteratorHasChar(&it)) {
    if (jsvStringIteratorGetCharAndNext(&it)=='\n') lines++;
  }
  jsvStringIteratorFree(&it);
  return lines;
}

// IN A STRING Get the number of characters on a line - lines start at 1
size_t jsvGetCharsOnLine(JsVar *v, size_t line) {
  size_t currentLine = 1;
  size_t chars = 0;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, 0);
  while (jsvStringIteratorHasChar(&it)) {
    if (jsvStringIteratorGetCharAndNext(&it)=='\n') {
      currentLine++;
      if (currentLine > line) break;
    } else if (currentLine==line) chars++;
  }
  jsvStringIteratorFree(&it);
  return chars;
}

//  IN A STRING, get the 1-based line and column of the given character. Both values must be non-null
void jsvGetLineAndCol(JsVar *v, size_t charIdx, size_t *line, size_t *col) {
  size_t x = 1;
  size_t y = 1;
  size_t n = 0;
  assert(line && col);

  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, 0);
  while (jsvStringIteratorHasChar(&it)) {
    char ch = jsvStringIteratorGetCharAndNext(&it);
    if (n==charIdx) {
      jsvStringIteratorFree(&it);
      *line = y;
      *col = x;
      return;
    }
    x++;
    if (ch=='\n') {
      x=1; y++;
    }
    n++;
  }
  jsvStringIteratorFree(&it);
  // uh-oh - not found
  *line = y;
  *col = x;
}

//  IN A STRING, get a character index from a line and column
size_t jsvGetIndexFromLineAndCol(JsVar *v, size_t line, size_t col) {
  size_t x = 1;
  size_t y = 1;
  size_t n = 0;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, 0);
  while (jsvStringIteratorHasChar(&it)) {
    char ch = jsvStringIteratorGetCharAndNext(&it);
    if ((y==line && x>=col) || y>line) {
      jsvStringIteratorFree(&it);
      return (y>line) ? (n-1) : n;
    }
    x++;
    if (ch=='\n') {
      x=1; y++;
    }
    n++;
  }
  jsvStringIteratorFree(&it);
  return n;
}

void jsvAppendString(JsVar *var, const char *str) {
  assert(jsvIsString(var));
  JsvStringIterator dst;
  jsvStringIteratorNew(&dst, var, 0);
  jsvStringIteratorGotoEnd(&dst);
  // now start appending
  /* This isn't as fast as something single-purpose, but it's not that bad,
   * and is less likely to break :) */
  while (*str)
    jsvStringIteratorAppend(&dst, *(str++));
  jsvStringIteratorFree(&dst);
}

// Append the given string to this one - but does not use null-terminated strings
void jsvAppendStringBuf(JsVar *var, const char *str, size_t length) {
  assert(jsvIsString(var));
  JsvStringIterator dst;
  jsvStringIteratorNew(&dst, var, 0);
  jsvStringIteratorGotoEnd(&dst);
  // now start appending
  /* This isn't as fast as something single-purpose, but it's not that bad,
   * and is less likely to break :) */
  while (length) {
    jsvStringIteratorAppend(&dst, *(str++));
    length--;
  }
  jsvStringIteratorFree(&dst);
}

/// Special version of append designed for use with vcbprintf_callback (See jsvAppendPrintf)
void jsvStringIteratorPrintfCallback(const char *str, void *user_data) {
  while (*str)
    jsvStringIteratorAppend((JsvStringIterator *)user_data, *(str++));
}

void jsvAppendPrintf(JsVar *var, const char *fmt, ...) {
  JsvStringIterator it;
  jsvStringIteratorNew(&it, var, 0);
  jsvStringIteratorGotoEnd(&it);

  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsvStringIteratorPrintfCallback,&it, fmt, argp);
  va_end(argp);

  jsvStringIteratorFree(&it);
}

JsVar *jsvVarPrintf( const char *fmt, ...) {
  JsVar *str = jsvNewFromEmptyString();
  if (!str) return 0;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, str, 0);
  jsvStringIteratorGotoEnd(&it);

  va_list argp;
  va_start(argp, fmt);
  vcbprintf((vcbprintf_callback)jsvStringIteratorPrintfCallback,&it, fmt, argp);
  va_end(argp);

  jsvStringIteratorFree(&it);
  return str;
}

/** Append str to var. Both must be strings. stridx = start char or str, maxLength = max number of characters (can be JSVAPPENDSTRINGVAR_MAXLENGTH) */
void jsvAppendStringVar(JsVar *var, const JsVar *str, size_t stridx, size_t maxLength) {
  assert(jsvIsString(var));

  JsvStringIterator dst;
  jsvStringIteratorNew(&dst, var, 0);
  jsvStringIteratorGotoEnd(&dst);
  // now start appending
  /* This isn't as fast as something single-purpose, but it's not that bad,
     * and is less likely to break :) */
  JsvStringIterator it;
  jsvStringIteratorNewConst(&it, str, stridx);
  while (jsvStringIteratorHasChar(&it) && (maxLength-->0)) {
    char ch = jsvStringIteratorGetCharAndNext(&it);
    jsvStringIteratorAppend(&dst, ch);
  }
  jsvStringIteratorFree(&it);
  jsvStringIteratorFree(&dst);
}

/** Create a new variable from a substring. argument must be a string. stridx = start char or str, maxLength = max number of characters (can be JSVAPPENDSTRINGVAR_MAXLENGTH) */
JsVar *jsvNewFromStringVar(const JsVar *str, size_t stridx, size_t maxLength) {
  if (jsvIsNativeString(str) || jsvIsFlashString(str)) {
    // if it's a flash string, just change the pointer (but we must check length)
    size_t l = jsvGetStringLength(str);
    if (stridx>l) stridx=l;
    if (stridx+maxLength>l) maxLength=l-stridx;
    JsVar *res = jsvNewWithFlags(str->flags&JSV_VARTYPEMASK);
    res->varData.nativeStr.ptr = str->varData.nativeStr.ptr + stridx;
    res->varData.nativeStr.len = (JsVarDataNativeStrLength)maxLength;
    return res;
  }
  JsVar *var = jsvNewFromEmptyString();
  if (var) jsvAppendStringVar(var, str, stridx, maxLength);
  return var;
}

/** Append all of str to var. Both must be strings.  */
void jsvAppendStringVarComplete(JsVar *var, const JsVar *str) {
  jsvAppendStringVar(var, str, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
}

char jsvGetCharInString(JsVar *v, size_t idx) {
  if (!jsvIsString(v)) return 0;

  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, idx);
  char ch = jsvStringIteratorGetChar(&it);
  jsvStringIteratorFree(&it);
  return ch;
}

void jsvSetCharInString(JsVar *v, size_t idx, char ch, bool bitwiseOR) {
  if (!jsvIsString(v)) return;
  JsvStringIterator it;
  jsvStringIteratorNew(&it, v, idx);
  if (bitwiseOR) ch |= jsvStringIteratorGetChar(&it);
  jsvStringIteratorSetChar(&it, ch);
  jsvStringIteratorFree(&it);
}

/// Get the index of a character in a string, or -1
int jsvGetStringIndexOf(JsVar *str, char ch) {
  JsvStringIterator it;
  jsvStringIteratorNew(&it, str, 0);
  while (jsvStringIteratorHasChar(&it)) {
    if (jsvStringIteratorGetChar(&it) == ch) {
      int idx = (int)jsvStringIteratorGetIndex(&it);
      jsvStringIteratorFree(&it);
      return idx;
    };
    jsvStringIteratorNext(&it);
  }
  jsvStringIteratorFree(&it);
  return -1;
}

/** Does this string contain only Numeric characters (with optional '-'/'+' at the front)? NOT '.'/'e' and similar (allowDecimalPoint is for '.' only) */
bool jsvIsStringNumericInt(const JsVar *var, bool allowDecimalPoint) {
  assert(jsvIsString(var));
  JsvStringIterator it;
  jsvStringIteratorNewConst(&it, var, 0); // we know it's non const

  // skip whitespace
  while (jsvStringIteratorHasChar(&it) && isWhitespace(jsvStringIteratorGetChar(&it)))
    jsvStringIteratorNext(&it);

  // skip a minus. if there was one
  if (jsvStringIteratorGetChar(&it)=='-' || jsvStringIteratorGetChar(&it)=='+')
    jsvStringIteratorNext(&it);

  int radix = 0;
  if (jsvStringIteratorGetChar(&it)=='0') {
    jsvStringIteratorNext(&it);
    char buf[3];
    buf[0] = '0';
    buf[1] = jsvStringIteratorGetChar(&it);
    buf[2] = 0;
    const char *p = buf;
    radix = getRadix(&p,0);
    if (p>&buf[1]) jsvStringIteratorNext(&it);
  }
  if (radix==0) radix=10;

  // now check...
  int chars=0;
  while (jsvStringIteratorHasChar(&it)) {
    chars++;
    char ch = jsvStringIteratorGetCharAndNext(&it);
    if (ch=='.' && allowDecimalPoint) {
      allowDecimalPoint = false; // there can be only one
    } else {
      int n = chtod(ch);
      if (n<0 || n>=radix) {
        jsvStringIteratorFree(&it);
        return false;
      }
    }
  }
  jsvStringIteratorFree(&it);
  return chars>0;
}

/** Does this string contain only Numeric characters? This is for arrays
 * and makes the assertion that int_to_string(string_to_int(var))==var */
bool jsvIsStringNumericStrict(const JsVar *var) {
  assert(jsvIsString(var));
  JsvStringIterator it;
  jsvStringIteratorNewConst(&it, var, 0);  // we know it's non const
  bool hadNonZero = false;
  bool hasLeadingZero = false;
  int chars = 0;
  while (jsvStringIteratorHasChar(&it)) {
    chars++;
    char ch = jsvStringIteratorGetCharAndNext(&it);
    if (!isNumeric(ch)) {
      // test for leading zero ensures int_to_string(string_to_int(var))==var
      jsvStringIteratorFree(&it);
      return false;
    }
    if (!hadNonZero && ch=='0') hasLeadingZero=true;
    if (ch!='0') hadNonZero=true;
  }
  jsvStringIteratorFree(&it);
  return chars>0 && (!hasLeadingZero || chars==1);
}


JsVarInt jsvGetInteger(const JsVar *v) {
  if (!v) return 0; // undefined
  /* strtol understands about hex and octal */
  if (jsvIsNull(v)) return 0;
  if (jsvIsUndefined(v)) return 0;
  if (jsvIsIntegerish(v) || jsvIsArrayBufferName(v)) return v->varData.integer;
  if (jsvIsArray(v) || jsvIsArrayBuffer(v)) {
    JsVarInt l = jsvGetLength((JsVar *)v);
    if (l==0) return 0; // 0 length, return 0
    if (l==1) {
      if (jsvIsArrayBuffer(v))
        return jsvGetIntegerAndUnLock(jsvArrayBufferGet((JsVar*)v,0));
      return jsvGetIntegerAndUnLock(jsvSkipNameAndUnLock(jsvGetArrayItem(v,0)));
    }
  }
  if (jsvIsFloat(v)) {
    if (isfinite(v->varData.floating))
      return (JsVarInt)(long long)v->varData.floating;
    return 0;
  }
  if (jsvIsString(v) && jsvIsStringNumericInt(v, true/* allow decimal point*/)) {
    char buf[32];
    if (jsvGetString(v, buf, sizeof(buf))==sizeof(buf))
      jsExceptionHere(JSET_ERROR, "String too big to convert to integer\n");
    else
      return (JsVarInt)stringToInt(buf);
  }
  return 0;
}

long long jsvGetLongInteger(const JsVar *v) {
  if (jsvIsInt(v)) return jsvGetInteger(v);
  return (long long)jsvGetFloat(v);
}

long long jsvGetLongIntegerAndUnLock(JsVar *v) {
  long long i = jsvGetLongInteger(v);
  jsvUnLock(v);
  return i;
}


void jsvSetInteger(JsVar *v, JsVarInt value) {
  assert(jsvIsInt(v));
  v->varData.integer  = value;
}

/**
 * Get the boolean value of a variable.
 * From a JavaScript variable, we determine its boolean value.  The rules
 * are:
 *
 * * If integer, true if value is not 0.
 * * If float, true if value is not 0.0.
 * * If function, array or object, always true.
 * * If string, true if length of string is greater than 0.
 */
bool jsvGetBool(const JsVar *v) {
  if (jsvIsString(v))
    return jsvGetStringLength((JsVar*)v)!=0;
  if (jsvIsPin(v))
    return jshIsPinValid(jshGetPinFromVar((JsVar*)v));
  if (jsvIsFunction(v) || jsvIsArray(v) || jsvIsObject(v) || jsvIsArrayBuffer(v))
    return true;
  if (jsvIsFloat(v)) {
    JsVarFloat f = jsvGetFloat(v);
    return !isnan(f) && f!=0.0;
  }
  return jsvGetInteger(v)!=0;
}

JsVarFloat jsvGetFloat(const JsVar *v) {
  if (!v) return NAN; // undefined
  if (jsvIsFloat(v)) return v->varData.floating;
  if (jsvIsIntegerish(v)) return (JsVarFloat)v->varData.integer;
  if (jsvIsArray(v) || jsvIsArrayBuffer(v)) {
    JsVarInt l = jsvGetLength(v);
    if (l==0) return 0; // zero element array==0 (not undefined)
    if (l==1) {
      if (jsvIsArrayBuffer(v))
        return jsvGetFloatAndUnLock(jsvArrayBufferGet((JsVar*)v,0));
      return jsvGetFloatAndUnLock(jsvSkipNameAndUnLock(jsvGetArrayItem(v,0)));
    }
  }
  if (jsvIsString(v)) {
    char buf[64];
    if (jsvGetString(v, buf, sizeof(buf))==sizeof(buf)) {
      jsExceptionHere(JSET_ERROR, "String too big to convert to float\n");
    } else {
      if (buf[0]==0) return 0; // empty string -> 0
      if (!strcmp(buf,"Infinity")) return INFINITY;
      if (!strcmp(buf,"-Infinity")) return -INFINITY;
      return stringToFloat(buf);
    }
  }
  return NAN;
}

/// Convert the given variable to a number
JsVar *jsvAsNumber(JsVar *var) {
  // stuff that we can just keep
  if (jsvIsInt(var) || jsvIsFloat(var)) return jsvLockAgain(var);
  // stuff that can be converted to an int
  if (jsvIsBoolean(var) ||
      jsvIsPin(var) ||
      jsvIsNull(var) ||
      jsvIsBoolean(var) ||
      jsvIsArrayBufferName(var))
    return jsvNewFromInteger(jsvGetInteger(var));
  if (jsvIsString(var) && (jsvIsEmptyString(var) || jsvIsStringNumericInt(var, false/* no decimal pt - handle that with GetFloat */))) {
    // handle strings like this, in case they're too big for an int
    char buf[64];
    if (jsvGetString(var, buf, sizeof(buf))==sizeof(buf)) {
      jsExceptionHere(JSET_ERROR, "String too big to convert to integer\n");
      return jsvNewFromFloat(NAN);
    } else
      return jsvNewFromLongInteger(stringToInt(buf));
  }
  // Else just try and get a float
  return jsvNewFromFloat(jsvGetFloat(var));
}

JsVar *jsvAsNumberAndUnLock(JsVar *v) { JsVar *n = jsvAsNumber(v); jsvUnLock(v); return n; }
JsVarInt jsvGetIntegerAndUnLock(JsVar *v) { return _jsvGetIntegerAndUnLock(v); }
JsVarFloat jsvGetFloatAndUnLock(JsVar *v) { return _jsvGetFloatAndUnLock(v); }
bool jsvGetBoolAndUnLock(JsVar *v) { return _jsvGetBoolAndUnLock(v); }


#ifndef ESPR_NO_GET_SET
// Executes the given getter, or if there are problems returns undefined
JsVar *jsvExecuteGetter(JsVar *parent, JsVar *getset) {
  assert(jsvIsGetterOrSetter(getset));
  if (!jsvIsGetterOrSetter(getset)) return 0; // wasn't an object?
  JsVar *fn = jsvObjectGetChild(getset, "get", 0);
  if (!jsvIsFunction(fn)) {
    jsvUnLock(fn);
    return 0;
  }
  JsVar *result = jspExecuteFunction(fn, parent, 0, NULL);
  jsvUnLock(fn);
  return result;
}

// Executes the given setter
void jsvExecuteSetter(JsVar *parent, JsVar *getset, JsVar *value) {
  assert(jsvIsGetterOrSetter(getset));
  if (!jsvIsGetterOrSetter(getset)) return; // wasn't an object?
  JsVar *fn = jsvObjectGetChild(getset, "set", 0);
  if (!jsvIsFunction(fn)) {
    jsvUnLock(fn);
    return;
  }
  if (!fn) return;
  jsvUnLock2(jspExecuteFunction(fn, parent, 1, &value), fn);
}

/// Add a named getter or setter to an object
void jsvAddGetterOrSetter(JsVar *obj, JsVar *varName, bool isGetter, JsVar *method) {
  // check for existing getter/setter, make one if needed
  JsVar *getsetName = jsvFindChildFromVar(obj, varName, true);
  if (jsvIsName(getsetName)) {
    JsVar *getset = jsvGetValueOfName(getsetName);
    if (!jsvIsGetterOrSetter(getset)) {
      jsvUnLock(getset);
      getset = jsvNewWithFlags(JSV_GET_SET);
      jsvSetValueOfName(getsetName, getset);
    }
    if (jsvIsGetterOrSetter(getset))
      jsvObjectSetChild(getset, isGetter?"get":"set", method);
    jsvUnLock(getset);
  }
  jsvUnLock(getsetName);
}
#endif


/* Set the value of the given variable. This is sort of like
 * jsvSetValueOfName except it deals with all the non-standard
 * stuff like ArrayBuffers, variables that haven't been allocated
 * yet, setters, etc.
 */
void jsvReplaceWith(JsVar *dst, JsVar *src) {
  // If this is an index in an array buffer, write directly into the array buffer
  if (jsvIsArrayBufferName(dst)) {
    size_t idx = (size_t)jsvGetInteger(dst);
    JsVar *arrayBuffer = jsvLock(jsvGetFirstChild(dst));
    jsvArrayBufferSet(arrayBuffer, idx, src);
    jsvUnLock(arrayBuffer);
    return;
  }
  // if destination isn't there, isn't a 'name', or is used, give an error
  if (!jsvIsName(dst)) {
    jsExceptionHere(JSET_ERROR, "Unable to assign value to non-reference %t", dst);
    return;
  }
  if (jsvIsConstant(dst)) {
    jsExceptionHere(JSET_TYPEERROR, "Assignment to a constant");
    return;
  }
#ifndef ESPR_NO_GET_SET
  JsVar *v = jsvGetValueOfName(dst);
  if (jsvIsGetterOrSetter(v)) {
    JsVar *parent = jsvIsNewChild(dst)?jsvLock(jsvGetNextSibling(dst)):0;
    jsvExecuteSetter(parent,v,src);
    jsvUnLock2(v,parent);
    return;
  }
  jsvUnLock(v);
#endif
  jsvSetValueOfName(dst, src);
  /* If dst is flagged as a new child, it means that
   * it was previously undefined, and we need to add it to
   * the given object when it is set.
   */
  if (jsvIsNewChild(dst)) {
    // Get what it should have been a child of
    JsVar *parent = jsvLock(jsvGetNextSibling(dst));
    if (!jsvIsString(parent)) {
      // if we can't find a char in a string we still return a NewChild,
      // but we can't add character back in
      if (!jsvHasChildren(parent)) {
        jsExceptionHere(JSET_ERROR, "Field or method \"%v\" does not already exist, and can't create it on %t", dst, parent);
      } else {
        // Remove the 'new child' flagging
        jsvUnRef(parent);
        jsvSetNextSibling(dst, 0);
        jsvUnRef(parent);
        jsvSetPrevSibling(dst, 0);
        // Add to the parent
        jsvAddName(parent, dst);
      }
    }
    jsvUnLock(parent);
  }
}

/* See jsvReplaceWith - this does the same but will
 * shove the variable in execInfo.root if it hasn't
 * been defined yet */
void jsvReplaceWithOrAddToRoot(JsVar *dst, JsVar *src) {
  /* If we're assigning to this and we don't have a parent,
   * add it to the symbol table root */
  if (!jsvGetRefs(dst) && jsvIsName(dst)) {
    if (!jsvIsArrayBufferName(dst) && !jsvIsNewChild(dst))
      jsvAddName(execInfo.root, dst);
  }
  jsvReplaceWith(dst, src);
}

/** Get the item at the given location in the array buffer and return the result */
size_t jsvGetArrayBufferLength(const JsVar *arrayBuffer) {
  assert(jsvIsArrayBuffer(arrayBuffer));
  return arrayBuffer->varData.arraybuffer.length;
}

/** Get the String the contains the data for this arrayBuffer. Is ok with being passed a String in the first place. Offset is the offset in the backing string of this arraybuffer. */
JsVar *jsvGetArrayBufferBackingString(JsVar *arrayBuffer, uint32_t *offset) {
  jsvLockAgain(arrayBuffer);
  if (offset) *offset = 0;
  while (jsvIsArrayBuffer(arrayBuffer)) {
    if (offset) *offset += arrayBuffer->varData.arraybuffer.byteOffset;
    JsVar *s = jsvLock(jsvGetFirstChild(arrayBuffer));
    jsvUnLock(arrayBuffer);
    arrayBuffer = s;
  }
  assert(jsvIsString(arrayBuffer));
  return arrayBuffer;
}

/** Get the item at the given location in the array buffer and return the result */
JsVar *jsvArrayBufferGet(JsVar *arrayBuffer, size_t idx) {
  JsvArrayBufferIterator it;
  jsvArrayBufferIteratorNew(&it, arrayBuffer, idx);
  JsVar *v = jsvArrayBufferIteratorGetValue(&it);
  jsvArrayBufferIteratorFree(&it);
  return v;
}

/** Set the item at the given location in the array buffer */
void jsvArrayBufferSet(JsVar *arrayBuffer, size_t idx, JsVar *value) {
  JsvArrayBufferIterator it;
  jsvArrayBufferIteratorNew(&it, arrayBuffer, idx);
  jsvArrayBufferIteratorSetValue(&it, value);
  jsvArrayBufferIteratorFree(&it);
}


/** Given an integer name that points to an arraybuffer or an arraybufferview, evaluate it and return the result */
JsVar *jsvArrayBufferGetFromName(JsVar *name) {
  assert(jsvIsArrayBufferName(name));
  size_t idx = (size_t)jsvGetInteger(name);
  JsVar *arrayBuffer = jsvLock(jsvGetFirstChild(name));
  JsVar *value = jsvArrayBufferGet(arrayBuffer, idx);
  jsvUnLock(arrayBuffer);
  return value;
}


JsVar *jsvGetFunctionArgumentLength(JsVar *functionScope) {
  JsVar *args = jsvNewEmptyArray();
  if (!args) return 0; // out of memory

  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, functionScope);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *idx = jsvObjectIteratorGetKey(&it);
    if (jsvIsFunctionParameter(idx)) {
      JsVar *val = jsvSkipOneName(idx);
      jsvArrayPushAndUnLock(args, val);
    }
    jsvUnLock(idx);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);

  return args;
}

/** Is this variable actually defined? eg, can we pass it into `jsvSkipName`
 * without getting a ReferenceError? This also returns false if the variable
 * if ok, but has the value `undefined`. */
bool jsvIsVariableDefined(JsVar *a) {
  return !jsvIsName(a) ||
         jsvIsNameWithValue(a) ||
         (jsvGetFirstChild(a)!=0);
}

/* If this is a simple name (that links to another var) the
 * return that var, else 0. Unlike jsvSkipName this doesn't
 * repeatedly get the name, or evaluate getters. */
JsVar *jsvGetValueOfName(JsVar *a) {
  if (!a) return 0;
  if (jsvIsArrayBufferName(a)) return jsvArrayBufferGetFromName(a);
  if (jsvIsNameInt(a)) return jsvNewFromInteger((JsVarInt)jsvGetFirstChildSigned(a));
  if (jsvIsNameIntBool(a)) return jsvNewFromBool(jsvGetFirstChild(a)!=0);
  assert(!jsvIsNameWithValue(a));
  if (jsvIsName(a))
    return jsvLockSafe(jsvGetFirstChild(a));
  return 0;
}

/* Check for and trigger a ReferenceError on a variable if it's a name that doesn't exist */
void jsvCheckReferenceError(JsVar *a) {
  if (jsvIsBasicName(a) && jsvGetRefs(a)==0 && !jsvIsNewChild(a) && !jsvGetFirstChild(a))
    jsExceptionHere(JSET_REFERENCEERROR, "%q is not defined", a);
}



/** If a is a name skip it and go to what it points to - and so on (if repeat=true).
 * ALWAYS locks - so must unlock what it returns. It MAY
 * return 0. Throws a ReferenceError if variable is not defined,
 * but you can check if it will with jsvIsReferenceError.
 * If a 'getter' needs to be executed, 'parent' is the object that
 * gets used unless a NewChild overwrites it */
JsVar *jsvSkipNameWithParent(JsVar *a, bool repeat, JsVar *parent) {
  if (!a) return 0;
  if (jsvIsArrayBufferName(a)) return jsvArrayBufferGetFromName(a);
  if (jsvIsNameInt(a)) return jsvNewFromInteger((JsVarInt)jsvGetFirstChildSigned(a));
  if (jsvIsNameIntBool(a)) return jsvNewFromBool(jsvGetFirstChild(a)!=0);
  JsVar *pa = jsvLockAgain(a);
  while (jsvIsName(pa)) {
    JsVarRef n = jsvGetFirstChild(pa);
    jsvUnLock(pa);
    if (!n) {
      // check here as it's less likely we get here (=faster)
      if (pa==a) jsvCheckReferenceError(a);
      return 0;
    }
    pa = jsvLock(n);
    assert(pa!=a);
    if (!repeat) break;
  }
#ifndef SAVE_ON_FLASH
  if (jsvIsGetterOrSetter(pa)) {
    JsVar *getterParent = jsvIsNewChild(a)?jsvLock(jsvGetNextSibling(a)):0;
    JsVar *v = jsvExecuteGetter(getterParent?getterParent:parent, pa);
    jsvUnLock2(getterParent,pa);
    pa = v;
  }
#endif
  return pa;
}

/** If a is a name skip it and go to what it points to - and so on
 * ALWAYS locks - so must unlock what it returns. It MAY
 * return 0. Throws a ReferenceError if variable is not defined,
 * but you can check if it will with jsvIsReferenceError */
JsVar *jsvSkipName(JsVar *a) {
  return jsvSkipNameWithParent(a, true, 0);
}

/** If a is a name skip it and go to what it points to.
 * ALWAYS locks - so must unlock what it returns. It MAY
 * return 0. Throws a ReferenceError if variable is not defined,
 * but you can check if it will with jsvIsReferenceError */
JsVar *jsvSkipOneName(JsVar *a) {
  return jsvSkipNameWithParent(a, false, 0);
}

/** If a is a's child is a name skip it and go to what it points to.
 * ALWAYS locks - so must unlock what it returns.  */
JsVar *jsvSkipToLastName(JsVar *a) {
  assert(jsvIsName(a));
  a = jsvLockAgain(a);
  while (true) {
    if (!jsvGetFirstChild(a)) return a;
    JsVar *child = jsvLock(jsvGetFirstChild(a));
    if (jsvIsName(child)) {
      jsvUnLock(a);
      a = child;
    } else {
      jsvUnLock(child);
      return a;
    }
  }
  return 0; // not called
}

/** Same as jsvSkipName, but ensures that 'a' is unlocked */
JsVar *jsvSkipNameAndUnLock(JsVar *a) {
  JsVar *b = jsvSkipName(a);
  jsvUnLock(a);
  return b;
}

/** Same as jsvSkipOneName, but ensures that 'a' is unlocked */
JsVar *jsvSkipOneNameAndUnLock(JsVar *a) {
  JsVar *b = jsvSkipOneName(a);
  jsvUnLock(a);
  return b;
}

bool jsvIsStringEqualOrStartsWithOffset(JsVar *var, const char *str, bool isStartsWith, size_t startIdx, bool ignoreCase) {
  if (!jsvHasCharacterData(var)) {
    return 0; // not a string so not equal!
  }

  JsvStringIterator it;
  jsvStringIteratorNew(&it, var, startIdx);
  if (ignoreCase) {
      while (jsvStringIteratorHasChar(&it) && *str &&
          charToLowerCase(jsvStringIteratorGetChar(&it)) == charToLowerCase(*str)) {
        str++;
        jsvStringIteratorNext(&it);
      }
  } else {
      while (jsvStringIteratorHasChar(&it) && *str &&
             jsvStringIteratorGetChar(&it) == *str) {
        str++;
        jsvStringIteratorNext(&it);
      }
  }
  bool eq = (isStartsWith && !*str) ||
            jsvStringIteratorGetChar(&it)==*str; // should both be 0 if equal
  jsvStringIteratorFree(&it);
  return eq;
}

/*
jsvIsStringEqualOrStartsWith(A, B, false) is a proper A==B
jsvIsStringEqualOrStartsWith(A, B, true) is A.startsWith(B)
 */
bool jsvIsStringEqualOrStartsWith(JsVar *var, const char *str, bool isStartsWith) {
  return jsvIsStringEqualOrStartsWithOffset(var, str, isStartsWith, 0, false);
}

// Also see jsvIsBasicVarEqual
bool jsvIsStringEqual(JsVar *var, const char *str) {
  return jsvIsStringEqualOrStartsWith(var, str, false);
}

// Also see jsvIsBasicVarEqual
bool jsvIsStringIEqualAndUnLock(JsVar *var, const char *str) {
  bool b = jsvIsStringEqualOrStartsWithOffset(var, str, false, 0, true);
  jsvUnLock(var);
  return b;
}


/** Compare 2 strings, starting from the given character positions. equalAtEndOfString means that
 * if one of the strings ends (even if the other hasn't), we treat them as equal.
 * For a basic strcmp, do: jsvCompareString(a,b,0,0,false)
 *  */
int jsvCompareString(JsVar *va, JsVar *vb, size_t starta, size_t startb, bool equalAtEndOfString) {
  JsvStringIterator ita, itb;
  jsvStringIteratorNew(&ita, va, starta);
  jsvStringIteratorNew(&itb, vb, startb);
  // step to first positions
  while (true) {
    int ca = jsvStringIteratorGetCharOrMinusOne(&ita);
    int cb = jsvStringIteratorGetCharOrMinusOne(&itb);

    if (ca != cb) {
      jsvStringIteratorFree(&ita);
      jsvStringIteratorFree(&itb);
      if ((ca<0 || cb<0) && equalAtEndOfString) return 0;
      return ca - cb;
    }
    if (ca < 0) { // both equal, but end of string
      jsvStringIteratorFree(&ita);
      jsvStringIteratorFree(&itb);
      return 0;
    }
    jsvStringIteratorNext(&ita);
    jsvStringIteratorNext(&itb);
  }
  // never get here, but the compiler warns...
  return true;
}

/** Return a new string containing just the characters that are
 * shared between two strings. */
JsVar *jsvGetCommonCharacters(JsVar *va, JsVar *vb) {
  JsVar *v = jsvNewFromEmptyString();
  if (!v) return 0;
  JsvStringIterator ita, itb;
  jsvStringIteratorNew(&ita, va, 0);
  jsvStringIteratorNew(&itb, vb, 0);
  int ca = jsvStringIteratorGetCharOrMinusOne(&ita);
  int cb = jsvStringIteratorGetCharOrMinusOne(&itb);
  while (ca>0 && cb>0 && ca == cb) {
    jsvAppendCharacter(v, (char)ca);
    jsvStringIteratorNext(&ita);
    jsvStringIteratorNext(&itb);
    ca = jsvStringIteratorGetCharOrMinusOne(&ita);
    cb = jsvStringIteratorGetCharOrMinusOne(&itb);
  }
  jsvStringIteratorFree(&ita);
  jsvStringIteratorFree(&itb);
  return v;
}


/** Compare 2 integers, >0 if va>vb,  <0 if va<vb. If compared with a non-integer, that gets put later */
int jsvCompareInteger(JsVar *va, JsVar *vb) {
  if (jsvIsInt(va) && jsvIsInt(vb))
    return (int)(jsvGetInteger(va) - jsvGetInteger(vb));
  else if (jsvIsInt(va))
    return -1;
  else if (jsvIsInt(vb))
    return 1;
  else
    return 0;
}

/** Copy only a name, not what it points to. ALTHOUGH the link to what it points to is maintained unless linkChildren=false
    If keepAsName==false, this will be converted into a normal variable */
JsVar *jsvCopyNameOnly(JsVar *src, bool linkChildren, bool keepAsName) {
  assert(jsvIsName(src));
  JsVarFlags flags = src->flags;
  JsVar *dst = 0;
  if (!keepAsName) {
    JsVarFlags t = src->flags & JSV_VARTYPEMASK;
    if (t>=_JSV_NAME_INT_START && t<=_JSV_NAME_INT_END) {
      flags = (flags & ~JSV_VARTYPEMASK) | JSV_INTEGER;
    } else {
      assert((JSV_NAME_STRING_INT_0 < JSV_NAME_STRING_0) &&
          (JSV_NAME_STRING_0 < JSV_STRING_0) &&
          (JSV_STRING_0 < JSV_STRING_EXT_0)); // this relies on ordering
      assert(t>=JSV_NAME_STRING_INT_0 && t<=JSV_NAME_STRING_MAX);
      if (jsvGetLastChild(src)) {
        /* it's not a simple name string - it has STRING_EXT bits on the end.
         * Because the max length of NAME and STRING is different we must just
         * copy */
        dst = jsvNewFromStringVar(src, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
        if (!dst) return 0;
      } else {
        flags = (flags & (JsVarFlags)~JSV_VARTYPEMASK) | (JSV_STRING_0 + jsvGetCharactersInVar(src));
      }
    }
  }
  if (!dst) {
    dst = jsvNewWithFlags(flags & JSV_VARIABLEINFOMASK);
    if (!dst) return 0; // out of memory

    memcpy(&dst->varData, &src->varData, JSVAR_DATA_STRING_NAME_LEN);

    assert(jsvGetLastChild(dst) == 0);
    assert(jsvGetFirstChild(dst) == 0);
    assert(jsvGetPrevSibling(dst) == 0);
    assert(jsvGetNextSibling(dst) == 0);
    // Copy extra string data if there was any
    if (jsvHasStringExt(src)) {
      // If it had extra string data it should have been handled above
      assert(keepAsName || !jsvGetLastChild(src));
      // copy extra bits of string if there were any
      if (jsvGetLastChild(src)) {
        JsVar *child = jsvLock(jsvGetLastChild(src));
        JsVar *childCopy = jsvCopy(child, true);
        if (childCopy) { // could be out of memory
          jsvSetLastChild(dst, jsvGetRef(childCopy)); // no ref for stringext
          jsvUnLock(childCopy);
        }
        jsvUnLock(child);
      }
    } else {
      assert(jsvIsBasic(src)); // in case we missed something!
    }
  }
  // Copy LINK of what it points to
  if (linkChildren && jsvGetFirstChild(src)) {
    if (jsvIsNameWithValue(src))
      jsvSetFirstChild(dst, jsvGetFirstChild(src));
    else
      jsvSetFirstChild(dst, jsvRefRef(jsvGetFirstChild(src)));
  }
  return dst;
}

JsVar *jsvCopy(JsVar *src, bool copyChildren) {
  if (jsvIsFlatString(src)) {
    // Copy a Flat String into a non-flat string - it's just safer
    return jsvNewFromStringVar(src, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
  }
  JsVar *dst = jsvNewWithFlags(src->flags & JSV_VARIABLEINFOMASK);
  if (!dst) return 0; // out of memory
  if (!jsvIsStringExt(src)) {
      bool refsAsData = jsvIsBasicString(src)||jsvIsNativeString(src)||jsvIsFlashString(src)||jsvIsNativeFunction(src);
      memcpy(&dst->varData, &src->varData, refsAsData ? JSVAR_DATA_STRING_LEN : JSVAR_DATA_STRING_NAME_LEN);
      if (jsvIsNativeFunction(src)) {
        jsvSetFirstChild(dst,0);
      }
      if (!refsAsData) {
        assert(jsvGetPrevSibling(dst) == 0);
        assert(jsvGetNextSibling(dst) == 0);
        assert(jsvGetFirstChild(dst) == 0);
      }
      assert(jsvGetLastChild(dst) == 0);
  } else {
    // stringexts use the extra pointers after varData to store characters
    // see jsvGetMaxCharactersInVar
    memcpy(&dst->varData, &src->varData, JSVAR_DATA_STRING_MAX_LEN);
    assert(jsvGetLastChild(dst) == 0);
  }

  // Copy what names point to
  if (copyChildren && jsvIsName(src)) {
    if (jsvGetFirstChild(src)) {
      if (jsvIsNameWithValue(src)) {
        // name_int/etc don't need references
        jsvSetFirstChild(dst, jsvGetFirstChild(src));
      } else {
        JsVar *child = jsvLock(jsvGetFirstChild(src));
        JsVar *childCopy = jsvRef(jsvCopy(child, true));
        jsvUnLock(child);
        if (childCopy) { // could have been out of memory
          jsvSetFirstChild(dst, jsvGetRef(childCopy));
          jsvUnLock(childCopy);
        }
      }
    }
  }

  if (jsvHasStringExt(src)) {
    // copy extra bits of string if there were any
    src = jsvLockAgain(src);
    JsVar *dstChild = jsvLockAgain(dst);
    while (jsvGetLastChild(src)) {
      JsVar *child = jsvLock(jsvGetLastChild(src));
      if (jsvIsStringExt(child)) {
        JsVar *childCopy = jsvNewWithFlags(child->flags & JSV_VARIABLEINFOMASK);
        if (childCopy) {// could be out of memory
          memcpy(&childCopy->varData, &child->varData, JSVAR_DATA_STRING_MAX_LEN);
          jsvSetLastChild(dstChild, jsvGetRef(childCopy)); // no ref for stringext
        }
        jsvUnLock2(src,dstChild);
        src = child;
        dstChild = childCopy;
      } else {
        JsVar *childCopy = jsvCopy(child, true);
        if (childCopy) {// could be out of memory
          jsvSetLastChild(dstChild, jsvGetRef(childCopy)); // no ref for stringext
          jsvUnLock(childCopy);
        }
        jsvUnLock2(src, dstChild);
        return dst;
      }
    }
    jsvUnLock2(src,dstChild);
  } else if (jsvHasChildren(src)) {
    if (copyChildren) {
      // Copy children..
      JsVarRef vr;
      vr = jsvGetFirstChild(src);
      while (vr) {
        JsVar *name = jsvLock(vr);
        JsVar *child = jsvCopyNameOnly(name, true/*link children*/, true/*keep as name*/); // NO DEEP COPY!
        if (child) { // could have been out of memory
          jsvAddName(dst, child);
          jsvUnLock(child);
        }
        vr = jsvGetNextSibling(name);
        jsvUnLock(name);
      }
    }
  } else {
    assert(jsvIsBasic(src)); // in case we missed something!
  }

  return dst;
}

void jsvAddName(JsVar *parent, JsVar *namedChild) {
  namedChild = jsvRef(namedChild); // ref here VERY important as adding to structure!
  assert(jsvIsName(namedChild));

  // update array length
  if (jsvIsArray(parent) && jsvIsInt(namedChild)) {
    JsVarInt index = namedChild->varData.integer;
    if (index >= jsvGetArrayLength(parent)) {
      jsvSetArrayLength(parent, index + 1, false);
    }
  }

  if (jsvGetLastChild(parent)) { // we have children already
    JsVar *insertAfter = jsvLock(jsvGetLastChild(parent));
    if (jsvIsArray(parent)) {
      // we must insert in order - so step back until we get the right place
      while (insertAfter && jsvCompareInteger(namedChild, insertAfter)<0) {
        JsVarRef prev = jsvGetPrevSibling(insertAfter);
        jsvUnLock(insertAfter);
        insertAfter = jsvLockSafe(prev);
      }
    }

    if (insertAfter) {
      assert(jsvIsName(insertAfter));
      if (jsvGetNextSibling(insertAfter)) {
        // great, we're in the middle...
        JsVar *insertBefore = jsvLock(jsvGetNextSibling(insertAfter));
        jsvSetPrevSibling(insertBefore, jsvGetRef(namedChild));
        jsvSetNextSibling(namedChild, jsvGetRef(insertBefore));
        jsvUnLock(insertBefore);
      } else {
        // We're at the end - just set up the parent
        jsvSetLastChild(parent, jsvGetRef(namedChild));
      }
      jsvSetNextSibling(insertAfter, jsvGetRef(namedChild));
      jsvSetPrevSibling(namedChild, jsvGetRef(insertAfter));
      jsvUnLock(insertAfter);
    } else { // Insert right at the beginning of the array
      // Link 2 children together
      JsVar *firstChild = jsvLock(jsvGetFirstChild(parent));
      jsvSetPrevSibling(firstChild, jsvGetRef(namedChild));
      jsvUnLock(firstChild);

      jsvSetNextSibling(namedChild, jsvGetFirstChild(parent));
      // finally set the new child as the first one
      jsvSetFirstChild(parent, jsvGetRef(namedChild));
    }
  } else { // we have no children - just add it
    JsVarRef r = jsvGetRef(namedChild);
    jsvSetFirstChild(parent, r);
    jsvSetLastChild(parent, r);
  }
}

JsVar *jsvAddNamedChild(JsVar *parent, JsVar *child, const char *name) {
  JsVar *namedChild = jsvMakeIntoVariableName(jsvNewFromString(name), child);
  if (!namedChild) return 0; // Out of memory
  jsvAddName(parent, namedChild);
  return namedChild;
}

JsVar *jsvSetNamedChild(JsVar *parent, JsVar *child, const char *name) {
  JsVar *namedChild = jsvFindChildFromString(parent, name, true);
  if (namedChild) // could be out of memory
    return jsvSetValueOfName(namedChild, child);
  return 0;
}

JsVar *jsvSetValueOfName(JsVar *name, JsVar *src) {
  assert(name && jsvIsName(name));
  assert(name!=src); // no infinite loops!
  // all is fine, so replace the existing child...
  /* Existing child may be null in the case of Z = 0 where
   * we create 'Z' and pass it down to '=' to have the value
   * filled in (or it may be undefined). */
  if (jsvIsNameWithValue(name)) {
    if (jsvIsString(name))
      name->flags = (name->flags & (JsVarFlags)~JSV_VARTYPEMASK) | (JSV_NAME_STRING_0 + jsvGetCharactersInVar(name));
    else
      name->flags = (name->flags & (JsVarFlags)~JSV_VARTYPEMASK) | JSV_NAME_INT;
    jsvSetFirstChild(name, 0);
  } else if (jsvGetFirstChild(name))
    jsvUnRefRef(jsvGetFirstChild(name)); // free existing
  if (src) {
    if (jsvIsInt(name)) {
      if ((jsvIsInt(src) || jsvIsBoolean(src)) && !jsvIsPin(src)) {
        JsVarInt v = src->varData.integer;
        if (v>=JSVARREF_MIN && v<=JSVARREF_MAX) {
          name->flags = (name->flags & (JsVarFlags)~JSV_VARTYPEMASK) | (jsvIsInt(src) ? JSV_NAME_INT_INT : JSV_NAME_INT_BOOL);
          jsvSetFirstChild(name, (JsVarRef)v);
          return name;
        }
      }
    } else if (jsvIsString(name)) {
      if (jsvIsInt(src) && !jsvIsPin(src)) {
        JsVarInt v = src->varData.integer;
        if (v>=JSVARREF_MIN && v<=JSVARREF_MAX) {
          name->flags = (name->flags & (JsVarFlags)~JSV_VARTYPEMASK) | (JSV_NAME_STRING_INT_0 + jsvGetCharactersInVar(name));
          jsvSetFirstChild(name, (JsVarRef)v);
          return name;
        }
      }
    }
    // we can link to a name if we want (so can remove the assert!)
    jsvSetFirstChild(name, jsvGetRef(jsvRef(src)));
  } else
    jsvSetFirstChild(name, 0);
  return name;
}

JsVar *jsvFindChildFromString(JsVar *parent, const char *name, bool addIfNotFound) {
  /* Pull out first 4 bytes, and ensure that everything
   * is 0 padded so that we can do a nice speedy check. */
  char fastCheck[4];
  fastCheck[0] = name[0];
  if (name[0]) {
    fastCheck[1] = name[1];
    if (name[1]) {
      fastCheck[2] = name[2];
      if (name[2]) {
        fastCheck[3] = name[3];
      } else {
        fastCheck[3] = 0;
      }
    } else {
      fastCheck[2] = 0;
      fastCheck[3] = 0;
    }
  } else {
    fastCheck[1] = 0;
    fastCheck[2] = 0;
    fastCheck[3] = 0;
  }

  assert(jsvHasChildren(parent));
  JsVarRef childref = jsvGetFirstChild(parent);
  while (childref) {
    // Don't Lock here, just use GetAddressOf - to try and speed up the finding
    // TODO: We can do this now, but when/if we move to cacheing vars, it'll break
    JsVar *child = jsvGetAddressOf(childref);
    if (*(int*)fastCheck==*(int*)child->varData.str && // speedy check of first 4 bytes
        jsvIsStringEqual(child, name)) {
      // found it! unlock parent but leave child locked
      return jsvLockAgain(child);
    }
    childref = jsvGetNextSibling(child);
  }

  JsVar *child = 0;
  if (addIfNotFound) {
    child = jsvMakeIntoVariableName(jsvNewFromString(name), 0);
    if (child) // could be out of memory
      jsvAddName(parent, child);
  }
  return child;
}

/// Find a child with a matching name using a case insensitive search
JsVar *jsvFindChildFromStringI(JsVar *parent, const char *name) {
  assert(jsvHasChildren(parent));
  JsVarRef childref = jsvGetFirstChild(parent);
  while (childref) {
    // Don't Lock here, just use GetAddressOf - to try and speed up the finding
    // TODO: We can do this now, but when/if we move to cacheing vars, it'll break
    JsVar *child = jsvGetAddressOf(childref);
    if (jsvHasCharacterData(child) &&
        jsvIsStringEqualOrStartsWithOffset(child, name, false, 0, true)) {
      // found it! unlock parent but leave child locked
      return jsvLockAgain(child);
    }
    childref = jsvGetNextSibling(child);
  }
  return 0;
}

/// See jsvIsNewChild - for fields that don't exist yet
JsVar *jsvCreateNewChild(JsVar *parent, JsVar *index, JsVar *child) {
  JsVar *newChild = jsvAsName(index);
  if (!newChild) return 0;
  assert(!jsvGetFirstChild(newChild));
  if (child) jsvSetValueOfName(newChild, child);
  assert(!jsvGetNextSibling(newChild) && !jsvGetPrevSibling(newChild));
  // by setting the siblings as the same, we signal that if set,
  // we should be made a member of the given object
  JsVarRef r = jsvGetRef(jsvRef(jsvRef(parent)));
  jsvSetNextSibling(newChild, r);
  jsvSetPrevSibling(newChild, r);

  return newChild;
}

/** Try and turn the supplied variable into a name. If not, make a new one. This locks again. */
JsVar *jsvAsName(JsVar *var) {
  if (!var) return 0;
  if (jsvGetRefs(var) == 0) {
    // Not reffed - great! let's just use it
    if (!jsvIsName(var))
      var = jsvMakeIntoVariableName(var, 0);
    return jsvLockAgain(var);
  } else { // it was reffed, we must add a new one
    return jsvMakeIntoVariableName(jsvCopy(var, false), 0);
  }
}

/** Non-recursive finding */
JsVar *jsvFindChildFromVar(JsVar *parent, JsVar *childName, bool addIfNotFound) {
  JsVar *child;
  JsVarRef childref = jsvGetFirstChild(parent);

  while (childref) {
    child = jsvLock(childref);
    if (jsvIsBasicVarEqual(child, childName)) {
      // found it! unlock parent but leave child locked
      return child;
    }
    childref = jsvGetNextSibling(child);
    jsvUnLock(child);
  }

  child = 0;
  if (addIfNotFound && childName) {
    child = jsvAsName(childName);
    jsvAddName(parent, child);
  }
  return child;
}

void jsvRemoveChild(JsVar *parent, JsVar *child) {
  assert(jsvHasChildren(parent));
  assert(jsvIsName(child));
#ifdef DEBUG
  assert(!(jsvGetPrevSibling(child) || jsvGetNextSibling(child)) || jsvIsChild(parent, child));
#endif
  JsVarRef childref = jsvGetRef(child);
  bool wasChild = false;
  // unlink from parent
  if (jsvGetFirstChild(parent) == childref) {
    jsvSetFirstChild(parent, jsvGetNextSibling(child));
    wasChild = true;
  }
  if (jsvGetLastChild(parent) == childref) {
    jsvSetLastChild(parent, jsvGetPrevSibling(child));
    wasChild = true;
    // If this was an array and we were the last
    // element, update the length
    if (jsvIsArray(parent)) {
      JsVarInt l = 0;
      // get index of last child
      if (jsvGetLastChild(parent))
        l = jsvGetIntegerAndUnLock(jsvLock(jsvGetLastChild(parent)))+1;
      // set it
      jsvSetArrayLength(parent, l, false);
    }
  }
  // unlink from child list
  if (jsvGetPrevSibling(child)) {
    JsVar *v = jsvLock(jsvGetPrevSibling(child));
    assert(jsvGetNextSibling(v) == jsvGetRef(child));
    jsvSetNextSibling(v, jsvGetNextSibling(child));
    jsvUnLock(v);
    wasChild = true;
  }
  if (jsvGetNextSibling(child)) {
    JsVar *v = jsvLock(jsvGetNextSibling(child));
    assert(jsvGetPrevSibling(v) == jsvGetRef(child));
    jsvSetPrevSibling(v, jsvGetPrevSibling(child));
    jsvUnLock(v);
    wasChild = true;
  }

  jsvSetPrevSibling(child, 0);
  jsvSetNextSibling(child, 0);
  if (wasChild)
    jsvUnRef(child);
}

void jsvRemoveAllChildren(JsVar *parent) {
  assert(jsvHasChildren(parent));
  while (jsvGetFirstChild(parent)) {
    JsVar *v = jsvLock(jsvGetFirstChild(parent));
    jsvRemoveChild(parent, v);
    jsvUnLock(v);
  }
}

/// Check if the given name is a child of the parent
bool jsvIsChild(JsVar *parent, JsVar *child) {
  assert(jsvIsArray(parent) || jsvIsObject(parent) || jsvIsFunction(parent));
  assert(jsvIsName(child));
  JsVarRef childref = jsvGetRef(child);
  JsVarRef indexref;
  indexref = jsvGetFirstChild(parent);
  while (indexref) {
    if (indexref == childref) return true;
    // get next
    JsVar *indexVar = jsvLock(indexref);
    indexref = jsvGetNextSibling(indexVar);
    jsvUnLock(indexVar);
  }
  return false; // not found undefined
}

/// Get the named child of an object. If createChild!=0 then create the child
JsVar *jsvObjectGetChild(JsVar *obj, const char *name, JsVarFlags createChild) {
  if (!obj) return 0;
  assert(jsvHasChildren(obj));
  JsVar *childName = jsvFindChildFromString(obj, name, createChild!=0);
  JsVar *child = jsvSkipName(childName);
  if (!child && createChild && childName!=0/*out of memory?*/) {
    child = jsvNewWithFlags(createChild);
    jsvSetValueOfName(childName, child);
    jsvUnLock(childName);
    return child;
  }
  jsvUnLock(childName);
  return child;
}

/// Get the named child of an object using a case-insensitive search
JsVar *jsvObjectGetChildI(JsVar *obj, const char *name) {
  if (!obj) return 0;
  assert(jsvHasChildren(obj));
  return jsvSkipNameAndUnLock(jsvFindChildFromStringI(obj, name));
}

/// Set the named child of an object, and return the child (so you can choose to unlock it if you want)
JsVar *jsvObjectSetChild(JsVar *obj, const char *name, JsVar *child) {
  assert(jsvHasChildren(obj));
  if (!jsvHasChildren(obj)) return 0;
  // child can actually be a name (for instance if it is a named function)
  JsVar *childName = jsvFindChildFromString(obj, name, true);
  if (!childName) return 0; // out of memory
  jsvSetValueOfName(childName, child);
  jsvUnLock(childName);
  return child;
}

/// Set the named child of an object, and return the child (so you can choose to unlock it if you want)
JsVar *jsvObjectSetChildVar(JsVar *obj, JsVar *name, JsVar *child) {
  assert(jsvHasChildren(obj));
  if (!jsvHasChildren(obj)) return 0;
  // child can actually be a name (for instance if it is a named function)
  JsVar *childName = jsvFindChildFromVar(obj, name, true);
  if (!childName) return 0; // out of memory
  jsvSetValueOfName(childName, child);
  jsvUnLock(childName);
  return child;
}

void jsvObjectSetChildAndUnLock(JsVar *obj, const char *name, JsVar *child) {
  jsvUnLock(jsvObjectSetChild(obj, name, child));
}

void jsvObjectRemoveChild(JsVar *obj, const char *name) {
  JsVar *child = jsvFindChildFromString(obj, name, false);
  if (child) {
    jsvRemoveChild(obj, child);
    jsvUnLock(child);
  }
}

/** Set the named child of an object, and return the child (so you can choose to unlock it if you want).
 * If the child is 0, the 'name' is also removed from the object */
JsVar *jsvObjectSetOrRemoveChild(JsVar *obj, const char *name, JsVar *child) {
  if (child)
    jsvObjectSetChild(obj, name, child);
  else
    jsvObjectRemoveChild(obj, name);
  return child;
}

/** Append all keys from the source object to the target object. Will ignore hidden/internal fields */
void jsvObjectAppendAll(JsVar *target, JsVar *source) {
  assert(jsvIsObject(target));
  assert(jsvIsObject(source));
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, source);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *k = jsvObjectIteratorGetKey(&it);
    JsVar *v = jsvSkipName(k);
    if (!jsvIsInternalObjectKey(k))
      jsvObjectSetChildVar(target, k, v);
    jsvUnLock2(k,v);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
}

int jsvGetChildren(const JsVar *v) {
  //OPT: could length be stored as the value of the array?
  int children = 0;
  JsVarRef childref = jsvGetFirstChild(v);
  while (childref) {
    JsVar *child = jsvLock(childref);
    children++;
    childref = jsvGetNextSibling(child);
    jsvUnLock(child);
  }
  return children;
}

/// Get the first child's name from an object,array or function
JsVar *jsvGetFirstName(JsVar *v) {
  assert(jsvHasChildren(v));
  if (!jsvGetFirstChild(v)) return 0;
  return jsvLock(jsvGetFirstChild(v));
}

JsVarInt jsvGetArrayLength(const JsVar *arr) {
  if (!arr) return 0;
  assert(jsvIsArray(arr));
  return arr->varData.integer;
}

JsVarInt jsvSetArrayLength(JsVar *arr, JsVarInt length, bool truncate) {
  assert(jsvIsArray(arr));
  if (truncate && length < arr->varData.integer) {
    // @TODO implement truncation here
  }
  arr->varData.integer = length;
  return length;
}

JsVarInt jsvGetLength(const JsVar *src) {
  if (jsvIsArray(src)) {
    return jsvGetArrayLength(src);
  } else if (jsvIsArrayBuffer(src)) {
    return (JsVarInt)jsvGetArrayBufferLength(src);
  } else if (jsvIsString(src)) {
    return (JsVarInt)jsvGetStringLength(src);
  } else if (jsvIsObject(src) || jsvIsFunction(src)) {
    return jsvGetChildren(src);
  } else {
    return 1;
  }
}

/** Count the amount of JsVars used. Mostly useful for debugging */
static size_t _jsvCountJsVarsUsedRecursive(JsVar *v, bool resetRecursionFlag) {
  if (!v) return 0;
  // Use IS_RECURSING  flag to stop recursion
  if (resetRecursionFlag) {
    if (!(v->flags & JSV_IS_RECURSING))
      return 0;
    v->flags &= ~JSV_IS_RECURSING;
  } else {
    if (v->flags & JSV_IS_RECURSING)
      return 0;
    v->flags |= JSV_IS_RECURSING;
  }

  size_t count = 1;
  if (jsvHasSingleChild(v) || jsvHasChildren(v)) {
    JsVarRef childref = jsvGetFirstChild(v);
    while (childref) {
      JsVar *child = jsvLock(childref);
      count += _jsvCountJsVarsUsedRecursive(child, resetRecursionFlag);
      if (jsvHasChildren(v)) childref = jsvGetNextSibling(child);
      else childref = 0;
      jsvUnLock(child);
    }
  } else if (jsvIsFlatString(v))
    count += jsvGetFlatStringBlocks(v);
  if (jsvHasCharacterData(v)) {
    JsVarRef childref = jsvGetLastChild(v);
    while (childref) {
      JsVar *child = jsvLock(childref);
      count++;
      childref = jsvGetLastChild(child);
      jsvUnLock(child);
    }
  }
  if (jsvIsName(v) && !jsvIsNameWithValue(v) && jsvGetFirstChild(v)) {
    JsVar *child = jsvLock(jsvGetFirstChild(v));
    count += _jsvCountJsVarsUsedRecursive(child, resetRecursionFlag);
    jsvUnLock(child);
  }
  return count;
}

/** Count the amount of JsVars used. Mostly useful for debugging */
size_t jsvCountJsVarsUsed(JsVar *v) {
  // don't count 'root' when getting sizes
  if ((execInfo.root) && (v != execInfo.root)) execInfo.root->flags |= JSV_IS_RECURSING;
  // we do this so we don't count the same item twice, but don't use too much memory
  size_t c = _jsvCountJsVarsUsedRecursive(v, false);
  _jsvCountJsVarsUsedRecursive(v, true);
  // restore recurse flag
  if ((execInfo.root) && (v != execInfo.root)) execInfo.root->flags &= ~JSV_IS_RECURSING;
  return c;
}

JsVar *jsvGetArrayIndex(const JsVar *arr, JsVarInt index) {
  JsVarRef childref = jsvGetLastChild(arr);
  JsVarInt lastArrayIndex = 0;
  // Look at last non-string element!
  while (childref) {
    JsVar *child = jsvLock(childref);
    if (jsvIsInt(child)) {
      lastArrayIndex = child->varData.integer;
      // it was the last element... sorted!
      if (lastArrayIndex == index) {
        return child;
      }
      jsvUnLock(child);
      break;
    }
    // if not an int, keep going
    childref = jsvGetPrevSibling(child);
    jsvUnLock(child);
  }
  // it's not in this array - don't search the whole lot...
  if (index > lastArrayIndex)
    return 0;
  // otherwise is it more than halfway through?
  if (index > lastArrayIndex/2) {
    // it's in the final half of the array (probably) - search backwards
    while (childref) {
      JsVar *child = jsvLock(childref);

      assert(jsvIsInt(child));
      if (child->varData.integer == index) {
        return child;
      }
      childref = jsvGetPrevSibling(child);
      jsvUnLock(child);
    }
  } else {
    // it's in the first half of the array (probably) - search forwards
    childref = jsvGetFirstChild(arr);
    while (childref) {
      JsVar *child = jsvLock(childref);

      assert(jsvIsInt(child));
      if (child->varData.integer == index) {
        return child;
      }
      childref = jsvGetNextSibling(child);
      jsvUnLock(child);
    }
  }
  return 0; // undefined
}

JsVar *jsvGetArrayItem(const JsVar *arr, JsVarInt index) {
  return jsvSkipNameAndUnLock(jsvGetArrayIndex(arr,index));
}

JsVar *jsvGetLastArrayItem(const JsVar *arr) {
  JsVarRef childref = jsvGetLastChild(arr);
  if (!childref) return 0;
  return jsvSkipNameAndUnLock(jsvLock(childref));
}

void jsvSetArrayItem(JsVar *arr, JsVarInt index, JsVar *item) {
  JsVar *indexVar = jsvGetArrayIndex(arr, index);
  if (indexVar) {
    jsvSetValueOfName(indexVar, item);
  } else {
    indexVar = jsvMakeIntoVariableName(jsvNewFromInteger(index), item);
    if (indexVar) // could be out of memory
      jsvAddName(arr, indexVar);
  }
  jsvUnLock(indexVar);
}

// Get all elements from arr and put them in itemPtr (unless it'd overflow).
// Makes sure all of itemPtr either contains a JsVar or 0
void jsvGetArrayItems(JsVar *arr, unsigned int itemCount, JsVar **itemPtr) {
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, arr);
  unsigned int i = 0;
  while (jsvObjectIteratorHasValue(&it)) {
    if (i<itemCount)
      itemPtr[i++] = jsvObjectIteratorGetValue(&it);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  while (i<itemCount)
    itemPtr[i++] = 0; // just ensure we don't end up with bad data
}

/// Get the index of the value in the iterable var (matchExact==use pointer not equality check, matchIntegerIndices = don't check non-integers)
JsVar *jsvGetIndexOfFull(JsVar *arr, JsVar *value, bool matchExact, bool matchIntegerIndices, int startIdx) {
  if (!jsvIsIterable(arr)) return 0;
  JsvIterator it;
  jsvIteratorNew(&it, arr, JSIF_DEFINED_ARRAY_ElEMENTS);
  while (jsvIteratorHasElement(&it)) {
    JsVar *childIndex = jsvIteratorGetKey(&it);
    if (!matchIntegerIndices ||
        (jsvIsInt(childIndex) && jsvGetInteger(childIndex)>=startIdx)) {
      JsVar *childValue = jsvIteratorGetValue(&it);
      if (childValue==value ||
          (!matchExact && jsvMathsOpTypeEqual(childValue, value))) {
        jsvUnLock(childValue);
        jsvIteratorFree(&it);
        return childIndex;
      }
      jsvUnLock(childValue);
    }
    jsvUnLock(childIndex);
    jsvIteratorNext(&it);
  }
  jsvIteratorFree(&it);
  return 0; // undefined
}

/// Get the index of the value in the array or object (matchExact==use pointer, not equality check)
JsVar *jsvGetIndexOf(JsVar *arr, JsVar *value, bool matchExact) {
  return jsvGetIndexOfFull(arr, value, matchExact, false, 0);
}


/// Adds new elements to the end of an array, and returns the new length. initialValue is the item index when no items are currently in the array.
JsVarInt jsvArrayAddToEnd(JsVar *arr, JsVar *value, JsVarInt initialValue) {
  assert(jsvIsArray(arr));
  JsVarInt index = initialValue;
  if (jsvGetLastChild(arr)) {
    JsVar *last = jsvLock(jsvGetLastChild(arr));
    index = jsvGetInteger(last)+1;
    jsvUnLock(last);
  }

  JsVar *idx = jsvMakeIntoVariableName(jsvNewFromInteger(index), value);
  if (!idx) return 0; // out of memory - error flag will have been set already
  jsvAddName(arr, idx);
  jsvUnLock(idx);
  return index+1;
}

/// Adds new elements to the end of an array, and returns the new length
JsVarInt jsvArrayPush(JsVar *arr, JsVar *value) {
  assert(jsvIsArray(arr));
  JsVarInt index = jsvGetArrayLength(arr);
  JsVar *idx = jsvMakeIntoVariableName(jsvNewFromInteger(index), value);
  if (!idx) return 0; // out of memory - error flag will have been set already
  jsvAddName(arr, idx);
  jsvUnLock(idx);
  return jsvGetArrayLength(arr);
}

/// Adds a new element to the end of an array, unlocks it, and returns the new length
JsVarInt jsvArrayPushAndUnLock(JsVar *arr, JsVar *value) {
  JsVarInt l = jsvArrayPush(arr, value);
  jsvUnLock(value);
  return l;
}

// Push 2 integers onto the end of an array
void jsvArrayPush2Int(JsVar *arr, JsVarInt a, JsVarInt b) {
  jsvArrayPushAndUnLock(arr, jsvNewFromInteger(a));
  jsvArrayPushAndUnLock(arr, jsvNewFromInteger(b));
}

/// Append all values from the source array to the target array
void jsvArrayPushAll(JsVar *target, JsVar *source, bool checkDuplicates) {
  assert(jsvIsArray(target));
  assert(jsvIsArray(source));
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, source);
  while (jsvObjectIteratorHasValue(&it)) {
    JsVar *v = jsvObjectIteratorGetValue(&it);
    bool add = true;
    if (checkDuplicates) {
      JsVar *idx = jsvGetIndexOf(target, v, false);
      if (idx) {
        add = false;
        jsvUnLock(idx);
      }
    }
    if (add) jsvArrayPush(target, v);
    jsvUnLock(v);
    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
}

/// Removes the last element of an array, and returns that element (or 0 if empty). includes the NAME
JsVar *jsvArrayPop(JsVar *arr) {
  assert(jsvIsArray(arr));
  JsVar *child = 0;
  JsVarInt length = jsvGetArrayLength(arr);
  if (length > 0) {
    length--;

    if (jsvGetLastChild(arr)) {
      // find last child with an integer key
      JsVarRef ref = jsvGetLastChild(arr);
      child = jsvLock(ref);
      while (child && !jsvIsInt(child)) {
        ref = jsvGetPrevSibling(child);
        jsvUnLock(child);
        if (ref) {
          child = jsvLock(ref);
        } else {
          child = 0;
        }
      }
      // check if the last integer key really is the last element
      if (child) {
        if (jsvGetInteger(child) == length) {
          // child is the last element - remove it
          jsvRemoveChild(arr, child);
        } else {
          // child is not the last element
          jsvUnLock(child);
          child = 0;
        }
      }
    }
    // and finally shrink the array
    jsvSetArrayLength(arr, length, false);
  }

  return child;
}

/// Removes the first element of an array, and returns that element (or 0 if empty). DOES NOT RENUMBER.
JsVar *jsvArrayPopFirst(JsVar *arr) {
  assert(jsvIsArray(arr));
  if (jsvGetFirstChild(arr)) {
    JsVar *child = jsvLock(jsvGetFirstChild(arr));
    if (jsvGetFirstChild(arr) == jsvGetLastChild(arr))
      jsvSetLastChild(arr, 0); // if 1 item in array
    jsvSetFirstChild(arr, jsvGetNextSibling(child)); // unlink from end of array
    jsvUnRef(child); // as no longer in array
    if (jsvGetNextSibling(child)) {
      JsVar *v = jsvLock(jsvGetNextSibling(child));
      jsvSetPrevSibling(v, 0);
      jsvUnLock(v);
    }
    jsvSetNextSibling(child, 0);
    return child; // and return it
  } else {
    // no children!
    return 0;
  }
}

/// Adds a new variable element to the end of an array (IF it was not already there). Return true if successful
void jsvArrayAddUnique(JsVar *arr, JsVar *v) {
  JsVar *idx = jsvGetIndexOf(arr, v, false); // did it already exist?
  if (!idx) {
    jsvArrayPush(arr, v); // if 0, it failed
  } else {
    jsvUnLock(idx);
  }
}

/// Join all elements of an array together into a string
JsVar *jsvArrayJoin(JsVar *arr, JsVar *filler, bool ignoreNull) {
  JsVar *str = jsvNewFromEmptyString();
  if (!str) return 0; // out of memory
  assert(!filler || jsvIsString(filler));

  JsvIterator it;
  jsvIteratorNew(&it, arr, JSIF_EVERY_ARRAY_ELEMENT);
  JsvStringIterator itdst;
  jsvStringIteratorNew(&itdst, str, 0);
  bool first = true;
  while (!jspIsInterrupted() && jsvIteratorHasElement(&it)) {
    JsVar *key = jsvIteratorGetKey(&it);
    if (jsvIsInt(key)) {
      // add the filler
      if (filler && !first)
        jsvStringIteratorAppendString(&itdst, filler, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
      first = false;
      // add the value
      JsVar *value = jsvIteratorGetValue(&it);
      if (value && (!ignoreNull || !jsvIsNull(value))) {
        JsVar *valueStr = jsvAsString(value);
        if (valueStr) { // could be out of memory
          jsvStringIteratorAppendString(&itdst, valueStr, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
          jsvUnLock(valueStr);
        }
      }
      jsvUnLock(value);
    }
    jsvUnLock(key);
    jsvIteratorNext(&it);
  }
  jsvIteratorFree(&it);
  jsvStringIteratorFree(&itdst);
  return str;
}

/// Insert a new element before beforeIndex, DOES NOT UPDATE INDICES
void jsvArrayInsertBefore(JsVar *arr, JsVar *beforeIndex, JsVar *element) {
  if (beforeIndex) {
    JsVar *idxVar = jsvMakeIntoVariableName(jsvNewFromInteger(0), element);
    if (!idxVar) return; // out of memory

    JsVarRef idxRef = jsvGetRef(jsvRef(idxVar));
    JsVarRef prev = jsvGetPrevSibling(beforeIndex);
    if (prev) {
      JsVar *prevVar = jsvRef(jsvLock(prev));
      jsvSetInteger(idxVar, jsvGetInteger(prevVar)+1); // update index number
      jsvSetNextSibling(prevVar, idxRef);
      jsvUnLock(prevVar);
      jsvSetPrevSibling(idxVar, prev);
    } else {
      jsvSetPrevSibling(idxVar, 0);
      jsvSetFirstChild(arr, idxRef);
    }
    jsvSetPrevSibling(beforeIndex, idxRef);
    jsvSetNextSibling(idxVar, jsvGetRef(jsvRef(beforeIndex)));
    jsvUnLock(idxVar);
  } else
    jsvArrayPush(arr, element);
}

/** Same as jsvMathsOpPtr, but if a or b are a name, skip them
 * and go to what they point to. Also handle the case where
 * they may be objects with valueOf functions. */
JsVar *jsvMathsOpSkipNames(JsVar *a, JsVar *b, int op) {
  JsVar *pa = jsvSkipName(a);
  JsVar *pb = jsvSkipName(b);
  JsVar *oa = jsvGetValueOf(pa);
  JsVar *ob = jsvGetValueOf(pb);
  jsvUnLock2(pa, pb);
  JsVar *res = jsvMathsOp(oa,ob,op);
  jsvUnLock2(oa, ob);
  return res;
}


JsVar *jsvMathsOpError(int op, const char *datatype) {
  char opName[32];
  jslTokenAsString(op, opName, sizeof(opName));
  jsError("Operation %s not supported on the %s datatype", opName, datatype);
  return 0;
}

bool jsvMathsOpTypeEqual(JsVar *a, JsVar *b) {
  // check type first, then call again to check data
  bool eql = (a==0) == (b==0);
  if (a && b) {
    // Check whether both are numbers, otherwise check the variable
    // type flags themselves
    eql = ((jsvIsInt(a)||jsvIsFloat(a)) && (jsvIsInt(b)||jsvIsFloat(b))) ||
          (jsvIsString(a) && jsvIsString(b)) ||
          ((a->flags & JSV_VARTYPEMASK) == (b->flags & JSV_VARTYPEMASK));
  }
  if (eql) {
    JsVar *contents = jsvMathsOp(a,b, LEX_EQUAL);
    if (!jsvGetBool(contents)) eql = false;
    jsvUnLock(contents);
  } else {
    /* Make sure we don't get in the situation where we have two equal
     * strings with a check that fails because they were stored differently */
    assert(!(jsvIsString(a) && jsvIsString(b) && jsvIsBasicVarEqual(a,b)));
  }
  return eql;
}

JsVar *jsvMathsOp(JsVar *a, JsVar *b, int op) {
  // Type equality check
  if (op == LEX_TYPEEQUAL || op == LEX_NTYPEEQUAL) {
    bool eql = jsvMathsOpTypeEqual(a,b);
    if (op == LEX_TYPEEQUAL)
      return jsvNewFromBool(eql);
    else
      return jsvNewFromBool(!eql);
  }

  bool needsInt = op=='&' || op=='|' || op=='^' || op==LEX_LSHIFT || op==LEX_RSHIFT || op==LEX_RSHIFTUNSIGNED;
  bool needsNumeric = needsInt || op=='*' || op=='/' || op=='%' || op=='-';
  bool isCompare = op==LEX_EQUAL || op==LEX_NEQUAL || op=='<' || op==LEX_LEQUAL || op=='>'|| op==LEX_GEQUAL;
  if (isCompare) {
    if (jsvIsNumeric(a) && jsvIsString(b)) {
      needsNumeric = true;
      needsInt = jsvIsIntegerish(a) && jsvIsStringNumericInt(b, false);
    } else if (jsvIsNumeric(b) && jsvIsString(a)) {
      needsNumeric = true;
      needsInt = jsvIsIntegerish(b) && jsvIsStringNumericInt(a, false);
    }
  }

  // do maths...
  if (jsvIsUndefined(a) && jsvIsUndefined(b)) {
    if (op == LEX_EQUAL)
      return jsvNewFromBool(true);
    else if (op == LEX_NEQUAL)
      return jsvNewFromBool(false);
    else
      return 0; // undefined
  } else if (needsNumeric ||
      ((jsvIsNumeric(a) || jsvIsUndefined(a) || jsvIsNull(a)) &&
          (jsvIsNumeric(b) || jsvIsUndefined(b) || jsvIsNull(b)))) {
    if (needsInt || (jsvIsIntegerish(a) && jsvIsIntegerish(b))) {
      // note that int+undefined should be handled as a double
      // use ints
      JsVarInt da = jsvGetInteger(a);
      JsVarInt db = jsvGetInteger(b);
      switch (op) {
      case '+': return jsvNewFromLongInteger((long long)da + (long long)db);
      case '-': return jsvNewFromLongInteger((long long)da - (long long)db);
      case '*': return jsvNewFromLongInteger((long long)da * (long long)db);
      case '/': return jsvNewFromFloat((JsVarFloat)da/(JsVarFloat)db);
      case '&': return jsvNewFromInteger(da&db);
      case '|': return jsvNewFromInteger(da|db);
      case '^': return jsvNewFromInteger(da^db);
      case '%': if (db<0) db=-db; // fix SIGFPE
                return db ? jsvNewFromInteger(da%db) : jsvNewFromFloat(NAN);
      case LEX_LSHIFT: return jsvNewFromInteger(da << db);
      case LEX_RSHIFT: return jsvNewFromInteger(da >> db);
      case LEX_RSHIFTUNSIGNED: return jsvNewFromLongInteger(((JsVarIntUnsigned)da) >> db);
      case LEX_EQUAL:     return jsvNewFromBool(da==db && jsvIsNull(a)==jsvIsNull(b));
      case LEX_NEQUAL:    return jsvNewFromBool(da!=db || jsvIsNull(a)!=jsvIsNull(b));
      case '<':           return jsvNewFromBool(da<db);
      case LEX_LEQUAL:    return jsvNewFromBool(da<=db);
      case '>':           return jsvNewFromBool(da>db);
      case LEX_GEQUAL:    return jsvNewFromBool(da>=db);
      default: return jsvMathsOpError(op, "Integer");
      }
    } else {
      // use doubles
      JsVarFloat da = jsvGetFloat(a);
      JsVarFloat db = jsvGetFloat(b);
      switch (op) {
      case '+': return jsvNewFromFloat(da+db);
      case '-': return jsvNewFromFloat(da-db);
      case '*': return jsvNewFromFloat(da*db);
      case '/': return jsvNewFromFloat(da/db);
      case '%': return jsvNewFromFloat(jswrap_math_mod(da, db));
      case LEX_EQUAL:
      case LEX_NEQUAL:  { bool equal = da==db && jsvIsNull(a)==jsvIsNull(b);
      if ((jsvIsNull(a) && jsvIsUndefined(b)) ||
          (jsvIsNull(b) && jsvIsUndefined(a))) equal = true; // JS quirk :)
      return jsvNewFromBool((op==LEX_EQUAL) ? equal : ((bool)!equal));
      }
      case '<':           return jsvNewFromBool(da<db);
      case LEX_LEQUAL:    return jsvNewFromBool(da<=db);
      case '>':           return jsvNewFromBool(da>db);
      case LEX_GEQUAL:    return jsvNewFromBool(da>=db);
      default: return jsvMathsOpError(op, "Double");
      }
    }
  } else if ((jsvIsArray(a) || jsvIsObject(a) || jsvIsFunction(a) ||
      jsvIsArray(b) || jsvIsObject(b) || jsvIsFunction(b)) &&
      jsvIsArray(a)==jsvIsArray(b) && // Fix #283 - convert to string and test if only one is an array
      (op == LEX_EQUAL || op==LEX_NEQUAL)) {
    bool equal = a==b;

    if (jsvIsNativeFunction(a) || jsvIsNativeFunction(b)) {
      // even if one is not native, the contents will be different
      equal = a && b &&
          a->varData.native.ptr == b->varData.native.ptr &&
          a->varData.native.argTypes == b->varData.native.argTypes &&
          jsvGetFirstChild(a) == jsvGetFirstChild(b);
    }

    /* Just check pointers */
    switch (op) {
    case LEX_EQUAL:  return jsvNewFromBool(equal);
    case LEX_NEQUAL: return jsvNewFromBool(!equal);
    default: return jsvMathsOpError(op, jsvIsArray(a)?"Array":"Object");
    }
  } else {
    JsVar *da = jsvAsString(a);
    JsVar *db = jsvAsString(b);
    if (!da || !db) { // out of memory
      jsvUnLock2(da, db);
      return 0;
    }
    if (op=='+') {
      JsVar *v;
      // Don't copy 'da' if it's not used elsewhere (eg we made it in 'jsvAsString' above)
      if (jsvIsBasicString(da) && jsvGetLocks(da)==1 && jsvGetRefs(da)==0)
        v = jsvLockAgain(da);
      else if (JSV_IS_NONAPPENDABLE_STRING(da->flags & JSV_VARTYPEMASK)) {
        // It's a string, but it can't be appended - don't copy as copying will just keep the same var type!
        // Instead we create a new string var by copying
        // opt: should we allocate a flat string here? but repeated appends would then be slow
        v = jsvNewFromEmptyString(); // don't use jsvNewFromStringVar as this does a copy for native/flash strs too
        if (v) jsvAppendStringVar(v, da, 0, JSVAPPENDSTRINGVAR_MAXLENGTH);
      } else // otherwise just copy it
        v = jsvCopy(da, false);
      if (v) // could be out of memory
        jsvAppendStringVarComplete(v, db);
      jsvUnLock2(da, db);
      return v;
    }

    int cmp = jsvCompareString(da,db,0,0,false);
    jsvUnLock2(da, db);
    // use strings
    switch (op) {
    case LEX_EQUAL:     return jsvNewFromBool(cmp==0);
    case LEX_NEQUAL:    return jsvNewFromBool(cmp!=0);
    case '<':           return jsvNewFromBool(cmp<0);
    case LEX_LEQUAL:    return jsvNewFromBool(cmp<=0);
    case '>':           return jsvNewFromBool(cmp>0);
    case LEX_GEQUAL:    return jsvNewFromBool(cmp>=0);
    default: return jsvMathsOpError(op, "String");
    }
  }
}

JsVar *jsvNegateAndUnLock(JsVar *v) {
  JsVar *zero = jsvNewFromInteger(0);
  JsVar *res = jsvMathsOpSkipNames(zero, v, '-');
  jsvUnLock2(zero, v);
  return res;
}

/// see jsvGetPathTo
static JsVar *jsvGetPathTo_int(JsVar *root, JsVar *element, int maxDepth, JsVar *ignoreParent, int *depth) {
  if (maxDepth<=0) return 0;

  int bestDepth = maxDepth+1;
  JsVar *found = 0;

  JsvIterator it;
  jsvIteratorNew(&it, root, JSIF_DEFINED_ARRAY_ElEMENTS);
  while (jsvIteratorHasElement(&it)) {
    JsVar *el = jsvIteratorGetValue(&it);
    if (el == element && root != ignoreParent) {
      // if we found it - send the key name back!
      JsVar *name = jsvAsStringAndUnLock(jsvIteratorGetKey(&it));
      jsvIteratorFree(&it);
      return name;
    } else if (jsvIsObject(el) || jsvIsArray(el) || jsvIsFunction(el)) {
      // recursively search
      int d;
      JsVar *n = jsvGetPathTo_int(el, element, maxDepth-1, ignoreParent, &d);
      if (n && d<bestDepth) {
        bestDepth = d;
        // we found it! Append our name onto it as well
        JsVar *keyName = jsvIteratorGetKey(&it);
        jsvUnLock(found);
        found = jsvVarPrintf(jsvIsObject(el) ? "%v.%v" : "%v[%q]",keyName,n);
        jsvUnLock(keyName);
      }
      jsvUnLock(n);
    }
    jsvIteratorNext(&it);
  }
  jsvIteratorFree(&it);
  *depth = bestDepth;
  return found;
}

/** If the given element is found, return the path to it as a string of
 * the form 'foo.bar', else return 0. If we would have returned a.b and
 * ignoreParent is a, don't! */
JsVar *jsvGetPathTo(JsVar *root, JsVar *element, int maxDepth, JsVar *ignoreParent) {
  int depth = 0;
  return jsvGetPathTo_int(root, element, maxDepth, ignoreParent, &depth);
}

void jsvTraceLockInfo(JsVar *v) {
  jsiConsolePrintf("#%d[r%d,l%d] ",jsvGetRef(v),jsvGetRefs(v),jsvGetLocks(v));
}

/** Get the lowest level at which searchRef appears */
int _jsvTraceGetLowestLevel(JsVar *var, JsVar *searchVar) {
  if (var == searchVar) return 0;
  int found = -1;

  // Use IS_RECURSING  flag to stop recursion
  if (var->flags & JSV_IS_RECURSING)
    return -1;
  var->flags |= JSV_IS_RECURSING;

  if (jsvHasSingleChild(var) && jsvGetFirstChild(var)) {
    JsVar *child = jsvLock(jsvGetFirstChild(var));
    int f = _jsvTraceGetLowestLevel(child, searchVar);
    jsvUnLock(child);
    if (f>=0 && (found<0 || f<found)) found=f+1;
  }
  if (jsvHasChildren(var)) {
    JsVarRef childRef = jsvGetFirstChild(var);
    while (childRef) {
      JsVar *child = jsvLock(childRef);
      int f = _jsvTraceGetLowestLevel(child, searchVar);
      if (f>=0 && (found<0 || f<found)) found=f+1;
      childRef = jsvGetNextSibling(child);
      jsvUnLock(child);
    }
  }

  var->flags &= ~JSV_IS_RECURSING;

  return found; // searchRef not found
}

void _jsvTrace(JsVar *var, int indent, JsVar *baseVar, int level) {
#ifdef SAVE_ON_FLASH
  jsiConsolePrint("Trace unimplemented in this version.\n");
#else
  int i;
  for (i=0;i<indent;i++) jsiConsolePrint(" ");


  if (!var) {
    jsiConsolePrint("undefined");
    return;
  }
  if (level>0 && var==execInfo.root) {
    jsiConsolePrint("ROOT");
    return;
  }

  jsvTraceLockInfo(var);

  int lowestLevel = _jsvTraceGetLowestLevel(baseVar, var);
  if (level>16 || (lowestLevel>=0 && lowestLevel < level)) {
    // If this data is available elsewhere in the tree (but nearer the root)
    // then don't print it. This makes the dump significantly more readable!
    // It also stops us getting in recursive loops ...
    jsiConsolePrint("...\n");
    return;
  }

  if (jsvIsNewChild(var)) {
    jsiConsolePrint("NewChild PARENT:");
    JsVar *parent = jsvGetAddressOf(jsvGetNextSibling(var));
    _jsvTrace(parent, indent+2, baseVar, level+1);
    jsiConsolePrint("CHILD: ");
  } else if (jsvIsName(var)) jsiConsolePrint("Name ");

  char endBracket = ' ';
  if (jsvIsObject(var)) { jsiConsolePrint("Object { "); endBracket = '}'; }
  else if (jsvIsGetterOrSetter(var)) { jsiConsolePrint("Getter/Setter { "); endBracket = '}'; }
  else if (jsvIsArray(var)) { jsiConsolePrintf("Array(%d) [ ", var->varData.integer); endBracket = ']'; }
  else if (jsvIsNativeFunction(var)) { jsiConsolePrintf("NativeFunction 0x%x (%d) { ", var->varData.native.ptr, var->varData.native.argTypes); endBracket = '}'; }
  else if (jsvIsFunction(var)) {
    jsiConsolePrint("Function { ");
    if (jsvIsFunctionReturn(var)) jsiConsolePrint("return ");
    endBracket = '}';
  } else if (jsvIsPin(var)) jsiConsolePrintf("Pin %d", jsvGetInteger(var));
  else if (jsvIsInt(var)) jsiConsolePrintf("Integer %d", jsvGetInteger(var));
  else if (jsvIsBoolean(var)) jsiConsolePrintf("Bool %s", jsvGetBool(var)?"true":"false");
  else if (jsvIsFloat(var)) jsiConsolePrintf("Double %f", jsvGetFloat(var));
  else if (jsvIsFunctionParameter(var)) jsiConsolePrintf("Param %q ", var);
  else if (jsvIsArrayBufferName(var)) jsiConsolePrintf("ArrayBufferName[%d] ", jsvGetInteger(var));
  else if (jsvIsArrayBuffer(var)) jsiConsolePrintf("%s (offs %d, len %d)", jswGetBasicObjectName(var)?jswGetBasicObjectName(var):"unknown ArrayBuffer", var->varData.arraybuffer.byteOffset, var->varData.arraybuffer.length); // way to get nice name
  else if (jsvIsString(var)) {
    size_t blocks = 1;
    if (jsvGetLastChild(var)) {
      JsVar *v = jsvGetAddressOf(jsvGetLastChild(var));
      blocks += jsvCountJsVarsUsed(v);
    }
    if (jsvIsFlatString(var)) {
      blocks += jsvGetFlatStringBlocks(var);
    }
    const char *name = "";
    if (jsvIsFlatString(var)) name="Flat";
    if (jsvIsNativeString(var)) name="Native";
    if (jsvIsFlashString(var)) name="Flash";
    jsiConsolePrintf("%sString [%d blocks] %q", name, blocks, var);
  } else {
    jsiConsolePrintf("Unknown %d", var->flags & (JsVarFlags)~(JSV_LOCK_MASK));
  }
  if (jsvIsConstant(var)) jsiConsolePrintf(" CONST ");

  // print a value if it was stored in here as well...
  if (jsvIsNameInt(var)) {
    jsiConsolePrintf(" = int %d\n", (int)jsvGetFirstChildSigned(var));
    return;
  } else if (jsvIsNameIntBool(var)) {
    jsiConsolePrintf(" = bool %s\n", jsvGetFirstChild(var)?"true":"false");
    return;
  }

  if (jsvHasSingleChild(var)) {
    JsVar *child = jsvGetFirstChild(var) ? jsvGetAddressOf(jsvGetFirstChild(var)) : 0;
    _jsvTrace(child, indent+2, baseVar, level+1);
  } else if (jsvHasChildren(var)) {
    JsvIterator it;
    jsvIteratorNew(&it, var, JSIF_DEFINED_ARRAY_ElEMENTS);
    bool first = true;
    while (jsvIteratorHasElement(&it) && !jspIsInterrupted()) {
      if (first) jsiConsolePrintf("\n");
      first = false;
      JsVar *child = jsvIteratorGetKey(&it);
      _jsvTrace(child, indent+2, baseVar, level+1);
      jsvUnLock(child);
      jsiConsolePrintf("\n");
      jsvIteratorNext(&it);
    }
    jsvIteratorFree(&it);
    if (!first)
      for (i=0;i<indent;i++) jsiConsolePrint(" ");
  }
  jsiConsolePrintf("%c", endBracket);
#endif
}

/** Write debug info for this Var out to the console */
void jsvTrace(JsVar *var, int indent) {
  /* Clear memory busy flags. If we're calling
   * trace then we really care about getting an answer */
  MemBusyType t = isMemoryBusy;
  isMemoryBusy = 0;
  _jsvTrace(var,indent,var,0);
  isMemoryBusy = t;
  jsiConsolePrintf("\n");
}


/** Recursively mark the variable. Return false if it fails due to stack. */
static bool jsvGarbageCollectMarkUsed(JsVar *var) {
  var->flags &= (JsVarFlags)~JSV_GARBAGE_COLLECT;
  JsVarRef child;
  JsVar *childVar;

  if (jsvHasStringExt(var)) {
    // non-recursively scan strings
    child = jsvGetLastChild(var);
    while (child) {
      childVar = jsvGetAddressOf(child);
      childVar->flags &= (JsVarFlags)~JSV_GARBAGE_COLLECT;
      child = jsvGetLastChild(childVar);
    }
  }
  // intentionally no else
  if (jsvHasSingleChild(var)) {
    if (jsvGetFirstChild(var)) {
      childVar = jsvGetAddressOf(jsvGetFirstChild(var));
      if (childVar->flags & JSV_GARBAGE_COLLECT)
        if (!jsvGarbageCollectMarkUsed(childVar)) return false;
    }
  } else if (jsvHasChildren(var)) {
    if (jsuGetFreeStack() < 256) return false;

    child = jsvGetFirstChild(var);
    while (child) {
      childVar = jsvGetAddressOf(child);
      if (childVar->flags & JSV_GARBAGE_COLLECT)
        if (!jsvGarbageCollectMarkUsed(childVar)) return false;
      child = jsvGetNextSibling(childVar);
    }
  }

  return true;
}

/** Run a garbage collection sweep - return nonzero if things have been freed */
int jsvGarbageCollect() {
  if (isMemoryBusy) return 0;
  isMemoryBusy = MEMBUSY_GC;
  JsVarRef i;
  // Add GC flags to anything that is currently used
  for (i=1;i<=jsVarsSize;i++)  {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags&JSV_VARTYPEMASK) != JSV_UNUSED) { // if it is not unused
      var->flags |= (JsVarFlags)JSV_GARBAGE_COLLECT;
      // if we have a flat string, skip that many blocks
      if (jsvIsFlatString(var))
        i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
    }
  }
  /* recursively remove anything that is referenced from a var that is locked. */
  for (i=1;i<=jsVarsSize;i++)  {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags & JSV_GARBAGE_COLLECT) && // not already GC'd
        jsvGetLocks(var)>0) { // or it is locked
      if (!jsvGarbageCollectMarkUsed(var)) {
        // this could fail due to stack exhausted (eg big linked list)
        // JSV_GARBAGE_COLLECT are left set, but not a big problem as next GC will clear them
        isMemoryBusy = MEM_NOT_BUSY;
        return 0;
      }
    }
    // if we have a flat string, skip that many blocks
    if (jsvIsFlatString(var))
      i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
  }
  /* now sweep for things that we can GC!
   * Also update the free list - this means that every new variable that
   * gets allocated gets allocated towards the start of memory, which
   * hopefully helps compact everything towards the start. */
  unsigned int freedCount = 0;
  jsVarFirstEmpty = 0;
  JsVar *lastEmpty = 0;
  for (i=1;i<=jsVarsSize;i++)  {
    JsVar *var = jsvGetAddressOf(i);
    if (var->flags & JSV_GARBAGE_COLLECT) {
      if (jsvIsFlatString(var)) {
        // If we're a flat string, there are more blocks to free.
        unsigned int count = (unsigned int)jsvGetFlatStringBlocks(var);
        freedCount+=count;
        // Free the first block
        var->flags = JSV_UNUSED;
        // add this to our free list
        if (lastEmpty) jsvSetNextSibling(lastEmpty, i);
        else jsVarFirstEmpty = i;
        lastEmpty = var;
        // free subsequent blocks
        while (count-- > 0) {
          i++;
          var = jsvGetAddressOf((JsVarRef)(i));
          var->flags = JSV_UNUSED;
          // add this to our free list
          if (lastEmpty) jsvSetNextSibling(lastEmpty, i);
          else jsVarFirstEmpty = i;
          lastEmpty = var;
        }
      } else {
        // otherwise just free 1 block
        if (jsvHasSingleChild(var)) {
          /* If this had a child that wasn't listed for GC then we need to
           * unref it. Everything else is fine because it'll disappear anyway.
           * We don't have to check if we should free this other variable
           * here because we know the GC picked up it was referenced from
           * somewhere else. */
          JsVarRef ch = jsvGetFirstChild(var);
          if (ch) {
            JsVar *child = jsvGetAddressOf(ch); // not locked
            if (child->flags!=JSV_UNUSED && // not already GC'd!
                !(child->flags&JSV_GARBAGE_COLLECT)) // not marked for GC
              jsvUnRef(child);
          }
        }
        /* Sanity checks here. We're making sure that any variables that are
         * linked from this one have either already been garbage collected or
         * are marked for GC */
        assert(!jsvHasChildren(var) || !jsvGetFirstChild(var) ||
            jsvGetLocks(jsvGetAddressOf(jsvGetFirstChild(var))) ||
            jsvGetAddressOf(jsvGetFirstChild(var))->flags==JSV_UNUSED ||
            (jsvGetAddressOf(jsvGetFirstChild(var))->flags&JSV_GARBAGE_COLLECT));
        assert(!jsvHasChildren(var) || !jsvGetLastChild(var) ||
            jsvGetLocks(jsvGetAddressOf(jsvGetLastChild(var))) ||
            jsvGetAddressOf(jsvGetLastChild(var))->flags==JSV_UNUSED ||
            (jsvGetAddressOf(jsvGetLastChild(var))->flags&JSV_GARBAGE_COLLECT));
        assert(!jsvIsName(var) || !jsvGetPrevSibling(var) ||
            jsvGetLocks(jsvGetAddressOf(jsvGetPrevSibling(var))) ||
            jsvGetAddressOf(jsvGetPrevSibling(var))->flags==JSV_UNUSED ||
            (jsvGetAddressOf(jsvGetPrevSibling(var))->flags&JSV_GARBAGE_COLLECT));
        assert(!jsvIsName(var) || !jsvGetNextSibling(var) ||
            jsvGetLocks(jsvGetAddressOf(jsvGetNextSibling(var))) ||
            jsvGetAddressOf(jsvGetNextSibling(var))->flags==JSV_UNUSED ||
            (jsvGetAddressOf(jsvGetNextSibling(var))->flags&JSV_GARBAGE_COLLECT));
        // free!
        var->flags = JSV_UNUSED;
        // add this to our free list
        if (lastEmpty) jsvSetNextSibling(lastEmpty, i);
        else jsVarFirstEmpty = i;
        lastEmpty = var;
        freedCount++;
      }
    } else if (jsvIsFlatString(var)) {
      // if we have a flat string, skip forward that many blocks
      i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
    } else if (var->flags == JSV_UNUSED) {
      // this is already free - add it to the free list
      if (lastEmpty) jsvSetNextSibling(lastEmpty, i);
      else jsVarFirstEmpty = i;
      lastEmpty = var;
    }
  }
  if (lastEmpty) jsvSetNextSibling(lastEmpty, 0);
  isMemoryBusy = MEM_NOT_BUSY;
  return (int)freedCount;
}

void jsvDefragment() {
  // garbage collect - removes cruft
  // also puts free list in order
  jsvGarbageCollect();
  // Fill defragVars with defraggable variables
  jshInterruptOff();
  const int DEFRAGVARS = 256; // POWER OF 2
  JsVarRef defragVars[DEFRAGVARS];
  memset(defragVars, 0, sizeof(defragVars));
  int defragVarIdx = 0;
  for (unsigned int i=0;i<jsvGetMemoryTotal();i++) {
    JsVarRef vr = (JsVarRef)(i+1);
    JsVar *v = _jsvGetAddressOf(vr);
    if ((v->flags&JSV_VARTYPEMASK)!=JSV_UNUSED) {
      if (jsvIsFlatString(v)) {
        i += (unsigned int)jsvGetFlatStringBlocks(v); // skip forward
      } else if (jsvGetLocks(v)==0) {
        defragVars[defragVarIdx] = vr;
        defragVarIdx = (defragVarIdx+1) & (DEFRAGVARS-1);
      }
    }
  }
  // Now go through defragVars defragging them
  defragVarIdx--;
  if (defragVarIdx<0) defragVarIdx+=DEFRAGVARS;
  while (defragVars[defragVarIdx]) {
    JsVarRef defragFromRef = defragVars[defragVarIdx];
    JsVarRef defragToRef = jsVarFirstEmpty;
    if (!defragToRef || defragFromRef<defragToRef) {
      // we're done!
      break;
    }
    // relocate!
    JsVar *defragFrom = _jsvGetAddressOf(defragFromRef);
    JsVar *defragTo = _jsvGetAddressOf(defragToRef);
    jsVarFirstEmpty = jsvGetNextSibling(defragTo); // move our reference to the next in the free list
    // copy data
    *defragTo = *defragFrom;
    defragFrom->flags = JSV_UNUSED;
    // find references!
    for (unsigned int i=0;i<jsvGetMemoryTotal();i++) {
      JsVarRef vr = (JsVarRef)(i+1);
      JsVar *v = _jsvGetAddressOf(vr);
      if ((v->flags&JSV_VARTYPEMASK)!=JSV_UNUSED) {
        if (jsvIsFlatString(v)) {
          i += (unsigned int)jsvGetFlatStringBlocks(v); // skip forward
        } else {
          if (jsvHasSingleChild(v))
            if (jsvGetFirstChild(v)==defragFromRef)
              jsvSetFirstChild(v,defragToRef);
          if (jsvHasStringExt(v))
            if (jsvGetLastChild(v)==defragFromRef)
              jsvSetLastChild(v,defragToRef);
          if (jsvHasChildren(v)) {
            if (jsvGetFirstChild(v)==defragFromRef)
              jsvSetFirstChild(v,defragToRef);
            if (jsvGetLastChild(v)==defragFromRef)
              jsvSetLastChild(v,defragToRef);
          }
          if (jsvIsName(v)) {
            if (jsvGetNextSibling(v)==defragFromRef)
              jsvSetNextSibling(v,defragToRef);
            if (jsvGetPrevSibling(v)==defragFromRef)
              jsvSetPrevSibling(v,defragToRef);
          }
        }
      }
    }
    // zero element and move to next...
    defragVars[defragVarIdx] = 0;
    defragVarIdx--;
    if (defragVarIdx<0) defragVarIdx+=DEFRAGVARS;
    // bump watchdog just in case it took too long
    jshKickWatchDog();
  }
  // rebuild free var list
  jsvCreateEmptyVarList();
  jshInterruptOn();
}

// Dump any locked variables that aren't referenced from `global` - for debugging memory leaks
void jsvDumpLockedVars() {
  jsvGarbageCollect();
  if (isMemoryBusy) return;
  isMemoryBusy = MEMBUSY_SYSTEM;
  JsVarRef i;
  // clear garbage collect flags
  for (i=1;i<=jsVarsSize;i++)  {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags&JSV_VARTYPEMASK) != JSV_UNUSED) { // if it is not unused
      var->flags |= (JsVarFlags)JSV_GARBAGE_COLLECT;
      // if we have a flat string, skip that many blocks
      if (jsvIsFlatString(var))
        i = (JsVarRef)(i+jsvGetFlatStringBlocks(var));
    }
  }
  // Add global
  jsvGarbageCollectMarkUsed(execInfo.root);
  // Now dump any that aren't used!
  for (i=1;i<=jsVarsSize;i++)  {
    JsVar *var = jsvGetAddressOf(i);
    if ((var->flags&JSV_VARTYPEMASK) != JSV_UNUSED) {
      if (var->flags & JSV_GARBAGE_COLLECT) {
        jsvGarbageCollectMarkUsed(var);
        jsvTrace(var, 0);
      }
    }
  }
  isMemoryBusy = MEM_NOT_BUSY;
}

// Dump the free list - in order
void jsvDumpFreeList() {
  JsVarRef ref = jsVarFirstEmpty;
  int n = 0;
  while (ref) {
    jsiConsolePrintf("%5d ", (int)ref);
    if (++n >= 16) {
      n = 0;
      jsiConsolePrintf("\n");
    }
    JsVar *v = jsvGetAddressOf(ref);
    ref = jsvGetNextSibling(v);
  }
  jsiConsolePrintf("\n");
}


/** Remove whitespace to the right of a string - on MULTIPLE LINES */
JsVar *jsvStringTrimRight(JsVar *srcString) {
  JsvStringIterator src, dst;
  JsVar *dstString = jsvNewFromEmptyString();
  jsvStringIteratorNew(&src, srcString, 0);
  jsvStringIteratorNew(&dst, dstString, 0);
  int spaces = 0;
  while (jsvStringIteratorHasChar(&src)) {
    char ch = jsvStringIteratorGetCharAndNext(&src);

    if (ch==' ') spaces++;
    else if (ch=='\n') {
      spaces = 0;
      jsvStringIteratorAppend(&dst, ch);
    } else {
      for (;spaces>0;spaces--)
        jsvStringIteratorAppend(&dst, ' ');
      jsvStringIteratorAppend(&dst, ch);
    }
  }
  jsvStringIteratorFree(&src);
  jsvStringIteratorFree(&dst);
  return dstString;
}

/// If v is the key of a function, return true if it is internal and shouldn't be visible to the user
bool jsvIsInternalFunctionKey(JsVar *v) {
  return (jsvIsString(v) && (
      v->varData.str[0]==JS_HIDDEN_CHAR)
  ) ||
  jsvIsFunctionParameter(v);
}

/// If v is the key of an object, return true if it is internal and shouldn't be visible to the user
bool jsvIsInternalObjectKey(JsVar *v) {
  return (jsvIsString(v) && (
      v->varData.str[0]==JS_HIDDEN_CHAR ||
      jsvIsStringEqual(v, JSPARSE_INHERITS_VAR) ||
      jsvIsStringEqual(v, JSPARSE_CONSTRUCTOR_VAR)
  ));
}

/// Get the correct checker function for the given variable. see jsvIsInternalFunctionKey/jsvIsInternalObjectKey
JsvIsInternalChecker jsvGetInternalFunctionCheckerFor(JsVar *v) {
  if (jsvIsFunction(v)) return jsvIsInternalFunctionKey;
  if (jsvIsObject(v)) return jsvIsInternalObjectKey;
  return 0;
}

/** Using 'configs', this reads 'object' into the given pointers, returns true on success.
 *  If object is not undefined and not an object, an error is raised.
 *  If there are fields that are not  in the list of configs, an error is raised
 */
bool jsvReadConfigObject(JsVar *object, jsvConfigObject *configs, int nConfigs) {
  if (jsvIsUndefined(object)) return true;
  if (!jsvIsObject(object)) {
    jsExceptionHere(JSET_ERROR, "Expecting an Object, or undefined");
    return false;
  }
  // Ok, it's an object
  JsvObjectIterator it;
  jsvObjectIteratorNew(&it, object);
  bool ok = true;
  while (ok && jsvObjectIteratorHasValue(&it)) {
    JsVar *key = jsvObjectIteratorGetKey(&it);
    bool found = false;
    for (int i=0;i<nConfigs;i++) {
      if (jsvIsStringEqual(key, configs[i].name)) {
        found = true;
        if (configs[i].ptr) {
          JsVar *val = jsvObjectIteratorGetValue(&it);
          switch (configs[i].type) {
          case 0: break;
          case JSV_OBJECT:
          case JSV_STRING_0:
          case JSV_ARRAY:
          case JSV_FUNCTION:
            *((JsVar**)configs[i].ptr) = jsvLockAgain(val); break;
          case JSV_PIN: *((Pin*)configs[i].ptr) = jshGetPinFromVar(val); break;
          case JSV_BOOLEAN: *((bool*)configs[i].ptr) = jsvGetBool(val); break;
          case JSV_INTEGER: *((JsVarInt*)configs[i].ptr) = jsvGetInteger(val); break;
          case JSV_FLOAT: *((JsVarFloat*)configs[i].ptr) = jsvGetFloat(val); break;
          default: assert(0); break;
          }
          jsvUnLock(val);
        }
      }
    }
    if (!found) {
      jsExceptionHere(JSET_ERROR, "Unknown option %q", key);
      ok = false;
    }
    jsvUnLock(key);

    jsvObjectIteratorNext(&it);
  }
  jsvObjectIteratorFree(&it);
  return ok;
}

/** Using data in the format passed to jsvReadConfigObject, reconstruct a new object  */
JsVar *jsvCreateConfigObject(jsvConfigObject *configs, int nConfigs) {
  JsVar *o = jsvNewObject();
  if (!o) return 0;
  for (int i=0;i<nConfigs;i++) {
     if (configs[i].ptr) {
      JsVar *v = 0;
      switch (configs[i].type) {
      case 0: break;
      case JSV_OBJECT:
      case JSV_STRING_0:
      case JSV_ARRAY:
      case JSV_FUNCTION:
        v = jsvLockAgain(*((JsVar**)configs[i].ptr)); break;
      case JSV_PIN:
        v = jsvNewFromPin(*((Pin*)configs[i].ptr)); break;
      case JSV_BOOLEAN:
        v = jsvNewFromBool(*((bool*)configs[i].ptr)); break;
      case JSV_INTEGER:
        v = jsvNewFromInteger(*((JsVarInt*)configs[i].ptr)); break;
      case JSV_FLOAT:
        v = jsvNewFromFloat(*((JsVarFloat*)configs[i].ptr)); break;
      default:
        assert(0);
        // ignore stuff we don't know about
        break;
      }
      jsvObjectSetChildAndUnLock(o, configs[i].name, v);
    }
  }
  return o;
}

/// Is the variable an instance of the given class. Eg. `jsvIsInstanceOf(e, "Error")` - does a simple, non-recursive check that doesn't take account of builtins like String
bool jsvIsInstanceOf(JsVar *var, const char *constructorName) {
  bool isInst = false;
  if (!jsvHasChildren(var)) return false;
  JsVar *proto = jsvObjectGetChild(var, JSPARSE_INHERITS_VAR, 0);
  if (jsvIsObject(proto)) {
    JsVar *constr = jsvObjectGetChild(proto, JSPARSE_CONSTRUCTOR_VAR, 0);
    if (constr)
      isInst = jspIsConstructor(constr, constructorName);
    jsvUnLock(constr);
  }
  jsvUnLock(proto);
  return isInst;
}

JsVar *jsvNewTypedArray(JsVarDataArrayBufferViewType type, JsVarInt length) {
  JsVar *lenVar = jsvNewFromInteger(length);
  if (!lenVar) return 0;
  JsVar *array = jswrap_typedarray_constructor(type, lenVar,0,0);
  jsvUnLock(lenVar);
  return array;
}

JsVar *jsvNewDataViewWithData(JsVarInt length, unsigned char *data) {
  JsVar *buf = jswrap_arraybuffer_constructor(length);
  if (!buf) return 0;
  JsVar *view = jswrap_dataview_constructor(buf, 0, 0);
  if (!view) {
    jsvUnLock(buf);
    return 0;
  }
  if (data) {
    JsVar *arrayBufferData = jsvGetArrayBufferBackingString(buf, NULL);
    if (arrayBufferData)
      jsvSetString(arrayBufferData, (char *)data, (size_t)length);
    jsvUnLock(arrayBufferData);
  }
  jsvUnLock(buf);
  return view;
}

JsVar *jsvNewArrayBufferWithPtr(unsigned int length, char **ptr) {
  assert(ptr);
  *ptr=0;
  JsVar *backingString = jsvNewFlatStringOfLength(length);
  if (!backingString) return 0;
  JsVar *arr = jsvNewArrayBufferFromString(backingString, length);
  if (!arr) {
    jsvUnLock(backingString);
    return 0;
  }
  *ptr = jsvGetFlatStringPointer(backingString);
  jsvUnLock(backingString);
  return arr;
}

JsVar *jsvNewArrayBufferWithData(JsVarInt length, unsigned char *data) {
  assert(data);
  assert(length>0);
  JsVar *dst = 0;
  JsVar *arr = jsvNewArrayBufferWithPtr((unsigned int)length, (char**)&dst);
  if (!dst) {
    jsvUnLock(arr);
    return 0;
  }
  memcpy(dst, data, (size_t)length);
  return arr;
}

void *jsvMalloc(size_t size) {
  assert(size>0);
  /** Allocate flat string, return pointer to its first element.
   * As we drop the pointer here, it's left locked. jsvGetFlatStringPointer
   * is also safe if 0 is passed in.  */
  JsVar *flatStr = jsvNewFlatStringOfLength((unsigned int)size);
  if (!flatStr) {
    jsErrorFlags |= JSERR_LOW_MEMORY;
    // Not allocated - try and free any command history/etc
    while (jsiFreeMoreMemory());
    // Garbage collect
    jsvGarbageCollect();
    // Try again
    flatStr = jsvNewFlatStringOfLength((unsigned int)size);
  }
  // intentionally no jsvUnLock - see above
  void *p = (void*)jsvGetFlatStringPointer(flatStr);
  if (p) {
    //jsiConsolePrintf("jsvMalloc var %d-%d at %d (%d bytes)\n", jsvGetRef(flatStr), jsvGetRef(flatStr)+jsvGetFlatStringBlocks(flatStr), p, size);
    memset(p,0,size);
  }
  return p;
}

void jsvFree(void *ptr) {
  JsVar *flatStr = jsvGetFlatStringFromPointer((char *)ptr);
  //jsiConsolePrintf("jsvFree var %d at %d (%d bytes)\n", jsvGetRef(flatStr), ptr, jsvGetLength(flatStr));

  jsvUnLock(flatStr);
}
