#pragma once
// Minimal libcurl ABI declarations for libcurl 7.x / 8.x.
// Used when libcurl4-openssl-dev is not installed but the runtime library is.
// Values match the stable libcurl ABI; do not modify.
//
// In environments with libcurl4-openssl-dev installed, include <curl/curl.h>
// instead. This file is a zero-dependency fallback for build environments.

#include <cstddef>
#include <cstdint>

extern "C" {

typedef void CURL;
typedef void CURLM;

struct curl_slist {
    char* data;
    curl_slist* next;
};

typedef enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_SSL_CONNECT_ERROR = 35,
} CURLcode;

typedef enum {
    CURL_GLOBAL_SSL = (1 << 0),
    CURL_GLOBAL_WIN32 = (1 << 1),
    CURL_GLOBAL_DEFAULT = CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32,
    CURL_GLOBAL_ALL = CURL_GLOBAL_DEFAULT,
    CURL_GLOBAL_NOTHING = 0,
} CURLglobal;

// CURLoption — only the subset used by rest_client.hpp.
// Values: CURLOPTTYPE_LONG=0, CURLOPTTYPE_OBJECTPOINT=10000,
//         CURLOPTTYPE_FUNCTIONPOINT=20000.
typedef enum {
    CURLOPT_WRITEDATA = 10001,     // OBJECTPOINT + 1
    CURLOPT_URL = 10002,           // OBJECTPOINT + 2
    CURLOPT_TIMEOUT = 13,          // LONG + 13
    CURLOPT_POSTFIELDS = 10015,    // OBJECTPOINT + 15
    CURLOPT_HTTPHEADER = 10023,    // OBJECTPOINT + 23
    CURLOPT_CUSTOMREQUEST = 10036, // OBJECTPOINT + 36
    CURLOPT_POST = 47,             // LONG + 47
    CURLOPT_FOLLOWLOCATION = 52,   // LONG + 52
    CURLOPT_POSTFIELDSIZE = 60,    // LONG + 60
    CURLOPT_CONNECTTIMEOUT = 78,   // LONG + 78
    CURLOPT_HTTPGET = 80,          // LONG + 80
    CURLOPT_WRITEFUNCTION = 20011, // FUNCTIONPOINT + 11
    CURLOPT_TCP_KEEPALIVE = 213,   // LONG + 213
    CURLOPT_SSL_VERIFYPEER = 64,   // LONG + 64
    CURLOPT_SSL_VERIFYHOST = 81,   // LONG + 81
} CURLoption;

// CURLINFO — only CURLINFO_RESPONSE_CODE used.
// CURLINFO_LONG = 0x200000
typedef enum {
    CURLINFO_RESPONSE_CODE = 0x200000 + 2, // CURLINFO_LONG + 2
} CURLINFO;

typedef size_t (*curl_write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL* curl_easy_init(void);
void curl_easy_cleanup(CURL* handle);
CURLcode curl_easy_perform(CURL* handle);
CURLcode curl_easy_setopt(CURL* handle, CURLoption option, ...);
CURLcode curl_easy_getinfo(CURL* handle, CURLINFO info, ...);

curl_slist* curl_slist_append(curl_slist* list, const char* string);
void curl_slist_free_all(curl_slist* list);

} // extern "C"
