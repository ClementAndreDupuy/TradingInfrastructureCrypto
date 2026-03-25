#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
typedef void CURL;
typedef void CURLM;

struct curl_slist {
    char *data;
    curl_slist *next;
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

typedef enum {
    CURLOPT_WRITEDATA = 10001, 
    CURLOPT_URL = 10002, 
    CURLOPT_TIMEOUT = 13, 
    CURLOPT_POSTFIELDS = 10015, 
    CURLOPT_HTTPHEADER = 10023, 
    CURLOPT_CUSTOMREQUEST = 10036, 
    CURLOPT_POST = 47, 
    CURLOPT_FOLLOWLOCATION = 52, 
    CURLOPT_POSTFIELDSIZE = 60, 
    CURLOPT_CONNECTTIMEOUT = 78, 
    CURLOPT_HTTPGET = 80, 
    CURLOPT_WRITEFUNCTION = 20011, 
    CURLOPT_TCP_KEEPALIVE = 213, 
    CURLOPT_SSL_VERIFYPEER = 64, 
    CURLOPT_SSL_VERIFYHOST = 81, 
} CURLoption;

typedef enum {
    CURLINFO_RESPONSE_CODE = 0x200000 + 2, 
} CURLINFO;

typedef size_t (*curl_write_callback)(char *ptr, size_t size, size_t nmemb, void *userdata);

CURLcode curl_global_init(long flags);

void curl_global_cleanup(void);

CURL *curl_easy_init(void);

void curl_easy_cleanup(CURL *handle);

CURLcode curl_easy_perform(CURL *handle);

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);

curl_slist *curl_slist_append(curl_slist *list, const char *string);

void curl_slist_free_all(curl_slist *list);
} 
