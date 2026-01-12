#ifndef NIP11_H
#define NIP11_H

#include "esp_http_server.h"

esp_err_t nip11_handler(httpd_req_t *req);
esp_err_t nip11_options_handler(httpd_req_t *req);

#endif
