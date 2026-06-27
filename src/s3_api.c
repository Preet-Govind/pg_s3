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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    
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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    
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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    
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
    char query[2048] = "list-type=2";
    char url[2048];
    char *auth_headers;
    HttpResponse resp;

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s", bucket);
    
    if (prefix && prefix[0]) {
        snprintf(query + strlen(query), sizeof(query) - strlen(query), "&prefix=%s", prefix);
    }
    if (continuation_token && continuation_token[0]) {
        snprintf(query + strlen(query), sizeof(query) - strlen(query), "&continuation-token=%s", continuation_token);
    }
    
    snprintf(url, sizeof(url), "%s%s%s?%s", protocol, host, path, query);
    
    auth_headers = s3_create_authorization_headers("GET", host, path, query, region, access_key, secret_key, NULL, 0, NULL, NULL);
    
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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/", bucket);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    
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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/", bucket);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    
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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);

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

    parse_endpoint(endpoint, host, protocol);
    snprintf(path, sizeof(path), "/%s/%s", target_bucket, target_key);
    snprintf(url, sizeof(url), "%s%s%s", protocol, host, path);
    snprintf(copy_source, sizeof(copy_source), "/%s/%s", source_bucket, source_key);
    
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
    char *host;

    if (!endpoint || !region || !access_key || !secret_key || !bucket || !object_key || !filepath) {
        return false;
    }

    host = strstr(endpoint, "://");
    if (host) host += 3;
    else host = (char *)endpoint;

    snprintf(url, sizeof(url), "%s/%s/%s", endpoint, bucket, object_key);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);

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
    char *host;
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

    host = strstr(endpoint, "://");
    if (host) host += 3;
    else host = (char *)endpoint;

    snprintf(url, sizeof(url), "%s/%s/%s", endpoint, bucket, object_key);
    snprintf(path, sizeof(path), "/%s/%s", bucket, object_key);
    
    sha256_file_hex(filepath, payload_hash);

    headers = s3_create_authorization_headers_with_hash("PUT", host, path, "", region, access_key, secret_key, payload_hash, content_type, "");
    
    status = s3_http_request_upload("PUT", url, headers, filepath, file_size);
    
    free(headers);
    
    return status >= 200 && status < 300;
}
