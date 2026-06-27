#ifndef S3_API_H
#define S3_API_H

#include <stdbool.h>

char* s3_api_get(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, size_t *out_len);
bool s3_api_put(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *content, size_t content_len, const char *content_type);
bool s3_api_get_to_file(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *filepath);
bool s3_api_put_from_file(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *filepath, const char *content_type);
bool s3_api_delete(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key);
char* s3_api_list(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *prefix, const char *continuation_token);

bool s3_api_create_bucket(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket);
bool s3_api_delete_bucket(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket);
char* s3_api_presign(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, long expires);
bool s3_api_copy(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *source_bucket, const char *source_key, const char *target_bucket, const char *target_key);

#endif // S3_API_H
