/*************************************************************************
    > File Name: rsa_error.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月14日 星期一 11时48分56秒
 ************************************************************************/

#ifndef __RSA_ERROR_H__
#define __RSA_ERROR_H__

enum {
    RSA_ERROR_NONE = 0,             // No error
    RSA_ERROR_BEGIN = -0xFFFF,      // Start of RSA error codes
    RSA_ERROR_NOT_INITIALIZED,      // RSA not initialized
    RSA_ERROR_INVALID_PARAMETER,    // Invalid parameter passed to function
    RSA_ERROR_INVALID_RSA_SIZE,     // Invalid RSA key size
    RSA_ERROR_INVALID_PADDING,      // Invalid padding scheme
    RSA_ERROR_INVALID_KEY,          // Invalid RSA key
    RSA_ERROR_ENCRYPTION_FAILED,    // Encryption failed
    RSA_ERROR_DECRYPTION_FAILED,    // Decryption failed
    RSA_ERROR_MEMORY_ALLOCATION,    // Memory allocation error
    RSA_ERROR_GENERATE_KEY,         // Key generation failed
    RSA_ERROR_UNKNOWN               // Unknown error
};

#define RSA_ERROR_MSG(x, m)             \
    switch (x) {                        \
    case RSA_ERROR_NONE:                \
        m = "Success";                  \
        break;                          \
    case RSA_ERROR_NOT_INITIALIZED:     \
        m = "RSA not initialized";      \
        break;                          \
    case RSA_ERROR_INVALID_PARAMETER:   \
        m = "Invalid parameter passed to function"; \
        break;                          \
    case RSA_ERROR_INVALID_RSA_SIZE:    \
        m = "Invalid RSA key size";     \
        break;                          \
    case RSA_ERROR_INVALID_PADDING:     \
        m = "Invalid padding scheme";   \
        break;                          \
    case RSA_ERROR_INVALID_KEY:         \
        m = "Invalid RSA key";          \
        break;                          \
    case RSA_ERROR_ENCRYPTION_FAILED:   \
        m = "Encryption failed";        \
        break;                          \
    case RSA_ERROR_DECRYPTION_FAILED:   \
        m = "Decryption failed";        \
        break;                          \
    case RSA_ERROR_MEMORY_ALLOCATION:   \
        m = "Memory allocation error";  \
        break;                          \
    case RSA_ERROR_GENERATE_KEY:        \
        m = "Key generation failed";    \
        break;                          \
    case RSA_ERROR_UNKNOWN:             \
        m = "Unknown error";            \
        break;                          \
    default:                            \
        m = NULL;                       \ 
        break;                          \
    };

#define MIN(x, y) ((x) < (y) ? (x) : (y))

#endif // __RSA_ERROR_H__
