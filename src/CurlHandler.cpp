//
// Created by mctrivia on 01/02/24.
//

// CurlHandler.cpp
// Implementation of the CurlHandler namespace HTTP helpers. libcurl is globally
// initialized once via a static_block. Each thread lazily creates and reuses a
// single CURL easy handle (thread_local) so the underlying session/connection
// (and TCP handshake) is preserved across calls on that thread. Response bodies
// are accumulated into a std::string or streamed to a FILE via write callbacks.

#include "CurlHandler.h"
#include "static_block.hpp"
#include <atomic>
#include <map>
#include <stdexcept>

using namespace std;


// Static block to register our callback function with IPFS Controller
static_block {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

namespace CurlHandler {
    namespace {
        // Thread-local CURL handle - reused across calls to preserve
        // WinHTTP session and connection handles (avoids TCP handshake per request)
        thread_local CURL* tl_curl = nullptr;

        // Returns this thread's reusable CURL easy handle, creating it on first
        // use and otherwise resetting all previously-set options (so a prior
        // request's settings don't leak into the next) while keeping the live
        // connection. Returns nullptr only if curl_easy_init() fails.
        CURL* acquireHandle() {
            if (!tl_curl) {
                tl_curl = curl_easy_init();
            } else {
                curl_easy_reset(tl_curl);
            }
            return tl_curl;
        }

        // Discard this thread's reusable handle so the next acquireHandle() makes
        // a fresh one. This MUST be used instead of a bare curl_easy_cleanup() on
        // an acquireHandle() handle: cleaning the handle up without nulling
        // tl_curl leaves a DANGLING thread-local pointer that the very next call
        // reuses via curl_easy_reset() -> use-after-free -> heap corruption. That
        // was the win.104 crash. Only call after an error/timeout where the
        // handle's state is suspect; a successful transfer keeps the handle for
        // connection reuse.
        void discardHandle() {
            if (tl_curl) {
                curl_easy_cleanup(tl_curl);
                tl_curl = nullptr;
            }
        }

        /**
         * Private callback function for curl to build a string with returned data
         */
        size_t writeCallback(void* contents, size_t size, size_t nmemb, string* s) {
            size_t newLength = size * nmemb;
            try {
                s->append((char*) contents, newLength);
                return newLength;
            } catch (bad_alloc& e) {
                // handle memory problem
                return 0;
            }
        }

        /**
         * Private callback function for curl to build a file with returned data
         */
        size_t writeData(void* ptr, size_t size, size_t nmemb, FILE* stream) {
            size_t written = fwrite(ptr, size, nmemb, stream);
            return written;
        }

        std::atomic<bool> _abortAll{false};

        //curl calls this roughly once a second during a transfer; nonzero return aborts
        int progressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            return _abortAll ? 1 : 0;
        }

        //attach the abort check to a request(curl disables progress callbacks by default)
        void applyAbortCheck(CURL* curl) {
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        }
    } // namespace

    void abortAllTransfers(bool abort) {
        _abortAll = abort;
    }

    // Blocking HTTP GET. Accumulates the response body into a string and
    // returns it. timeout in ms (0 disables). Throws exceptionTimeout on
    // timeout, runtime_error on handle-init failure or other curl errors.
    string get(const string& url, unsigned int timeout) {
        CURL* curl = acquireHandle();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        applyAbortCheck(curl);
        CURLcode res = curl_easy_perform(curl);
        if ((res == CURLE_OPERATION_TIMEDOUT) || (res == CURLE_ABORTED_BY_CALLBACK)) {
            discardHandle();
            throw exceptionTimeout();
        }
        if (res != CURLE_OK) {
            discardHandle();
            throw runtime_error(curl_easy_strerror(res));
        }
        return readBuffer;   // success: keep the thread-local handle for reuse
    }

    // Blocking HTTP POST. Builds an "&"-joined key=value body from data, sends
    // it, and returns the response body. timeout in ms (0 disables). Throws
    // exceptionTimeout on timeout, runtime_error on other curl errors.
    // Note: keys/values are not URL-encoded by this helper.
    string post(const string& url, const map<string, string>& data, unsigned int timeout) {
        //preprocess post data
        string postData;
        for (const auto& entry: data) {
            if (!postData.empty()) {
                postData += "&";
            }
            postData += entry.first + "=" + entry.second;
        }

        CURL* curl = acquireHandle();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        applyAbortCheck(curl);
        CURLcode res = curl_easy_perform(curl);
        if ((res == CURLE_OPERATION_TIMEDOUT) || (res == CURLE_ABORTED_BY_CALLBACK)) {
            discardHandle();
            throw exceptionTimeout();
        }
        if (res != CURLE_OK) {
            discardHandle();
            throw runtime_error(curl_easy_strerror(res));
        }
        return readBuffer;   // success: keep the thread-local handle for reuse
    }

    /**
     * Makes a multipart/form-data post request with content uploaded as a file attachment.
     * Needed for the IPFS "add" api endpoint.
     */
    string postFile(const string& url, const string& fieldName, const string& fileName,
                    const string& content, unsigned int timeout) {
        //check CURL is installed and get a thread safe instance of it
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        //build multipart form with the content as an in memory file
        curl_mime* mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, fieldName.c_str());
        curl_mime_filename(part, fileName.c_str());
        curl_mime_data(part, content.data(), content.size());

        //make post request
        string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        applyAbortCheck(curl);
        CURLcode res = curl_easy_perform(curl);
        curl_mime_free(mime);
        if ((res == CURLE_OPERATION_TIMEDOUT) || (res == CURLE_ABORTED_BY_CALLBACK)) {
            curl_easy_cleanup(curl);
            throw exceptionTimeout();
        }
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            throw runtime_error(curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
        return readBuffer;
    }

    // Blocking POST of a raw JSON body (Content-Type: application/json). Writes
    // the response body into responseBody and returns the HTTP status code.
    // A non-2xx status is NOT an error here - only transport/network failures
    // throw (exceptionTimeout on timeout, runtime_error otherwise). The
    // temporary header list is always freed before returning/throwing.
    long postJson(const string& url, const string& body, string& responseBody, unsigned int timeout) {
        CURL* curl = acquireHandle();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        responseBody.clear();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) body.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);

        CURLcode res = curl_easy_perform(curl);
        long statusCode = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
        }

        // curl_easy_reset on next acquireHandle() clears the header pointer from
        // the handle, but we still own the list here and must free it explicitly.
        curl_slist_free_all(headers);

        if (res == CURLE_OPERATION_TIMEDOUT) throw exceptionTimeout();
        if (res != CURLE_OK) {
            throw runtime_error(curl_easy_strerror(res));
        }
        return statusCode;
    }

    // Blocking HTTP GET that streams the response body straight to fileName
    // (opened "wb", truncating any existing file) instead of buffering it.
    // timeout in ms (0 disables). Throws runtime_error if the file cannot be
    // opened, exceptionTimeout on timeout, runtime_error on other curl errors.
    void getDownload(const string& url, const string& fileName, unsigned int timeout) {
        CURL* curl = acquireHandle();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        FILE* fp;
#ifdef _WIN32
        errno_t err = fopen_s(&fp, fileName.c_str(), "wb");
        if (err != 0 || fp == NULL) {
            throw runtime_error("Failed to open file for writing: " + fileName);
        }
#else
        fp = fopen(fileName.c_str(), "wb");
        if (fp == NULL) {
            throw runtime_error("Failed to open file for writing: " + fileName);
        }
#endif
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        applyAbortCheck(curl);
        CURLcode res = curl_easy_perform(curl);
        fclose(fp);
        if ((res == CURLE_OPERATION_TIMEDOUT) || (res == CURLE_ABORTED_BY_CALLBACK)) {
            discardHandle();
            throw exceptionTimeout();
        }
        if (res != CURLE_OK) {
            throw runtime_error(curl_easy_strerror(res));
        }
    }

    // Blocking form-urlencoded HTTP POST that streams the response body straight
    // to fileName (opened "wb", truncating any existing file). Body is the
    // "&"-joined key=value pairs from data (not URL-encoded). timeout in ms
    // (0 disables). Throws runtime_error if the file cannot be opened,
    // exceptionTimeout on timeout, runtime_error on other curl errors.
    void postDownload(const string& url, const string& fileName, const map<string, string>& data, unsigned int timeout) {
        //preprocess post data
        string postData;
        for (const auto& entry: data) {
            if (!postData.empty()) {
                postData += "&";
            }
            postData += entry.first + "=" + entry.second;
        }

        CURL* curl = acquireHandle();
        if (!curl) {
            throw runtime_error("Failed to initialize CURL");
        }

        FILE* fp;
#ifdef _WIN32
        errno_t err = fopen_s(&fp, fileName.c_str(), "wb");
        if (err != 0 || fp == NULL) {
            throw runtime_error("Failed to open file for writing: " + fileName);
        }
#else
        fp = fopen(fileName.c_str(), "wb");
        if (fp == NULL) {
            throw runtime_error("Failed to open file for writing: " + fileName);
        }
#endif
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
        if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        applyAbortCheck(curl);
        CURLcode res = curl_easy_perform(curl);
        fclose(fp);
        if ((res == CURLE_OPERATION_TIMEDOUT) || (res == CURLE_ABORTED_BY_CALLBACK)) {
            discardHandle();
            throw exceptionTimeout();
        }
        if (res != CURLE_OK) {
            throw runtime_error(curl_easy_strerror(res));
        }
    }


} // namespace CurlHandler
