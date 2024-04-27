#ifndef __CHROMIUM_BASE64_H__
#define __CHROMIUM_BASE64_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODP_B64_ERROR ((uint64_t)-1)
/**
 * Encode a raw binary string into base 64.
 * src contains the bytes
 * len contains the number of bytes in the src
 * dest should be allocated by the caller to contain
 *   at least chromium_base64_encode_len(len) bytes (see below)
 *   This will contain the null-terminated b64 encoded result
 * returns length of the destination string plus the ending null byte
 *    i.e.  the result will be equal to strlen(dest) + 1
 *
 * Example
 *
 * \code
 * char* src = ...;
 * int srclen = ...; //the length of number of bytes in src
 * char* dest = (char*) malloc(chromium_base64_encode_len(srclen));
 * int len = chromium_base64_encode(dest, src, sourcelen);
 * if (len == MODP_B64_ERROR) {
 *   printf("Error\n");
 * } else {
 *   printf("b64 = %s\n", dest);
 * }
 * \endcode
 *
 */
uint64_t chromium_base64_encode(void *dest, const void *str, uint64_t len);

/**
 * Decode a base64 encoded string
 *
 *
 * src should contain exactly len bytes of b64 characters.
 *     if src contains -any- non-base characters (such as white
 *     space, MODP_B64_ERROR is returned.
 *
 * dest should be allocated by the caller to contain at least
 *    len * 3 / 4 bytes.
 *
 * Returns the length (strlen) of the output, or MODP_B64_ERROR if unable to
 * decode
 *
 * \code
 * char* src = ...;
 * int srclen = ...; // or if you don't know use strlen(src)
 * char* dest = (char*) malloc(chromium_base64_decode_len(srclen));
 * int len = chromium_base64_decode(dest, src, sourcelen);
 * if (len == MODP_B64_ERROR) { error }
 * \endcode
 */
uint64_t chromium_base64_decode(void *dest, const void *src, uint64_t len);

/**
 * Given a source string of length len, this returns the amount of
 * memory the destination string should have.
 *
 * remember, this is integer math
 * 3 bytes turn into 4 chars
 * ceiling[len / 3] * 4 + 1
 *
 * +1 is for any extra null.
 */
#define chromium_base64_encode_len(A) ((A + 2) / 3 * 4 + 1)

/**
 * Given a base64 string of length len,
 *   this returns the amount of memory required for output string
 *  It maybe be more than the actual number of bytes written.
 * NOTE: remember this is integer math
 * this allocates a bit more memory than traditional versions of b64
 * decode  4 chars turn into 3 bytes
 * floor[len * 3/4] + 2
 */
#define chromium_base64_decode_len(A) (A / 4 * 3 + 2)

/**
 * Will return the strlen of the output from encoding.
 * This may be less than the required number of bytes allocated.
 *
 * This allows you to 'deserialized' a struct
 * \code
 * char* b64encoded = "...";
 * int len = strlen(b64encoded);
 *
 * struct datastuff foo;
 * if (chromium_base64_encode_strlen(sizeof(struct datastuff)) != len) {
 *    // wrong size
 *    return false;
 * } else {
 *    // safe to do;
 *    if (chromium_base64_encode((char*) &foo, b64encoded, len) == MODP_B64_ERROR) {
 *      // bad characters
 *      return false;
 *    }
 * }
 * // foo is filled out now
 * \endcode
 */
#define chromium_base64_encode_strlen(A) ((A + 2) / 3 * 4)

#ifdef __cplusplus
}
#endif

#endif // __CHROMIUM_BASE64_H__