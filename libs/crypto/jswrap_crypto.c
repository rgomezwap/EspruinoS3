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
 * Contains cryptography functions like AES
 * ----------------------------------------------------------------------------
 */
#include "jsvar.h"
#include "jsvariterator.h"
#include "jswrap_crypto.h"
#include "jsparse.h"

#ifdef USE_AES
#include "mbedtls/include/mbedtls/aes.h"
#endif
#ifndef USE_SHA1_JS
#include "mbedtls/include/mbedtls/sha1.h"
#endif
#ifdef USE_SHA256
#include "mbedtls/include/mbedtls/sha256.h"
#endif
#ifdef USE_SHA512
#include "mbedtls/include/mbedtls/sha512.h"
#endif
#include "mbedtls/include/mbedtls/pkcs5.h"
#ifdef USE_TLS
#include "mbedtls/include/mbedtls/pk.h"
#include "mbedtls/include/mbedtls/x509.h"
#include "mbedtls/include/mbedtls/ssl.h"
#endif


/*JSON{
  "type" : "library",
  "class" : "crypto",
  "ifdef" : "USE_CRYPTO"
}
Cryptographic functions

**Note:** This library is currently only included in builds for boards where
there is space. For other boards there is `crypto.js` which implements SHA1 in
JS.
*/


/*JSON{
  "type" : "class",
  "library" : "crypto",
  "class" : "AES",
  "ifdef" : "USE_AES"
}
Class containing AES encryption/decryption

**Note:** This library is currently only included in builds for boards where
there is space. For other boards there is `crypto.js` which implements SHA1 in
JS.
*/
/*JSON{
  "type" : "staticproperty",
  "class" : "crypto",
  "name" : "AES",
  "generate_full" : "jspNewBuiltin(\"AES\");",
  "return" : ["JsVar"],
  "return_object" : "AES",
  "ifdef" : "USE_AES"
}
Class containing AES encryption/decryption
*/

const char *jswrap_crypto_error_to_str(int err) {
  switch(err) {
#ifdef USE_TLS
    case MBEDTLS_ERR_X509_INVALID_FORMAT:
    case MBEDTLS_ERR_PK_KEY_INVALID_FORMAT:
      return "Invalid format";
    case MBEDTLS_ERR_SSL_PK_TYPE_MISMATCH:
      return "Public key type mismatch";
    case MBEDTLS_ERR_X509_ALLOC_FAILED:
    case MBEDTLS_ERR_SSL_ALLOC_FAILED:
    case MBEDTLS_ERR_PK_ALLOC_FAILED:
#endif
    case MBEDTLS_ERR_MD_ALLOC_FAILED: return "Not enough memory";
    case MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE: return "Feature unavailable";
    case MBEDTLS_ERR_MD_BAD_INPUT_DATA: return "Bad input data";
#ifdef USE_AES	
    case MBEDTLS_ERR_AES_INVALID_INPUT_LENGTH: return "Invalid input length";
#endif
  }
  return 0;
}

JsVar *jswrap_crypto_error_to_jsvar(int err) {
  const char *e = jswrap_crypto_error_to_str(err);
  if (e) return jsvNewFromString(e);
  return jsvVarPrintf("-0x%x", -err);
}

void jswrap_crypto_error(int err) {
  const char *e = jswrap_crypto_error_to_str(err);
  if (e) jsError("%s", e);
  else jsError("Unknown error: -0x%x", -err);
}

typedef enum {
  CM_NONE,
  CM_CBC,
  CM_CFB,
  CM_CTR,
  CM_OFB,
  CM_ECB,
} CryptoMode;

CryptoMode jswrap_crypto_getMode(JsVar *mode) {
  if (jsvIsStringEqual(mode, "CBC")) return CM_CBC;
  if (jsvIsStringEqual(mode, "CFB")) return CM_CFB;
  if (jsvIsStringEqual(mode, "CTR")) return CM_CTR;
  if (jsvIsStringEqual(mode, "OFB")) return CM_OFB;
  if (jsvIsStringEqual(mode, "ECB")) return CM_ECB;
  jsExceptionHere(JSET_ERROR, "Unknown Crypto mode %q", mode);
  return CM_NONE;
}

mbedtls_md_type_t jswrap_crypto_getHasher(JsVar *hasher) {
#ifndef USE_SHA1_JS
  if (jsvIsStringEqual(hasher, "SHA1")) return MBEDTLS_MD_SHA1;
#endif
#ifdef USE_SHA256
  if (jsvIsStringEqual(hasher, "SHA224")) return MBEDTLS_MD_SHA224;
  if (jsvIsStringEqual(hasher, "SHA256")) return MBEDTLS_MD_SHA256;
#endif
#ifdef USE_SHA512
  if (jsvIsStringEqual(hasher, "SHA384")) return MBEDTLS_MD_SHA384;
  if (jsvIsStringEqual(hasher, "SHA512")) return MBEDTLS_MD_SHA512;
#endif
  jsExceptionHere(JSET_ERROR, "Unknown Hasher %q", hasher);
  return MBEDTLS_MD_NONE;
}

JsVar *jswrap_crypto_SHAx(JsVar *message, int shaNum) {
#ifdef USE_SHA1_JS
  if (shaNum==1) {
    // (c) 2016 Rhys Williams, @jumjum. https://github.com/espruino/EspruinoDocs/blob/master/modules/crypto.js
    return jspExecuteJSFunction("(function(b){function n(a){for(d=3;0<=d;d--)g.push(a>>8*d&255)}var d,a;b=E.toString(b)+'\\x80';var v=new Int32Array([1518500249,1859775393,2400959708,3395469782]);var k=Math.ceil((b.length/4+2)/16);var g=Array(k);b=E.toUint8Array(b);for(d=0;d<k;d++){var f=d<<6;var e=new Int32Array(16);for(a=0;16>a;a++){var c=f+(a<<2);e[a]=b[c]<<24|b[c+1]<<16|b[c+2]<<8|b[c+3]}g[d]=e}g[k-1][14]=8*(b.length-1)/Math.pow(2,32);g[k-1][14]=Math.floor(g[k-1][14]);g[k-1][15]=8*(b.length-1)&4294967295;b=1732584193;var p=4023233417;var q=2562383102;var r=271733878;var t=3285377520;var l=new Int32Array(80);for(d=0;d<k;d++){for(a=0;16>a;a++)l[a]=g[d][a];for(a=16;80>a;a++)f=l[a-3]^l[a-8]^l[a-14]^l[a-16],l[a]=f<<1|f>>>31;f=b;c=p;e=q;var h=r;var u=t;for(a=0;80>a;a++){var m=Math.floor(a/20);var w=f<<5|f>>>27;var x=0===m?c&e^~c&h:1===m?c^e^h:2===m?c&e^c&h^e&h:c^e^h;m=w+x+u+v[m]+l[a]&4294967295;u=h;h=e;e=c<<30|c>>>2;c=f;f=m}b=b+f&4294967295;p=p+c&4294967295;q=q+e&4294967295;r=r+h&4294967295;t=t+u&4294967295}g=[];n(b);n(p);n(q);n(r);n(t);return E.toUint8Array(g).buffer})",0,1,&message);
  }
#endif

  JSV_GET_AS_CHAR_ARRAY(msgPtr, msgLen, message);
  if (!msgPtr) return 0;

  int bufferSize = 20;
  if (shaNum>1) bufferSize = shaNum/8;

  char *outPtr = 0;
  JsVar *outArr = jsvNewArrayBufferWithPtr((unsigned int)bufferSize, &outPtr);
  if (!outPtr) {
    jsError("Not enough memory for result");
    return 0;
  }

#ifndef USE_SHA1_JS
  if (shaNum==1) mbedtls_sha1((unsigned char *)msgPtr, msgLen, (unsigned char *)outPtr);
#endif
#ifdef USE_SHA256
  else if (shaNum==224) mbedtls_sha256((unsigned char *)msgPtr, msgLen, (unsigned char *)outPtr, true/*224*/);
  else if (shaNum==256) mbedtls_sha256((unsigned char *)msgPtr, msgLen, (unsigned char *)outPtr, false/*256*/);
#endif
#ifdef USE_SHA512
  else if (shaNum==384) mbedtls_sha512((unsigned char *)msgPtr, msgLen, (unsigned char *)outPtr, true/*384*/);
  else if (shaNum==512) mbedtls_sha512((unsigned char *)msgPtr, msgLen, (unsigned char *)outPtr, false/*512*/);
#endif
  return outArr;
}

/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "SHA1",
  "generate_full" : "jswrap_crypto_SHAx(message, 1)",
  "params" : [
    ["message","JsVar","The message to apply the hash to"]
  ],
  "return" : ["JsVar","Returns a 20 byte ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_CRYPTO"
}

Performs a SHA1 hash and returns the result as a 20 byte ArrayBuffer.

**Note:** On some boards (currently only Espruino Original) there isn't space
for a fully unrolled SHA1 implementation so a slower all-JS implementation is
used instead.
*/
/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "SHA224",
  "generate_full" : "jswrap_crypto_SHAx(message, 224)",
  "params" : [
    ["message","JsVar","The message to apply the hash to"]
  ],
  "return" : ["JsVar","Returns a 20 byte ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_SHA256"
}

Performs a SHA224 hash and returns the result as a 28 byte ArrayBuffer
*/
/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "SHA256",
  "generate_full" : "jswrap_crypto_SHAx(message, 256)",
  "params" : [
    ["message","JsVar","The message to apply the hash to"]
  ],
  "return" : ["JsVar","Returns a 20 byte ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_SHA256"
}

Performs a SHA256 hash and returns the result as a 32 byte ArrayBuffer
*/
/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "SHA384",
  "generate_full" : "jswrap_crypto_SHAx(message, 384)",
  "params" : [
    ["message","JsVar","The message to apply the hash to"]
  ],
  "return" : ["JsVar","Returns a 20 byte ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_SHA512"
}

Performs a SHA384 hash and returns the result as a 48 byte ArrayBuffer
*/
/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "SHA512",
  "generate_full" : "jswrap_crypto_SHAx(message, 512)",
  "params" : [
    ["message","JsVar","The message to apply the hash to"]
  ],
  "return" : ["JsVar","Returns a 32 byte ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_SHA512"
}

Performs a SHA512 hash and returns the result as a 64 byte ArrayBuffer
*/

#ifdef USE_TLS
/*JSON{
  "type" : "staticmethod",
  "class" : "crypto",
  "name" : "PBKDF2",
  "generate" : "jswrap_crypto_PBKDF2",
  "params" : [
    ["passphrase","JsVar","Passphrase"],
    ["salt","JsVar","Salt for turning passphrase into a key"],
    ["options","JsVar","Object of Options, `{ keySize: 8 (in 32 bit words), iterations: 10, hasher: 'SHA1'/'SHA224'/'SHA256'/'SHA384'/'SHA512' }`"]
  ],
  "return" : ["JsVar","Returns an ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_TLS"
}

Password-Based Key Derivation Function 2 algorithm, using SHA512
*/
JsVar *jswrap_crypto_PBKDF2(JsVar *passphrase, JsVar *salt, JsVar *options) {
  int iterations = 1;
  int keySize = 128/32;
  mbedtls_md_type_t hasher = MBEDTLS_MD_SHA1;


  if (jsvIsObject(options)) {
    keySize = jsvGetIntegerAndUnLock(jsvObjectGetChild(options, "keySize", 0));
    if (keySize<=0) keySize=128/32;
    iterations = jsvGetIntegerAndUnLock(jsvObjectGetChild(options, "iterations", 0));
    if (iterations<1) iterations = 1;

    JsVar *hashVar = jsvObjectGetChild(options, "hasher", 0);
    if (!jsvIsUndefined(hashVar))
      hasher = jswrap_crypto_getHasher(hashVar);
    jsvUnLock(hashVar);

  } else if (!jsvIsUndefined(options))
    jsError("Options should be an object or undefined, got %t", options);

  if (hasher == MBEDTLS_MD_NONE)
    return 0; // already shown an error

  JSV_GET_AS_CHAR_ARRAY(passPtr, passLen, passphrase);
  if (!passPtr) return 0;
  JSV_GET_AS_CHAR_ARRAY(saltPtr, saltLen, salt);
  if (!saltPtr) return 0;

  int err;
  mbedtls_md_context_t ctx;

  mbedtls_md_init( &ctx );
  err = mbedtls_md_setup( &ctx, mbedtls_md_info_from_type( hasher ), 1 );
  assert(err==0);

  char *keyPtr = 0;
  JsVar *keyArr = jsvNewArrayBufferWithPtr((unsigned)keySize*4, &keyPtr);
  if (!keyPtr) {
    jsError("Not enough memory for result");
    return 0;
  }

  err = mbedtls_pkcs5_pbkdf2_hmac( &ctx,
        (unsigned char*)passPtr, passLen,
        (unsigned char*)saltPtr, saltLen,
        (unsigned)iterations,
        (unsigned)keySize*4, (unsigned char*)keyPtr );
  mbedtls_md_free( &ctx );
  if (!err) {
    return keyArr;
  } else {
    jswrap_crypto_error(err);
    jsvUnLock(keyArr);
    return 0;
  }
}
#endif

#ifdef USE_AES
static NO_INLINE JsVar *jswrap_crypto_AEScrypt(JsVar *message, JsVar *key, JsVar *options, bool encrypt) {
  int err;

  unsigned char iv[16]; // initialisation vector
  memset(iv, 0, 16);

  CryptoMode mode = CM_CBC;

  if (jsvIsObject(options)) {
    JsVar *ivVar = jsvObjectGetChild(options, "iv", 0);
    if (ivVar) {
      jsvIterateCallbackToBytes(ivVar, iv, sizeof(iv));
      jsvUnLock(ivVar);
    }
    JsVar *modeVar = jsvObjectGetChild(options, "mode", 0);
    if (!jsvIsUndefined(modeVar))
      mode = jswrap_crypto_getMode(modeVar);
    jsvUnLock(modeVar);
    if (mode == CM_NONE) return 0;
  } else if (!jsvIsUndefined(options)) {
    jsError("'options' must be undefined, or an Object");
    return 0;
  }



  mbedtls_aes_context aes;
  mbedtls_aes_init( &aes );

  JSV_GET_AS_CHAR_ARRAY(messagePtr, messageLen, message);
  if (!messagePtr) return 0;

  JSV_GET_AS_CHAR_ARRAY(keyPtr, keyLen, key);
  if (!keyPtr) return 0;

  if (encrypt)
    err = mbedtls_aes_setkey_enc( &aes, (unsigned char*)keyPtr, (unsigned int)keyLen*8 );
  else
    err = mbedtls_aes_setkey_dec( &aes, (unsigned char*)keyPtr, (unsigned int)keyLen*8 );
  if (err) {
    jswrap_crypto_error(err);
    return 0;
  }

  char *outPtr = 0;
  JsVar *outVar = jsvNewArrayBufferWithPtr((unsigned int)messageLen, &outPtr);
  if (!outPtr) {
    jsError("Not enough memory for result");
    return 0;
  }



  switch (mode) {
  case CM_CBC:
    err = mbedtls_aes_crypt_cbc( &aes,
                     encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                     messageLen,
                     iv,
                     (unsigned char*)messagePtr,
                     (unsigned char*)outPtr );
    break;
  case CM_CFB:
    err = mbedtls_aes_crypt_cfb8( &aes,
                     encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                     messageLen,
                     iv,
                     (unsigned char*)messagePtr,
                     (unsigned char*)outPtr );
    break;
  case CM_CTR: {
    size_t nc_off = 0;
    unsigned char nonce_counter[16];
    unsigned char stream_block[16];
    memset(nonce_counter, 0, sizeof(nonce_counter));
    memset(stream_block, 0, sizeof(stream_block));
    err = mbedtls_aes_crypt_ctr( &aes,
                     messageLen,
                     &nc_off,
                     nonce_counter,
                     stream_block,
                     (unsigned char*)messagePtr,
                     (unsigned char*)outPtr );
    break;
  }
  case CM_ECB: {
    size_t i = 0;
    while (!err && i+15 < messageLen) {
      err = mbedtls_aes_crypt_ecb( &aes,
                       encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                       (unsigned char*)&messagePtr[i],
                       (unsigned char*)&outPtr[i] );
      i += 16;
    }
    break;
  }
  default:
    err = MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE;
    break;
  }

  mbedtls_aes_free( &aes );
  if (!err) {
    return outVar;
  } else {
    jswrap_crypto_error(err);
    jsvUnLock(outVar);
    return 0;
  }
}

/*JSON{
  "type" : "staticmethod",
  "class" : "AES",
  "name" : "encrypt",
  "generate" : "jswrap_crypto_AES_encrypt",
  "params" : [
    ["passphrase","JsVar","Message to encrypt"],
    ["key","JsVar","Key to encrypt message - must be an ArrayBuffer of 128, 192, or 256 BITS"],
    ["options","JsVar","An optional object, may specify `{ iv : new Uint8Array(16), mode : 'CBC|CFB|CTR|OFB|ECB' }`"]
  ],
  "return" : ["JsVar","Returns an ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_AES"
}
*/
JsVar *jswrap_crypto_AES_encrypt(JsVar *message, JsVar *key, JsVar *options) {
  return jswrap_crypto_AEScrypt(message, key, options, true);
}

/*JSON{
  "type" : "staticmethod",
  "class" : "AES",
  "name" : "decrypt",
  "generate" : "jswrap_crypto_AES_decrypt",
  "params" : [
    ["passphrase","JsVar","Message to decrypt"],
    ["key","JsVar","Key to encrypt message - must be an ArrayBuffer of 128, 192, or 256 BITS"],
    ["options","JsVar","An optional object, may specify `{ iv : new Uint8Array(16), mode : 'CBC|CFB|CTR|OFB|ECB' }`"]
  ],
  "return" : ["JsVar","Returns an ArrayBuffer"],
  "return_object" : "ArrayBuffer",
  "ifdef" : "USE_AES"
}
*/
JsVar *jswrap_crypto_AES_decrypt(JsVar *message, JsVar *key, JsVar *options) {
  return jswrap_crypto_AEScrypt(message, key, options, false);
}
#endif
