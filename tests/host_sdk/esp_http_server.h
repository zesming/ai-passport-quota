#pragma once
#include "host_sdk.h"

typedef void *httpd_handle_t;
typedef struct httpd_req {
    size_t content_len;
    void *user_ctx; /* Tests point this at their own request fixture. */
} httpd_req_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;
typedef struct {
    unsigned stack_size, max_open_sockets, max_uri_handlers, recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() {0}
#define HTTPD_RESP_USE_STRLEN (-1)
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, ssize_t length);
size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *field, char *value,
                                      size_t size);
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length);
int httpd_req_to_sockfd(httpd_req_t *request);
