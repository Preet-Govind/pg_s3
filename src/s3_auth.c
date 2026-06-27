/*
 * s3_auth.c
 * Implementation of AWS Signature Version 4.
 *
 * dev notes:
 * - S3 requires every request to be cryptographically signed via SHA256/HMAC.
 * - This file computes the exact canonical requests and string-to-sign dynamically.
 * - Note: currently uses OpenSSL 3.0 deprecated macros (SHA256_Init). A future refactor
 *   could upgrade this to the newer EVP API.
 */
#include "s3_auth.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static void sha256_hex(const char *data, size_t len, char *out) {
    unsigned char hash_bin[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data, len);
    SHA256_Final(hash_bin, &sha256);
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(out + (i * 2), "%02x", hash_bin[i]);
    }
    out[64] = '\0';
}

void sha256_file_hex(const char *filepath, char out[65]) {
    unsigned char hash_bin[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    FILE *fp = fopen(filepath, "rb");
    if (fp) {
        char buffer[32768];
        size_t bytesRead;
        while ((bytesRead = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            SHA256_Update(&sha256, buffer, bytesRead);
        }
        fclose(fp);
    }
    
    SHA256_Final(hash_bin, &sha256);
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(out + (i * 2), "%02x", hash_bin[i]);
    }
    out[64] = '\0';
}

static void hmac_sha256(const unsigned char *key, int key_len, const char *data, int data_len, unsigned char *out, unsigned int *out_len) {
    HMAC_CTX *ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, key, key_len, EVP_sha256(), NULL);
    HMAC_Update(ctx, (const unsigned char*)data, data_len);
    HMAC_Final(ctx, out, out_len);
    HMAC_CTX_free(ctx);
}

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
    const char *copy_source)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char date_stamp[9];
    char amz_date[17];
    char payload_hash[65];
    char canonical_headers[1024];
    char signed_headers[256];
    char content_type_header[256] = {0};
    char copy_source_header[256] = {0};
    char canonical_request[2048];
    char canonical_request_hash[65];
    char credential_scope[256];
    char string_to_sign[1024];
    unsigned char kDate[EVP_MAX_MD_SIZE];
    unsigned int kDate_len;
    char aws4_secret[256];
    unsigned char kRegion[EVP_MAX_MD_SIZE];
    unsigned int kRegion_len;
    unsigned char kService[EVP_MAX_MD_SIZE];
    unsigned int kService_len;
    unsigned char kSigning[EVP_MAX_MD_SIZE];
    unsigned int kSigning_len;
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_len;
    char signature_hex[65];
    char *auth_headers;
    unsigned int i;

    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    sha256_hex(payload ? payload : "", payload ? payload_len : 0, payload_hash);

    if (content_type && strlen(content_type) > 0) {
        snprintf(content_type_header, sizeof(content_type_header), "content-type:%s\n", content_type);
    }
    if (copy_source && strlen(copy_source) > 0) {
        snprintf(copy_source_header, sizeof(copy_source_header), "x-amz-copy-source:%s\n", copy_source);
    }

    snprintf(canonical_headers, sizeof(canonical_headers), 
             "%s"
             "host:%s\n"
             "x-amz-content-sha256:%s\n"
             "%s"
             "x-amz-date:%s\n", 
             content_type_header, 
             host, 
             payload_hash, 
             copy_source_header, 
             amz_date);

    snprintf(signed_headers, sizeof(signed_headers), 
             "%s"
             "host;"
             "x-amz-content-sha256;"
             "%s"
             "x-amz-date", 
             (content_type && strlen(content_type) > 0) ? "content-type;" : "",
             (copy_source && strlen(copy_source) > 0) ? "x-amz-copy-source;" : "");

    snprintf(canonical_request, sizeof(canonical_request), "%s\n%s\n%s\n%s\n%s\n%s",
             method, path, query ? query : "", canonical_headers, signed_headers, payload_hash);

    sha256_hex(canonical_request, strlen(canonical_request), canonical_request_hash);

    snprintf(credential_scope, sizeof(credential_scope), "%s/%s/s3/aws4_request", date_stamp, region);

    snprintf(string_to_sign, sizeof(string_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, credential_scope, canonical_request_hash);

    snprintf(aws4_secret, sizeof(aws4_secret), "AWS4%s", secret_key);
    hmac_sha256((unsigned char*)aws4_secret, strlen(aws4_secret), date_stamp, strlen(date_stamp), kDate, &kDate_len);

    hmac_sha256(kDate, kDate_len, region, strlen(region), kRegion, &kRegion_len);

    hmac_sha256(kRegion, kRegion_len, "s3", 2, kService, &kService_len);

    hmac_sha256(kService, kService_len, "aws4_request", 12, kSigning, &kSigning_len);

    hmac_sha256(kSigning, kSigning_len, string_to_sign, strlen(string_to_sign), signature, &signature_len);

    for(i = 0; i < signature_len; i++) {
        sprintf(signature_hex + (i * 2), "%02x", signature[i]);
    }
    signature_hex[64] = '\0';

    auth_headers = malloc(4096);
    snprintf(auth_headers, 4096, 
             "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s\r\n" // 
             "x-amz-date: %s\r\n"
             "x-amz-content-sha256: %s\r\n"
             "%s%s%s"
             "%s%s%s",
             access_key, credential_scope, signed_headers, signature_hex, amz_date, payload_hash,
             (content_type && strlen(content_type) > 0) ? "Content-Type: " : "", 
             (content_type && strlen(content_type) > 0) ? content_type : "", 
             (content_type && strlen(content_type) > 0) ? "\r\n" : "",
             (copy_source && strlen(copy_source) > 0) ? "x-amz-copy-source: " : "", 
             (copy_source && strlen(copy_source) > 0) ? copy_source : "", 
             (copy_source && strlen(copy_source) > 0) ? "\r\n" : "");

    return auth_headers;
}

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
    const char *copy_source)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char date_stamp[9];
    char amz_date[17];
    char canonical_headers[1024];
    char signed_headers[256];
    char content_type_header[256] = {0};
    char copy_source_header[256] = {0};
    char canonical_request[2048];
    char canonical_request_hash[65];
    char credential_scope[256];
    char string_to_sign[1024];
    unsigned char kDate[EVP_MAX_MD_SIZE];
    unsigned int kDate_len;
    char aws4_secret[256];
    unsigned char kRegion[EVP_MAX_MD_SIZE];
    unsigned int kRegion_len;
    unsigned char kService[EVP_MAX_MD_SIZE];
    unsigned int kService_len;
    unsigned char kSigning[EVP_MAX_MD_SIZE];
    unsigned int kSigning_len;
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_len;
    char signature_hex[65];
    char *auth_headers;
    unsigned int i;

    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    if (content_type && strlen(content_type) > 0) {
        snprintf(content_type_header, sizeof(content_type_header), "content-type:%s\n", content_type);
    }
    if (copy_source && strlen(copy_source) > 0) {
        snprintf(copy_source_header, sizeof(copy_source_header), "x-amz-copy-source:%s\n", copy_source);
    }

    snprintf(canonical_headers, sizeof(canonical_headers), 
             "%s"
             "host:%s\n"
             "x-amz-content-sha256:%s\n"
             "%s"
             "x-amz-date:%s\n", 
             content_type_header, 
             host, 
             payload_hash, 
             copy_source_header, 
             amz_date);

    snprintf(signed_headers, sizeof(signed_headers), 
             "%s"
             "host;"
             "x-amz-content-sha256;"
             "%s"
             "x-amz-date", 
             (content_type && strlen(content_type) > 0) ? "content-type;" : "",
             (copy_source && strlen(copy_source) > 0) ? "x-amz-copy-source;" : "");

    snprintf(canonical_request, sizeof(canonical_request), "%s\n%s\n%s\n%s\n%s\n%s",
             method, path, query ? query : "", canonical_headers, signed_headers, payload_hash);

    sha256_hex(canonical_request, strlen(canonical_request), canonical_request_hash);

    snprintf(credential_scope, sizeof(credential_scope), "%s/%s/s3/aws4_request", date_stamp, region);

    snprintf(string_to_sign, sizeof(string_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, credential_scope, canonical_request_hash);

    snprintf(aws4_secret, sizeof(aws4_secret), "AWS4%s", secret_key);
    hmac_sha256((unsigned char*)aws4_secret, strlen(aws4_secret), date_stamp, strlen(date_stamp), kDate, &kDate_len);

    hmac_sha256(kDate, kDate_len, region, strlen(region), kRegion, &kRegion_len);

    hmac_sha256(kRegion, kRegion_len, "s3", 2, kService, &kService_len);

    hmac_sha256(kService, kService_len, "aws4_request", 12, kSigning, &kSigning_len);

    hmac_sha256(kSigning, kSigning_len, string_to_sign, strlen(string_to_sign), signature, &signature_len);

    for(i = 0; i < signature_len; i++) {
        sprintf(signature_hex + (i * 2), "%02x", signature[i]);
    }
    signature_hex[64] = '\0';

    auth_headers = malloc(4096);
    snprintf(auth_headers, 4096, 
             "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s\r\n"
             "x-amz-date: %s\r\n"
             "x-amz-content-sha256: %s\r\n"
             "%s%s%s"
             "%s%s%s",
             access_key, credential_scope, signed_headers, signature_hex, amz_date, payload_hash,
             (content_type && strlen(content_type) > 0) ? "Content-Type: " : "", 
             (content_type && strlen(content_type) > 0) ? content_type : "", 
             (content_type && strlen(content_type) > 0) ? "\r\n" : "",
             (copy_source && strlen(copy_source) > 0) ? "x-amz-copy-source: " : "", 
             (copy_source && strlen(copy_source) > 0) ? copy_source : "", 
             (copy_source && strlen(copy_source) > 0) ? "\r\n" : "");

    return auth_headers;
}

char* s3_create_presigned_url(
    const char *method,
    const char *host,
    const char *protocol,
    const char *path,
    const char *region,
    const char *access_key,
    const char *secret_key,
    long expires
) {
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char date_stamp[9];
    char amz_date[17];
    char credential_scope[256];
    char credential_scope_encoded[256];
    char canonical_query[1024];
    char canonical_request[2048];
    char canonical_request_hash[65];
    char string_to_sign[1024];
    unsigned char kDate[EVP_MAX_MD_SIZE];
    unsigned int kDate_len;
    char aws4_secret[256];
    unsigned char kRegion[EVP_MAX_MD_SIZE];
    unsigned int kRegion_len;
    unsigned char kService[EVP_MAX_MD_SIZE];
    unsigned int kService_len;
    unsigned char kSigning[EVP_MAX_MD_SIZE];
    unsigned int kSigning_len;
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_len;
    char signature_hex[65];
    char *final_url = malloc(4096);
    unsigned int i;

    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", tm_info);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", tm_info);

    snprintf(credential_scope, sizeof(credential_scope), "%s/%s/s3/aws4_request", date_stamp, region);
    snprintf(credential_scope_encoded, sizeof(credential_scope_encoded), "%s%%2F%s%%2Fs3%%2Faws4_request", date_stamp, region);

    snprintf(canonical_query, sizeof(canonical_query),
             "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=%s%%2F%s&X-Amz-Date=%s&X-Amz-Expires=%ld&X-Amz-SignedHeaders=host",
             access_key, credential_scope_encoded, amz_date, expires);

    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n%s\n%s\nhost:%s\n\nhost\nUNSIGNED-PAYLOAD",
             method, path, canonical_query, host);

    sha256_hex(canonical_request, strlen(canonical_request), canonical_request_hash);

    snprintf(string_to_sign, sizeof(string_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, credential_scope, canonical_request_hash);

    snprintf(aws4_secret, sizeof(aws4_secret), "AWS4%s", secret_key);
    hmac_sha256((unsigned char*)aws4_secret, strlen(aws4_secret), date_stamp, strlen(date_stamp), kDate, &kDate_len);

    hmac_sha256(kDate, kDate_len, region, strlen(region), kRegion, &kRegion_len);

    hmac_sha256(kRegion, kRegion_len, "s3", 2, kService, &kService_len);

    hmac_sha256(kService, kService_len, "aws4_request", 12, kSigning, &kSigning_len);

    hmac_sha256(kSigning, kSigning_len, string_to_sign, strlen(string_to_sign), signature, &signature_len);

    for(i = 0; i < signature_len; i++) {
        sprintf(signature_hex + (i * 2), "%02x", signature[i]);
    }
    signature_hex[64] = '\0';

    snprintf(final_url, 4096, "%s%s%s?%s&X-Amz-Signature=%s", protocol, host, path, canonical_query, signature_hex);

    return final_url;
}
