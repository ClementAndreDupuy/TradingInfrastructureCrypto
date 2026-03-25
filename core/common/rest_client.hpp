#pragma once

#include <chrono>
#include <cstdlib>
#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
#include <functional>
#include <utility>
#endif
#include <string>
#include <vector>
#ifdef __has_include
#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#else
#include "curl_abi.hpp"
#endif
#else
#include "curl_abi.hpp"
#endif

namespace trading {
    namespace http {
        struct HttpResponse {
            int status{0};
            std::string body;
            bool ok() const { return status >= 200 && status < 300; }
        };

        namespace detail {
            struct CurlGlobal {
                CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
                ~CurlGlobal() { curl_global_cleanup(); }
            };

            inline const CurlGlobal k_curl_global;

            struct ThreadCurl {
                CURL *h{nullptr};

                ThreadCurl() : h(curl_easy_init()) {
                }

                ~ThreadCurl() {
                    if (h)
                        curl_easy_cleanup(h);
                }
            };

            inline CURL *tl_handle() {
                thread_local ThreadCurl tc;
                return tc.h;
            }

            inline size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
                static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
                return size * nmemb;
            }

            inline HttpResponse perform(CURL *h, const std::vector<std::string> &headers) {
                HttpResponse resp;

                curl_slist *hlist = nullptr;
                for (const auto &hdr: headers)
                    hlist = curl_slist_append(hlist, hdr.c_str());
                if (hlist)
                    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hlist);

                curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
                curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp.body);
                curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
                curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
                curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
                curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);

                CURLcode rc = curl_easy_perform(h);
                if (rc == CURLE_OK) {
                    long code = 0;
                    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
                    resp.status = static_cast<int>(code);
                }

                if (hlist) {
                    curl_slist_free_all(hlist);
                    curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
                }
                return resp;
            }
        } 

#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
        using MockTransport =
        std::function<HttpResponse(const char*method, const std::string & url, const std::string & body,
        const std::vector<std::string> &headers)>;

        inline MockTransport &mock_transport_slot() {
            static MockTransport mock_transport;
            return mock_transport;
        }
#endif

        

        inline HttpResponse get(const std::string &url, const std::vector<std::string> &headers) {
#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
            if (mock_transport_slot())
                return mock_transport_slot()("GET", url, "", headers);
#endif
            CURL *h = detail::tl_handle();
            if (!h)
                return {};
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
            return detail::perform(h, headers);
        }

        inline HttpResponse post(const std::string &url, const std::string &body,
                                 const std::vector<std::string> &headers) {
#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
            if (mock_transport_slot())
                return mock_transport_slot()("POST", url, body, headers);
#endif
            CURL *h = detail::tl_handle();
            if (!h)
                return {};
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            return detail::perform(h, headers);
        }

        inline HttpResponse put(const std::string &url, const std::string &body,
                                const std::vector<std::string> &headers) {
#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
            if (mock_transport_slot())
                return mock_transport_slot()("PUT", url, body, headers);
#endif
            CURL *h = detail::tl_handle();
            if (!h)
                return {};
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            return detail::perform(h, headers);
        }

        inline HttpResponse del(const std::string &url, const std::vector<std::string> &headers) {
#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
            if (mock_transport_slot())
                return mock_transport_slot()("DELETE", url, "", headers);
#endif
            CURL *h = detail::tl_handle();
            if (!h)
                return {};
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
            return detail::perform(h, headers);
        }

        

        inline int64_t now_ns() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                    .count();
        }

        inline int64_t now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                    .count();
        }

        

        inline std::string env_var(const char *name) {
            const char *v = std::getenv(name);
            return v ? v : "";
        }

#if defined(TRT_ENABLE_HTTP_MOCK_TRANSPORT)
        inline void set_mock_transport(MockTransport handler) {
            mock_transport_slot() = std::move(handler);
        }

        inline void clear_mock_transport() { mock_transport_slot() = nullptr; }
#endif
    } 
} 
