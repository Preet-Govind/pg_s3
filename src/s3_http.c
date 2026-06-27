#include "s3_http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

struct MemoryStruct {
  char *memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
 
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) return 0;
 
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
 
  return realsize;
}

static size_t WriteFileCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    FILE *fp = (FILE *)userp;
    return fwrite(contents, size, nmemb, fp);
}

static size_t ReadFileCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    FILE *fp = (FILE *)userdata;
    return fread(buffer, size, nitems, fp);
}

HttpResponse s3_http_request(const char *method, const char *url, const char *headers_str, const char *body, size_t body_len, const char *content_type) {
    CURL *curl;
    CURLcode res;
    HttpResponse response = {NULL, 0, 0};
    struct curl_slist *chunk = NULL;
    struct MemoryStruct chunk_mem;
    chunk_mem.memory = malloc(1);
    chunk_mem.size = 0;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        
        if (headers_str && headers_str[0]) {
            char *headers_copy = strdup(headers_str);
            char *saveptr;
            char *line = strtok_r(headers_copy, "\r\n", &saveptr);
            while (line != NULL) {
                chunk = curl_slist_append(chunk, line);
                line = strtok_r(NULL, "\r\n", &saveptr);
            }
            free(headers_copy);
        }

        if (chunk) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        }

        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk_mem);

        res = curl_easy_perform(curl);

        if(res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
            response.data = chunk_mem.memory;
            response.size = chunk_mem.size;
        } else {
            free(chunk_mem.memory);
            response.data = NULL;
        }

        curl_easy_cleanup(curl);
        if(chunk) curl_slist_free_all(chunk);
    } else {
        free(chunk_mem.memory);
    }
    return response;
}

void free_http_response(HttpResponse *resp) {
    if (resp && resp->data) {
        free(resp->data);
        resp->data = NULL;
        resp->size = 0;
    }
}

long s3_http_request_download(const char *url, const char *headers_str, const char *filepath) {
    CURL *curl;
    CURLcode res;
    long status_code = 0;
    struct curl_slist *chunk = NULL;

    FILE *fp = fopen(filepath, "wb");
    if (!fp) return 0;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        
        if (headers_str && headers_str[0]) {
            char *headers_copy = strdup(headers_str);
            char *saveptr;
            char *line = strtok_r(headers_copy, "\r\n", &saveptr);
            while (line != NULL) {
                chunk = curl_slist_append(chunk, line);
                line = strtok_r(NULL, "\r\n", &saveptr);
            }
            free(headers_copy);
        }

        if (chunk) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)fp);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        }

        curl_easy_cleanup(curl);
        if(chunk) curl_slist_free_all(chunk);
    }
    
    fclose(fp);
    return status_code;
}

long s3_http_request_upload(const char *method, const char *url, const char *headers_str, const char *filepath, size_t file_size) {
    CURL *curl;
    CURLcode res;
    long status_code = 0;
    struct curl_slist *chunk = NULL;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return 0;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        
        if (headers_str && headers_str[0]) {
            char *headers_copy = strdup(headers_str);
            char *saveptr;
            char *line = strtok_r(headers_copy, "\r\n", &saveptr);
            while (line != NULL) {
                chunk = curl_slist_append(chunk, line);
                line = strtok_r(NULL, "\r\n", &saveptr);
            }
            free(headers_copy);
        }

        if (chunk) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        }

        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadFileCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, (void *)fp);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size);

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        }

        curl_easy_cleanup(curl);
        if(chunk) curl_slist_free_all(chunk);
    }
    
    fclose(fp);
    return status_code;
}
