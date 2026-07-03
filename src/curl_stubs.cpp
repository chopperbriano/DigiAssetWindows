/*
** WinHTTP-based curl implementation for Windows
** Implements the curl API subset used by DigiAsset Core
** Persists WinHTTP session and connection handles across calls to eliminate
** per-request TCP handshake overhead.
*/
#include "curl/curl.h"
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// ---- Helpers ----------------------------------------------------------------

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &result[0], len);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

static std::string base64Encode(const std::string& in) {
    static const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

struct ParsedUrl {
    std::wstring host;
    int port = 80;
    std::wstring path;
    bool isHttps = false;
    std::string username;
    std::string password;
};

static bool parseUrl(const std::string& url, ParsedUrl& out) {
    // scheme://[user:pass@]host[:port][/path]
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    std::string scheme = url.substr(0, schemeEnd);
    out.isHttps = (scheme == "https");
    out.port    = out.isHttps ? 443 : 80;

    size_t rest = schemeEnd + 3;

    // user:pass@
    size_t atPos = url.find('@', rest);
    if (atPos != std::string::npos) {
        std::string auth = url.substr(rest, atPos - rest);
        size_t colon = auth.find(':');
        if (colon != std::string::npos) {
            out.username = auth.substr(0, colon);
            out.password = auth.substr(colon + 1);
        } else {
            out.username = auth;
        }
        rest = atPos + 1;
    }

    // host[:port][/path]
    size_t pathStart = url.find('/', rest);
    std::string hostPort;
    std::string path;
    if (pathStart != std::string::npos) {
        hostPort = url.substr(rest, pathStart - rest);
        path     = url.substr(pathStart);
    } else {
        hostPort = url.substr(rest);
        path     = "/";
    }
    size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        out.host = utf8ToWide(hostPort.substr(0, colon));
        out.port = std::stoi(hostPort.substr(colon + 1));
    } else {
        out.host = utf8ToWide(hostPort);
    }
    out.path = utf8ToWide(path);
    return true;
}

// ---- Handle -----------------------------------------------------------------

struct CurlHandle {
    std::string            url;
    std::string            postFields;
    curl_slist*            headers       = nullptr;
    curl_write_callback    writeFunc     = nullptr;
    void*                  writeData     = nullptr;
    long                   timeoutMs     = 30000;
    bool                   isPost        = false;
    long                   responseCode  = 0;

    // Persistent WinHTTP handles — reused across curl_easy_perform calls
    HINTERNET              hSession      = nullptr;
    HINTERNET              hConnect      = nullptr;
    // Track what host/port/scheme the connection handle is for
    std::wstring           connHost;
    int                    connPort      = 0;
    bool                   connHttps     = false;
};

// ---- Internal helpers -------------------------------------------------------

static void closeConnectHandle(CurlHandle* h) {
    if (h->hConnect) {
        WinHttpCloseHandle(h->hConnect);
        h->hConnect  = nullptr;
        h->connHost  = L"";
        h->connPort  = 0;
        h->connHttps = false;
    }
}

// Open or reuse the WinHTTP session; apply timeout settings.
static CURLcode ensureSession(CurlHandle* h, DWORD timeout) {
    if (!h->hSession) {
        h->hSession = WinHttpOpen(
            L"DigiAssetWindows/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!h->hSession) return CURLE_FAILED_INIT;

        // Disable automatic Windows auth (NTLM/Negotiate) — prevents hang on HTTP 401
        DWORD autologonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(h->hSession, WINHTTP_OPTION_AUTOLOGON_POLICY,
                         &autologonPolicy, sizeof(autologonPolicy));
    }
    // Always refresh timeouts in case they changed via CURLOPT_TIMEOUT
    WinHttpSetTimeouts(h->hSession, 5000, 5000, timeout, timeout);
    return CURLE_OK;
}

// Open or reuse the WinHTTP connection handle for this host/port/scheme.
static CURLcode ensureConnect(CurlHandle* h, const ParsedUrl& parsed) {
    bool same = (h->hConnect != nullptr &&
                 h->connHost  == parsed.host &&
                 h->connPort  == parsed.port &&
                 h->connHttps == parsed.isHttps);
    if (same) return CURLE_OK;

    closeConnectHandle(h);
    h->hConnect = WinHttpConnect(h->hSession, parsed.host.c_str(),
                                  (INTERNET_PORT)parsed.port, 0);
    if (!h->hConnect) return CURLE_COULDNT_CONNECT;
    h->connHost  = parsed.host;
    h->connPort  = parsed.port;
    h->connHttps = parsed.isHttps;
    return CURLE_OK;
}

// ---- Public API -------------------------------------------------------------

CURL* curl_easy_init(void) {
    return reinterpret_cast<CURL*>(new CurlHandle());
}

void curl_easy_reset(CURL* handle) {
    CurlHandle* h = reinterpret_cast<CurlHandle*>(handle);
    if (!h) return;
    // Reset per-request state but preserve persistent WinHTTP session/connection
    h->url.clear();
    h->postFields.clear();
    h->headers       = nullptr;
    h->writeFunc     = nullptr;
    h->writeData     = nullptr;
    h->timeoutMs     = 30000;
    h->isPost        = false;
    h->responseCode  = 0;
    // hSession, hConnect, connHost, connPort, connHttps are preserved
}

CURLcode curl_easy_setopt(CURL* handle, CURLoption option, ...) {
    CurlHandle* h = reinterpret_cast<CurlHandle*>(handle);
    if (!h) return CURLE_BAD_FUNCTION_ARGUMENT;

    va_list args;
    va_start(args, option);

    switch (option) {
        case CURLOPT_URL:
            h->url = va_arg(args, const char*);
            break;
        case CURLOPT_WRITEFUNCTION:
            h->writeFunc = va_arg(args, curl_write_callback);
            break;
        case CURLOPT_WRITEDATA:
            h->writeData = va_arg(args, void*);
            break;
        case CURLOPT_POSTFIELDS:
            h->postFields = va_arg(args, const char*);
            h->isPost = true;
            break;
        case CURLOPT_HTTPHEADER:
            h->headers = va_arg(args, curl_slist*);
            break;
        case CURLOPT_TIMEOUT_MS:
            h->timeoutMs = va_arg(args, long);
            break;
        case CURLOPT_TIMEOUT:
            h->timeoutMs = va_arg(args, long) * 1000L;
            break;
        case CURLOPT_POST:
            h->isPost = (va_arg(args, long) != 0);
            break;
        default:
            // consume unknown argument (treat as long/pointer)
            (void)va_arg(args, void*);
            break;
    }

    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL* easy_handle) {
    CurlHandle* h = reinterpret_cast<CurlHandle*>(easy_handle);
    if (!h || h->url.empty()) return CURLE_URL_MALFORMAT;

    ParsedUrl parsed;
    if (!parseUrl(h->url, parsed)) return CURLE_URL_MALFORMAT;

    DWORD timeout = (h->timeoutMs > 0) ? (DWORD)h->timeoutMs : 30000;

    // Ensure we have a persistent session handle
    CURLcode rc = ensureSession(h, timeout);
    if (rc != CURLE_OK) return rc;

    // Ensure we have a connection handle for this host/port/scheme
    rc = ensureConnect(h, parsed);
    if (rc != CURLE_OK) return rc;

    // Build extra headers string
    std::wstring extraHeaders;
    for (curl_slist* sl = h->headers; sl; sl = sl->next) {
        extraHeaders += utf8ToWide(sl->data);
        extraHeaders += L"\r\n";
    }
    if (!parsed.username.empty()) {
        std::string encoded = base64Encode(parsed.username + ":" + parsed.password);
        extraHeaders += L"Authorization: Basic ";
        extraHeaders += utf8ToWide(encoded);
        extraHeaders += L"\r\n";
    }
    // libcurl automatically sets Content-Type for POSTFIELDS — match that behavior.
    // Without this, servers like Express body-parser silently drop the body.
    if (h->isPost && !h->postFields.empty()) {
        // Only add if user hasn't already provided one
        bool hasContentType = false;
        for (curl_slist* sl = h->headers; sl; sl = sl->next) {
            std::string hdr(sl->data);
            std::string lower;
            for (char c : hdr) lower += (char)tolower(c);
            if (lower.find("content-type:") == 0) { hasContentType = true; break; }
        }
        if (!hasContentType) {
            extraHeaders += L"Content-Type: application/x-www-form-urlencoded\r\n";
        }
    }

    const void* postData    = h->isPost ? h->postFields.c_str() : nullptr;
    DWORD       postDataLen = h->isPost ? (DWORD)h->postFields.size() : 0;

    // Attempt the request, with one reconnect retry if the connection was stale
    for (int attempt = 0; attempt < 2; ++attempt) {
        DWORD reqFlags = parsed.isHttps ? WINHTTP_FLAG_SECURE : 0;
        const wchar_t* method = h->isPost ? L"POST" : L"GET";
        HINTERNET hRequest = WinHttpOpenRequest(
            h->hConnect,
            method,
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            reqFlags);

        if (!hRequest) {
            // Connection handle is bad — drop it and reconnect
            closeConnectHandle(h);
            if (ensureConnect(h, parsed) != CURLE_OK) return CURLE_COULDNT_CONNECT;
            continue; // retry with fresh connection
        }

        BOOL sent;
        if (!extraHeaders.empty()) {
            sent = WinHttpSendRequest(
                hRequest,
                extraHeaders.c_str(), (DWORD)extraHeaders.size(),
                (LPVOID)postData, postDataLen, postDataLen,
                0);
        } else {
            sent = WinHttpSendRequest(
                hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                (LPVOID)postData, postDataLen, postDataLen,
                0);
        }

        if (!sent) {
            DWORD err = GetLastError();
            WinHttpCloseHandle(hRequest);
            if (attempt == 0 && err != ERROR_WINHTTP_TIMEOUT) {
                // Possibly stale keep-alive connection — reconnect and retry once
                closeConnectHandle(h);
                if (ensureConnect(h, parsed) != CURLE_OK) return CURLE_COULDNT_CONNECT;
                continue;
            }
            if (err == ERROR_WINHTTP_TIMEOUT)           return CURLE_OPERATION_TIMEDOUT;
            if (err == ERROR_WINHTTP_CANNOT_CONNECT ||
                err == ERROR_WINHTTP_CONNECTION_ERROR)  return CURLE_COULDNT_CONNECT;
            return CURLE_SEND_ERROR;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD err = GetLastError();
            WinHttpCloseHandle(hRequest);
            return (err == ERROR_WINHTTP_TIMEOUT) ? CURLE_OPERATION_TIMEDOUT : CURLE_RECV_ERROR;
        }

        // HTTP status code
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &sz,
            WINHTTP_NO_HEADER_INDEX);
        h->responseCode = (long)statusCode;

        // Read body via write callback
        CURLcode result = CURLE_OK;
        if (h->writeFunc) {
            std::vector<char> buf(65536);
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
                if (available > (DWORD)buf.size()) buf.resize(available);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buf.data(), available, &bytesRead) && bytesRead > 0) {
                    size_t written = h->writeFunc(buf.data(), 1, bytesRead, h->writeData);
                    if (written != bytesRead) { result = CURLE_WRITE_ERROR; break; }
                }
            }
        }

        WinHttpCloseHandle(hRequest);
        return result;
    }

    return CURLE_COULDNT_CONNECT;
}

void curl_easy_cleanup(CURL* handle) {
    CurlHandle* h = reinterpret_cast<CurlHandle*>(handle);
    if (!h) return;
    closeConnectHandle(h);
    if (h->hSession) {
        WinHttpCloseHandle(h->hSession);
        h->hSession = nullptr;
    }
    delete h;
}

CURLcode curl_easy_getinfo(CURL* handle, CURLINFO info, ...) {
    CurlHandle* h = reinterpret_cast<CurlHandle*>(handle);
    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long* code = va_arg(args, long*);
        if (code && h) *code = h->responseCode;
    } else {
        (void)va_arg(args, void*);
    }
    va_end(args);
    return CURLE_OK;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK:                  return "No error";
        case CURLE_OPERATION_TIMEDOUT:  return "Operation timed out";
        case CURLE_COULDNT_CONNECT:     return "Could not connect to server";
        case CURLE_URL_MALFORMAT:       return "URL malformed";
        case CURLE_SEND_ERROR:          return "Failed to send data";
        case CURLE_RECV_ERROR:          return "Failed to receive data";
        case CURLE_FAILED_INIT:         return "Failed to initialize";
        default:                        return "curl operation error";
    }
}

struct curl_slist* curl_slist_append(struct curl_slist* list, const char* string) {
    struct curl_slist* item = new curl_slist();
    item->data = _strdup(string);
    item->next = nullptr;
    if (!list) return item;
    curl_slist* end = list;
    while (end->next) end = end->next;
    end->next = item;
    return list;
}

void curl_slist_free_all(struct curl_slist* list) {
    while (list) {
        curl_slist* next = list->next;
        free(list->data);
        delete list;
        list = next;
    }
}

CURLcode curl_global_init(long flags) {
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
}
