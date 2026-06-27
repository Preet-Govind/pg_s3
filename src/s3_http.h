#ifndef S3_HTTP_H
#define S3_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t size;
    long status_code;
} HttpResponse;

// headers is a newline-separated list of headers, e.g., "Host: ...\r\nAuthorization: ..."
HttpResponse s3_http_request(const char *method, const char *url, const char *headers_str, const char *body, size_t body_len, const char *content_type);
long s3_http_request_download(const char *url, const char *headers_str, const char *filepath);
long s3_http_request_upload(const char *method, const char *url, const char *headers_str, const char *filepath, size_t file_size);
void free_http_response(HttpResponse *resp);

#endif // S3_HTTP_H
