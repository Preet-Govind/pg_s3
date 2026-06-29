#include "postgres.h"
/*
 * s3_api.c
 * Core abstraction layer mapping pgsql commands into s3 HTTP requests.
 *
 * dev notes:
 * - This acts as the bridge between pgsql's C API and `s3_http.c`.
 * - All AWS Signature V4 authentication logic is initialized here before making network calls.
 */
#include "s3_api.h"
#include "s3_auth.h"
#include "s3_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool s3_use_virtual_host;

// helper function to parse endpoint into host and protocol
static void parse_endpoint(const char *endpoint, char *host, char *protocol) {
    char *slash;
    if (strncmp(endpoint, "https://", 8) == 0) {
        strcpy(protocol, "https://");
        strcpy(host, endpoint + 8);
    } else if (strncmp(endpoint, "http://", 7) == 0) {
        strcpy(protocol, "http://");
        strcpy(host, endpoint + 7);
    } else {
        strcpy(protocol, "https://");
        strcpy(host, endpoint);
    }
    slash = strchr(host, '/');
    if (slash) *slash = '\0';
}
// AWS Signature Version 4 requires the canonical query String to be strictly URI-encoded and mathematically sorted by byte value (continuation-token -> list-type -> prefix
static char *s3_url_encode(const char *str) {
    const char *hex = "0123456789ABCDEF";
    char *encoded = malloc(strlen(str) * 3 + 1);
    char *p = encoded;
    if (!encoded) return NULL;
    while (*str) {
        if ((*str >= 'a' && *str <= 'z') ||
            (*str >= 'A' && *str <= 'Z') ||
            (*str >= '0' && *str <= '9') ||
            *str == '-' || *str == '_' || *str == '.' || *str == '~') {
            *p++ = *str;
        } else {
            *p++ = '%';
            *p++ = hex[(*str >> 4) & 0xF];
            *p++ = hex[*str & 0xF];
        }
        str++;
    }
    *p = '\0';
    return encoded;
}

static void build_s3_url(const char *endpoint, const char *bucket, const char *object_key,
                         char *host, char *protocol, char *path, char *url) {
    char base_host[256];
    const char *safe_key = (object_key && object_key[0] == '/') ? object_key + 1 : (object_key ? object_key : "");

    parse_endpoint(endpoint, base_host, protocol);

    if (s3_use_virtual_host && bucket && bucket[0]) {
        snprintf(host, 256, "%s.%s", bucket, base_host);
        if (object_key) {
            snprintf(path, 1024, "/%s", safe_key);
        } else {
            snprintf(path, 1024, "/");
        }
    } else {
        strcpy(host, base_host);
        if (bucket && bucket[0]) {
            if (object_key) {
                snprintf(path, 1024, "/%s/%s", bucket, safe_key);
            } else {
                snprintf(path, 1024, "/%s", bucket);
            }
        } else {
            snprintf(path, 1024, "/");
        }
    }
    snprintf(url, 2048, "%s%s%s", protocol, host, path);
}

// helper function to handle s3 XML error responses and bubble them to pgsql
static void handle_s3_error(HttpResponse *resp) {
    long status = resp->status_code;
    char error_body[2048];
    snprintf(error_body, sizeof(error_body), "%s", resp->data ? resp->data : "Unknown s3 Error");
    free_http_response(resp);
    ereport(ERROR, (
        errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION),
        errmsg("s3 API Error (HTTP %ld)", status),
        errdetail("%s", error_body)
    ));
}

// Fetches an object from s3 and returns its string content
char* s3_api_get(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, size_t *out_len) {
    char host[256], protocol[16];
    char path[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, url);
    
    ereport(NOTICE, (errmsg("DEBUG: GET Request URL: %s", url)));
    ereport(NOTICE, (errmsg("DEBUG: GET Request Path: %s", path)));

    auth_headers = s3_create_authorization_headers("GET", host, path, "", region, access_key, secret_key, NULL, 0, NULL, NULL);
    
    resp = s3_http_request("GET", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        char *ret = malloc(resp.size);
        if (resp.size > 0 && resp.data) {
            memcpy(ret, resp.data, resp.size);
        }
        *out_len = resp.size;
        free_http_response(&resp);
        return ret;
    } else {
        handle_s3_error(&resp);
        return NULL;
    }
}

// Uploads an object to s3 with the specified content type
bool s3_api_put(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *content, size_t content_len, const char *content_type) {
    char host[256], protocol[16];
    char path[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, url);
    
    auth_headers = s3_create_authorization_headers("PUT", host, path, "", region, access_key, secret_key, content, content_len, content_type, NULL);
    
    resp = s3_http_request("PUT", url, auth_headers, content, content_len, content_type);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        free_http_response(&resp);
        return true;
    } else {
        handle_s3_error(&resp);
        return false;
    }
}

bool s3_api_delete(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key) {
    char host[256], protocol[16];
    char path[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, url);
    
    auth_headers = s3_create_authorization_headers("DELETE", host, path, "", region, access_key, secret_key, NULL, 0, NULL, NULL);
    
    resp = s3_http_request("DELETE", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        free_http_response(&resp);
        return true;
    } else {
        handle_s3_error(&resp);
        return false;
    }
}

// Lists objects in an s3 bucket, with support for prefixes and continuation tokens for pagination
char* s3_api_list(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *prefix, const char *continuation_token) {
    char host[256], protocol[16];
    char path[1024];
    char query[2048] = "";
    char url[2048];
    char *auth_headers;
    HttpResponse resp;
    char *encoded_prefix = NULL;
    char *encoded_token = NULL;

    build_s3_url(endpoint, bucket, NULL, host, protocol, path, url);
    
    if (continuation_token && continuation_token[0]) {
        encoded_token = s3_url_encode(continuation_token);
        snprintf(query + strlen(query), sizeof(query) - strlen(query), "continuation-token=%s&", encoded_token);
    }

    snprintf(query + strlen(query), sizeof(query) - strlen(query), "list-type=2");

    if (prefix && prefix[0]) {
        const char *safe_prefix = prefix[0] == '/' ? prefix + 1 : prefix;
        encoded_prefix = s3_url_encode(safe_prefix);
        snprintf(query + strlen(query), sizeof(query) - strlen(query), "&prefix=%s", encoded_prefix);
    }
    
    snprintf(url, sizeof(url), "%s%s%s?%s", protocol, host, path, query);
    
    auth_headers = s3_create_authorization_headers("GET", host, path, query, region, access_key, secret_key, NULL, 0, NULL, NULL);
    
    if (encoded_prefix) free(encoded_prefix);
    if (encoded_token) free(encoded_token);
    
    resp = s3_http_request("GET", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        char *ret = strdup(resp.data ? resp.data : "");
        free_http_response(&resp);
        return ret;
    } else {
        handle_s3_error(&resp);
        return NULL;
    }
}

bool s3_api_create_bucket(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket) {
    char host[256], protocol[16];
    char path[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    build_s3_url(endpoint, bucket, NULL, host, protocol, path, url);
    
    auth_headers = s3_create_authorization_headers("PUT", host, path, "", region, access_key, secret_key, NULL, 0, NULL, NULL);
    
    resp = s3_http_request("PUT", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        free_http_response(&resp);
        return true;
    } else {
        handle_s3_error(&resp);
        return false;
    }
}

bool s3_api_delete_bucket(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket) {
    char host[256], protocol[16];
    char path[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    build_s3_url(endpoint, bucket, NULL, host, protocol, path, url);
    
    auth_headers = s3_create_authorization_headers("DELETE", host, path, "", region, access_key, secret_key, NULL, 0, NULL, NULL);
    
    resp = s3_http_request("DELETE", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        free_http_response(&resp);
        return true;
    } else {
        handle_s3_error(&resp);
        return false;
    }
}

// Generates an authenticated Presigned URL for temporary access to an object
char* s3_api_presign(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, long expires) {
    char host[256], protocol[16];
    char path[1024];
    char dummy_url[2048];

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, dummy_url);

    return s3_create_presigned_url("GET", host, protocol, path, region, access_key, secret_key, expires);
}

// Native s3 CopyObject API to copy files between buckets or keys instantly
bool s3_api_copy(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *source_bucket, const char *source_key, const char *target_bucket, const char *target_key) {
    char host[256], protocol[16];
    char path[1024];
    char copy_source[1024];
    char url[2048];
    char *auth_headers;
    HttpResponse resp;
    const char *safe_source = source_key[0] == '/' ? source_key + 1 : source_key;

    build_s3_url(endpoint, target_bucket, target_key, host, protocol, path, url);
    snprintf(copy_source, sizeof(copy_source), "/%s/%s", source_bucket, safe_source);
    
    auth_headers = s3_create_authorization_headers("PUT", host, path, "", region, access_key, secret_key, NULL, 0, NULL, copy_source);
    
    resp = s3_http_request("PUT", url, auth_headers, NULL, 0, NULL);
    free(auth_headers);
    
    if (resp.status_code >= 200 && resp.status_code < 300) {
        free_http_response(&resp);
        return true;
    } else {
        handle_s3_error(&resp);
        return false;
    }
}

bool s3_api_get_to_file(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *filepath) {
    char url[4096];
    char *headers;
    long status;
    char path[1024];
    char host[256], protocol[16];

    if (!endpoint || !region || !access_key || !secret_key || !bucket || !object_key || !filepath) {
        return false;
    }

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, url);

    ereport(NOTICE, (errmsg("DEBUG: GET-TO-FILE Request URL: %s", url)));
    ereport(NOTICE, (errmsg("DEBUG: GET-TO-FILE Request Path: %s", path)));

    headers = s3_create_authorization_headers("GET", host, path, "", region, access_key, secret_key, "", 0, "", "");
    
    status = s3_http_request_download(url, headers, filepath);
    
    free(headers);
    
    return status >= 200 && status < 300;
}

bool s3_api_put_from_file(const char *endpoint, const char *region, const char *access_key, const char *secret_key, const char *bucket, const char *object_key, const char *filepath, const char *content_type) {
    char url[4096];
    char *headers;
    long status;
    char payload_hash[65];
    char path[1024];
    char host[256], protocol[16];
    FILE *fp;
    size_t file_size;

    if (!endpoint || !region || !access_key || !secret_key || !bucket || !object_key || !filepath) {
        return false;
    }

    fp = fopen(filepath, "rb");
    if (!fp) return false;
    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp);
    fclose(fp);

    build_s3_url(endpoint, bucket, object_key, host, protocol, path, url);
    
    sha256_file_hex(filepath, payload_hash);

    headers = s3_create_authorization_headers_with_hash("PUT", host, path, "", region, access_key, secret_key, payload_hash, content_type, "");
    
    status = s3_http_request_upload("PUT", url, headers, filepath, file_size);
    
    free(headers);
    
    return status >= 200 && status < 300;
}
