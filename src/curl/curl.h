/*
** Stub header for curl
** This minimal header allows compilation to proceed
**
** Declaration-only subset of libcurl's <curl/curl.h> "easy" API. It exists so
** code that includes <curl/curl.h> (the node/pool HTTP client wrappers) can be
** compiled without the full libcurl development headers on the build machine;
** the real libcurl library still provides these symbols at link/run time.
** Contains no implementation — only the types, enums, and function prototypes
** actually referenced by this project.
*/

#ifndef __CURL_CURL_H
#define __CURL_CURL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle type for a libcurl "easy" session. */
typedef void CURL;

/* Singly-linked list of strings, used to pass e.g. custom HTTP headers. */
struct curl_slist {
  char *data;
  struct curl_slist *next;
};
typedef struct curl_slist CURLIST;

/* Result/error codes returned by the easy-interface functions (CURLE_OK == success). */
typedef enum {
  CURLE_OK = 0,
  CURLE_UNSUPPORTED_PROTOCOL,
  CURLE_FAILED_INIT,
  CURLE_URL_MALFORMAT,
  CURLE_COULDNT_RESOLVE_PROXY,
  CURLE_COULDNT_RESOLVE_HOST,
  CURLE_COULDNT_CONNECT,
  CURLE_FTP_WEIRD_SERVER_REPLY,
  CURLE_REMOTE_ACCESS_DENIED,
  CURLE_FTP_WEIRD_PASS_REPLY,
  CURLE_FTP_WEIRD_USER_REPLY,
  CURLE_FTP_WEIRD_PASV_REPLY,
  CURLE_FTP_WEIRD_227_FORMAT,
  CURLE_FTP_CANT_GET_HOST,
  CURLE_PARTIAL_FILE,
  CURLE_FTP_COULDNT_RETR_FILE,
  CURLE_QUOTE_ERROR,
  CURLE_HTTP_RETURNED_ERROR,
  CURLE_WRITE_ERROR,
  CURLE_UPLOAD_FAILED,
  CURLE_READ_ERROR,
  CURLE_OUT_OF_MEMORY,
  CURLE_OPERATION_TIMEDOUT,
  CURLE_FTP_PORT_FAILED,
  CURLE_FTP_COULDNT_USE_REST,
  CURLE_RANGE_ERROR,
  CURLE_HTTP_POST_ERROR,
  CURLE_SSL_CONNECT_ERROR,
  CURLE_BAD_DOWNLOAD_RESUME,
  CURLE_FILE_COULDNT_READ_FILE,
  CURLE_LDAP_CANNOT_BIND,
  CURLE_LDAP_SEARCH_FAILED,
  CURLE_LIBRARY_NOT_FOUND,
  CURLE_FUNCTION_NOT_FOUND,
  CURLE_ABORTED_BY_CALLBACK,
  CURLE_BAD_FUNCTION_ARGUMENT,
  CURLE_INTERFACE_FAILED,
  CURLE_TOO_MANY_REDIRECTS,
  CURLE_UNKNOWN_OPTION,
  CURLE_TELNET_OPTION_SYNTAX,
  CURLE_PEER_FAILED_VERIFICATION,
  CURLE_GOT_NOTHING,
  CURLE_SSL_ENGINE_NOTFOUND,
  CURLE_SSL_ENGINE_SETFAILED,
  CURLE_SEND_ERROR,
  CURLE_RECV_ERROR,
  CURLE_SSL_CERTPROBLEM,
  CURLE_SSL_CIPHER,
  CURLE_SSL_CACERT,
  CURLE_BAD_CONTENT_ENCODING,
  CURLE_LDAP_INVALID_URL,
  CURLE_FILESIZE_EXCEEDED,
  CURLE_USE_SSL_FAILED,
  CURLE_SEND_FAIL_REWIND,
  CURLE_SSL_ENGINE_INITFAILED,
  CURLE_LOGIN_DENIED,
  CURLE_TFTP_NOTFOUND,
  CURLE_TFTP_PERM,
  CURLE_REMOTE_DISK_FULL,
  CURLE_TFTP_ILLEGAL,
  CURLE_TFTP_UNKNOWNID,
  CURLE_REMOTE_FILE_EXISTS,
  CURLE_TFTP_NOSUCHUSER,
  CURLE_CONV_FAILED,
  CURLE_CONV_REQD,
  CURLE_SSL_CACERT_BADFILE,
  CURLE_REMOTE_FILE_NOT_FOUND,
  CURLE_SSH,
  CURLE_SSL_SHUTDOWN_FAILED,
  CURLE_AGAIN,
  CURLE_SSL_CRL_BADFILE,
  CURLE_SSL_ISSUER_ERROR,
  CURLE_FTP_PRET_FAILED,
  CURLE_RTSP_CSEQ_ERROR,
  CURLE_RTSP_SESSION_ERROR,
  CURLE_FTP_BAD_FILE_LIST,
  CURLE_CHUNK_FAILED,
  CURLE_NO_CONNECTION_AVAILABLE,
  CURLE_SSL_PINNEDPUBKEYNOTMATCH,
  CURLE_SSL_INVALIDCERTSTATUS,
  CURLE_HTTP2_STREAM,
  CURLE_RECURSIVE_API_CALL,
  CURLE_AUTH_ERROR,
  CURLE_HTTP3,
  CURLE_QUIC_CONNECT_ERROR,
  CURLE_PROXY,
  CURLE_SSL_CLIENTCERT,
  CURLE_UNRECOVERABLE_PUSH,
  CURLE_POLL_ERROR,
  CURLE_GOT_NOTHING_ERROR,
  CURLE_USER_REJECTED_PEER_VERIF,
  CURL_LAST
} CURLcode;

/* Option identifiers passed to curl_easy_setopt to configure a transfer. */
typedef enum {
  CURLOPT_VERBOSE = 0,
  CURLOPT_URL,
  CURLOPT_PORT,
  CURLOPT_PROXY,
  CURLOPT_USERPWD,
  CURLOPT_PROXYUSERPWD,
  CURLOPT_RANGE,
  CURLOPT_INFILE,
  CURLOPT_ERRORBUFFER,
  CURLOPT_WRITEFUNCTION,
  CURLOPT_READFUNCTION,
  CURLOPT_TIMEOUT,
  CURLOPT_INFILESIZE,
  CURLOPT_POSTFIELDS,
  CURLOPT_REFERER,
  CURLOPT_FTPPORT,
  CURLOPT_USERAGENT,
  CURLOPT_LOW_SPEED_LIMIT,
  CURLOPT_LOW_SPEED_TIME,
  CURLOPT_RESUME_FROM,
  CURLOPT_COOKIE,
  CURLOPT_HTTPHEADER,
  CURLOPT_HTTPPOST,
  CURLOPT_SSLCERT,
  CURLOPT_KEYPASSWD,
  CURLOPT_CRLF,
  CURLOPT_QUOTE,
  CURLOPT_WRITEHEADER,
  CURLOPT_COOKIEFILE,
  CURLOPT_SSLVERSION,
  CURLOPT_TIMECONDITION,
  CURLOPT_TIMEVALUE,
  CURLOPT_CUSTOMREQUEST,
  CURLOPT_STDERR,
  CURLOPT_POSTQUOTE,
  CURLOPT_WRITEINFO,
  CURLOPT_VERBOSE_MASK,
  CURLOPT_NOPROGRESS,
  CURLOPT_NOBODY,
  CURLOPT_FAILONERROR,
  CURLOPT_UPLOAD,
  CURLOPT_POST,
  CURLOPT_DIRLISTONLY,
  CURLOPT_APPEND,
  CURLOPT_NETRC,
  CURLOPT_FOLLOWLOCATION,
  CURLOPT_TRANSFERTEXT,
  CURLOPT_MUTE,
  CURLOPT_PASSWDFUNCTION,
  CURLOPT_RETURNTRANSFER,
  CURLOPT_FILE_PARAMETER,
  CURLOPT_WRITEDATA = 10001,
  CURLOPT_READDATA = 10002,
  CURLOPT_TIMEOUT_MS = 155,
  CURLOPT_NOSIGNAL = 99,
  CURLOPT_POSTFIELDSIZE = 60,
  CURLOPT_COPYPOSTFIELDS = 10165,
  CURLOPT_SSL_VERIFYPEER = 64,
  CURLOPT_SSL_VERIFYHOST = 81
} CURLoption;

/* Info identifiers passed to curl_easy_getinfo to read transfer results. */
typedef enum {
  CURLINFO_RESPONSE_CODE = 2097154,
  CURLINFO_TOTAL_TIME = 3145731,
  CURLINFO_NAMELOOKUP_TIME = 3145732,
  CURLINFO_CONNECT_TIME = 3145733
} CURLINFO;

/* libcurl easy-interface prototypes (implemented by the real libcurl at link time). */
CURL *curl_easy_init(void);                                                      /* create a handle */
void curl_easy_reset(CURL *handle);                                              /* reset options to defaults */
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);                 /* set one transfer option */
CURLcode curl_easy_perform(CURL *easy_handle);                                   /* run the transfer (blocking) */
void curl_easy_cleanup(CURL *handle);                                            /* free a handle */
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);                    /* read info after a transfer */
const char *curl_easy_strerror(CURLcode code);                                   /* human-readable error string */
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string); /* append to a string list */
void curl_slist_free_all(struct curl_slist *list);                               /* free a string list */
CURLcode curl_global_init(long flags);                                           /* process-wide init */
void curl_global_cleanup(void);                                                  /* process-wide teardown */

#define CURL_MAX_WRITE_SIZE 16384
/* Signature of the callback libcurl invokes with received response body chunks. */
typedef size_t (*curl_write_callback)(char *buffer, size_t size, size_t nmemb, void *userp);

#define CURL_GLOBAL_DEFAULT 1
#define CURL_GLOBAL_NOTHING 0
#define CURL_GLOBAL_SSL     1
#define CURL_GLOBAL_WIN32   2
#define CURL_GLOBAL_ALL     (CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32)

#ifdef __cplusplus
}
#endif

#endif /* __CURL_CURL_H */
