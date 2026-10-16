#pragma once
/* Host implementation of the esp_http_server subset used by ap_transfer_server.cc (see sim_httpd.cc). */
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* httpd_handle_t;
typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[512];
    size_t content_len;
    void* aux;       /* per-request state owned by sim_httpd.cc */
    void* user_ctx;
} httpd_req_t;

typedef struct httpd_uri {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t* r);
    void* user_ctx;
} httpd_uri_t;

typedef struct {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    unsigned short server_port;
    unsigned short ctrl_port;
    unsigned max_open_sockets;
    unsigned max_uri_handlers;
    unsigned max_resp_headers;
    unsigned backlog_conn;
    bool lru_purge_enable;
    unsigned recv_wait_timeout;
    unsigned send_wait_timeout;
} httpd_config_t;

/* Port 80 needs root on a PC; SIM_HTTP_PORT overrides the default at httpd_start(). */
#define HTTPD_DEFAULT_CONFIG() { 5, 4096, 0, 8080, 32768, 7, 8, 8, 5, false, 5, 5 }

typedef enum {
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_404_NOT_FOUND = 404,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;

#define HTTPD_RESP_USE_STRLEN (-1)

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri_handler);
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type);
esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value);
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t buf_len);
esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str);
esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t error, const char* msg);
int httpd_req_recv(httpd_req_t* r, char* buf, size_t buf_len);
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t val_size);
int httpd_req_to_sockfd(httpd_req_t* r);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);

#ifdef __cplusplus
}
#endif
