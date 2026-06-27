#ifndef S3_AUTH_H
#define S3_AUTH_H

#include <stddef.h>

void sha256_file_hex(const char *filepath, char out[65]);

char* s3_create_authorization_headers(
    const char *method,
    const char *host,
    const char *path,
    const char *query,
    const char *region,
    const char *access_key,
    const char *secret_key,
    const char *payload,
    size_t payload_len,
    const char *content_type,
    const char *copy_source
);

char* s3_create_authorization_headers_with_hash(
    const char *method,
    const char *host,
    const char *path,
    const char *query,
    const char *region,
    const char *access_key,
    const char *secret_key,
    const char *payload_hash,
    const char *content_type,
    const char *copy_source
);

char* s3_create_presigned_url(
    const char *method,
    const char *host,
    const char *protocol,
    const char *path,
    const char *region,
    const char *access_key,
    const char *secret_key,
    long expires
);

#endif // S3_AUTH_H
