/*
 * pg_s3.c
 * Core Postgresql bindings and logic for the pg_s3 extension.
 *
 * dev notes:
 * - This file contains the C-level implementations of the SQL functions.
 * - use palloc/pfree for memory management so PostgreSQL can track and free 
 *   memory in case of transaction aborts/errors.
 * - ereport(ERROR, ...) will throw a PostgreSQL error and abort the current transaction.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "s3_api.h"
#include "s3_parquet.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include <unistd.h>

PG_MODULE_MAGIC;

char *s3_endpoint = NULL;
char *s3_region = NULL;
char *s3_access_key = NULL;
char *s3_secret_key = NULL;
char *s3_bucket = NULL;
bool s3_use_temp_files = false;
bool s3_use_virtual_host = false;

void _PG_init(void);

PGDLLEXPORT Datum pg_s3_get(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_put(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_delete(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_list(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_create_bucket(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_delete_bucket(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_presign(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_copy(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_export(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pg_s3_read(PG_FUNCTION_ARGS);

void _PG_init(void) {
    // define GUC vars for the extension
    DefineCustomStringVariable("pg_s3.endpoint",
                               "S3 Endpoint URL",
                               NULL,
                               &s3_endpoint,
                               "",
                               PGC_USERSET,
                               0, NULL, NULL, NULL);

    DefineCustomStringVariable("pg_s3.region",
                               "S3 Region",
                               NULL,
                               &s3_region,
                               "us-east-1",
                               PGC_USERSET,
                               0, NULL, NULL, NULL);

    DefineCustomStringVariable("pg_s3.access_key",
                               "S3 Access Key",
                               NULL,
                               &s3_access_key,
                               "",
                               PGC_USERSET,
                               0, NULL, NULL, NULL);

    DefineCustomStringVariable("pg_s3.secret_key",
                               "S3 Secret Key",
                               NULL,
                               &s3_secret_key,
                               "",
                               PGC_USERSET,
                               0, NULL, NULL, NULL);

    DefineCustomStringVariable("pg_s3.bucket",
                               "S3 Bucket",
                               NULL,
                               &s3_bucket,
                               "",
                               PGC_USERSET,
                               0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_s3.use_temp_files",
                             "Use temporary files for large datasets instead of RAM",
                             NULL,
                             &s3_use_temp_files,
                             false,
                             PGC_USERSET,
                             0, NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_s3.use_virtual_host",
                             "Use Virtual-Hosted Style URLs (e.g. bucket.s3.amazonaws.com) instead of Path-Style",
                             NULL,
                             &s3_use_virtual_host,
                             false,
                             PGC_USERSET,
                             0, NULL, NULL, NULL);
}

typedef struct {
    char *endpoint;
    char *region;
    char *access_key;
    char *secret_key;
    char *bucket;
} S3Config;

static void extract_s3_config(PG_FUNCTION_ARGS, int start_arg_idx, S3Config *config) {
    config->endpoint = s3_endpoint;
    config->region = s3_region;
    config->access_key = s3_access_key;
    config->secret_key = s3_secret_key;
    config->bucket = s3_bucket;

    if (PG_NARGS() > start_arg_idx && !PG_ARGISNULL(start_arg_idx)) {
        config->endpoint = text_to_cstring(PG_GETARG_TEXT_PP(start_arg_idx));
    }
    if (PG_NARGS() > start_arg_idx + 1 && !PG_ARGISNULL(start_arg_idx + 1)) {
        config->region = text_to_cstring(PG_GETARG_TEXT_PP(start_arg_idx + 1));
    }
    if (PG_NARGS() > start_arg_idx + 2 && !PG_ARGISNULL(start_arg_idx + 2)) {
        config->access_key = text_to_cstring(PG_GETARG_TEXT_PP(start_arg_idx + 2));
    }
    if (PG_NARGS() > start_arg_idx + 3 && !PG_ARGISNULL(start_arg_idx + 3)) {
        config->secret_key = text_to_cstring(PG_GETARG_TEXT_PP(start_arg_idx + 3));
    }
    if (PG_NARGS() > start_arg_idx + 4 && !PG_ARGISNULL(start_arg_idx + 4)) {
        config->bucket = text_to_cstring(PG_GETARG_TEXT_PP(start_arg_idx + 4));
    }
    // ensure creds
    if (!config->endpoint || !config->endpoint[0])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("S3 endpoint is not set. Provide it as an argument or set pg_s3.endpoint")));
    if (!config->region || !config->region[0])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("S3 region is not set. Provide it as an argument or set pg_s3.region")));
    if (!config->access_key || !config->access_key[0])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("S3 access_key is not set. Provide it as an argument or set pg_s3.access_key")));
    if (!config->secret_key || !config->secret_key[0])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("S3 secret_key is not set. Provide it as an argument or set pg_s3.secret_key")));
    if (!config->bucket || !config->bucket[0])
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("S3 bucket is not set. Provide it as an argument or set pg_s3.bucket")));
}

PG_FUNCTION_INFO_V1(pg_s3_get);
// sql binding: ext_pg_s3.s3_get
PGDLLEXPORT Datum pg_s3_get(PG_FUNCTION_ARGS) {
    char *object_key;
    char *response = NULL;
    bytea *result;
    size_t out_len = 0;
    S3Config config;

    if (PG_ARGISNULL(0)) PG_RETURN_NULL();

    object_key = text_to_cstring(PG_GETARG_TEXT_PP(0));

    extract_s3_config(fcinfo, 1, &config);

    response = s3_api_get(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, &out_len);
    
    if (!response) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to GET object from S3")));
    }

    result = (bytea *) palloc(out_len + VARHDRSZ);
    SET_VARSIZE(result, out_len + VARHDRSZ);
    if (out_len > 0) {
        memcpy(VARDATA(result), response, out_len);
    }
    free(response);
    
    PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(pg_s3_put);
// sql binding: ext_pg_s3.s3_put
PGDLLEXPORT Datum pg_s3_put(PG_FUNCTION_ARGS) {
    char *object_key;
    char *content;
    size_t content_len;
    char *content_type = "application/octet-stream";
    bool success;
    S3Config config;
    bytea *content_bytea;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();

    object_key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    content_bytea = PG_GETARG_BYTEA_PP(1);
    content_len = VARSIZE_ANY_EXHDR(content_bytea);
    content = VARDATA_ANY(content_bytea);
    
    if (!PG_ARGISNULL(2)) {
        content_type = text_to_cstring(PG_GETARG_TEXT_PP(2));
    }

    extract_s3_config(fcinfo, 3, &config);

    success = s3_api_put(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, content, content_len, content_type);
    
    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to PUT object to S3")));
    }

    PG_RETURN_BOOL(success);
}

PG_FUNCTION_INFO_V1(pg_s3_delete);
// sql binding: ext_pg_s3.s3_delete
PGDLLEXPORT Datum pg_s3_delete(PG_FUNCTION_ARGS) {
    char *object_key;
    bool success;
    S3Config config;

    if (PG_ARGISNULL(0)) PG_RETURN_NULL();

    object_key = text_to_cstring(PG_GETARG_TEXT_PP(0));

    extract_s3_config(fcinfo, 1, &config);

    success = s3_api_delete(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key);
    
    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to DELETE object from S3")));
    }

    PG_RETURN_BOOL(success);
}

PG_FUNCTION_INFO_V1(pg_s3_list);
// sql binding: ext_pg_s3.s3_list
PGDLLEXPORT Datum pg_s3_list(PG_FUNCTION_ARGS) {
    char *prefix = "";
    char *continuation_token = "";
    char *response = NULL;
    text *result;
    S3Config config;

    if (!PG_ARGISNULL(0)) {
        prefix = text_to_cstring(PG_GETARG_TEXT_PP(0));
    }
    if (!PG_ARGISNULL(1)) {
        continuation_token = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }

    extract_s3_config(fcinfo, 2, &config);

    response = s3_api_list(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, prefix, continuation_token);
    
    if (!response) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to LIST objects from S3")));
    }

    result = cstring_to_text(response);
    free(response);
    
    PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pg_s3_create_bucket);
// sql binding: ext_pg_s3.s3_create_bucket
PGDLLEXPORT Datum pg_s3_create_bucket(PG_FUNCTION_ARGS) {
    bool success;
    S3Config config;

    extract_s3_config(fcinfo, 0, &config);

    success = s3_api_create_bucket(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket);
    
    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to CREATE bucket in S3")));
    }
    // free(success) // stupid thing i did
    PG_RETURN_BOOL(success);
}

PG_FUNCTION_INFO_V1(pg_s3_delete_bucket);
// sql binding: ext_pg_s3.s3_delete_bucket
PGDLLEXPORT Datum pg_s3_delete_bucket(PG_FUNCTION_ARGS) {
    bool success;
    S3Config config;

    extract_s3_config(fcinfo, 0, &config);

    success = s3_api_delete_bucket(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket);
    
    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to DELETE bucket from S3")));
    }

    PG_RETURN_BOOL(success);
}

PG_FUNCTION_INFO_V1(pg_s3_presign);
// sql binding: ext_pg_s3.s3_presign
PGDLLEXPORT Datum pg_s3_presign(PG_FUNCTION_ARGS) {
    char *object_key;
    long expires = 3600;
    char *response = NULL;
    text *result;
    S3Config config;

    if (PG_ARGISNULL(0)) PG_RETURN_NULL();

    object_key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    
    if (!PG_ARGISNULL(1)) {
        expires = PG_GETARG_INT64(1);
    }

    extract_s3_config(fcinfo, 2, &config);

    response = s3_api_presign(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, expires);
    
    if (!response) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to PRESIGN URL for S3")));
    }

    result = cstring_to_text(response);
    free(response);
    
    PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pg_s3_copy);
// sql binding: ext_pg_s3.s3_copy
PGDLLEXPORT Datum pg_s3_copy(PG_FUNCTION_ARGS) {
    char *source_key;
    char *target_key;
    char *source_bucket;
    char *target_bucket;
    bool success;
    S3Config config;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();

    source_key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    target_key = text_to_cstring(PG_GETARG_TEXT_PP(1));

    extract_s3_config(fcinfo, 4, &config);

    if (!PG_ARGISNULL(2)) {
        source_bucket = text_to_cstring(PG_GETARG_TEXT_PP(2));
    } else {
        source_bucket = config.bucket;
    }
    
    if (!PG_ARGISNULL(3)) {
        target_bucket = text_to_cstring(PG_GETARG_TEXT_PP(3));
    } else {
        target_bucket = config.bucket;
    }

    success = s3_api_copy(config.endpoint, config.region, config.access_key, config.secret_key, source_bucket, source_key, target_bucket, target_key);
    
    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to COPY object in S3")));
    }

    PG_RETURN_BOOL(success);
}

/*
 * pg_s3_export
 * executes a SELECT query natively via SPI and streams 
 * the results into an in-memory buffer (CSV/Text) or a Parquet file via Apache Arrow,
 * then uploads directly to S3.
 * 
 * Note: SPI allows C extensions to run queries in the current transaction natively.
 */
PG_FUNCTION_INFO_V1(pg_s3_export);
// sql binding: ext_pg_s3.s3_export
PGDLLEXPORT Datum pg_s3_export(PG_FUNCTION_ARGS) {
    char *query;
    char *object_key;
    char *format;
    char *content_type = "text/csv";
    bool success;
    S3Config config;
    int ret;
    StringInfoData buf;
    char *final_payload = NULL;
    size_t final_len = 0;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2)) PG_RETURN_NULL();

    query = text_to_cstring(PG_GETARG_TEXT_PP(0));
    object_key = text_to_cstring(PG_GETARG_TEXT_PP(1));
    format = text_to_cstring(PG_GETARG_TEXT_PP(2));

    extract_s3_config(fcinfo, 3, &config);

    if (strcmp(format, "csv") == 0) {
        content_type = "text/csv";
    } else if (strcmp(format, "text") == 0) {
        content_type = "text/plain";
    } else if (strcmp(format, "parquet") == 0) {
        content_type = "application/vnd.apache.parquet"; // for parquet , vnd.apache. to be prepended
    } else {
        ereport(ERROR, (errmsg("Unsupported format. Use 'csv', 'text', or 'parquet'.")));
    }

    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errmsg("Failed to connect to SPI")));
    }

    ret = SPI_execute(query, true, 0);
    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        ereport(ERROR, (errmsg("Query must be a SELECT statement")));
    }

    initStringInfo(&buf);

    if (SPI_processed > 0 && SPI_tuptable != NULL) {
        if (strcmp(format, "parquet") == 0) {
            if (s3_use_temp_files) {
                char filepath[1024];
                snprintf(filepath, sizeof(filepath), "/tmp/pg_s3_export_%d.parquet", MyProcPid);
                
                PG_TRY();
                {
                    build_parquet_from_spi_to_file(SPI_tuptable, SPI_processed, filepath);
                    success = s3_api_put_from_file(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, filepath, content_type);
                    unlink(filepath);
                }
                PG_CATCH();
                {
                    unlink(filepath);
                    PG_RE_THROW();
                }
                PG_END_TRY();
                
                SPI_finish();
                if (!success) {
                    ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to EXPORT query to S3 via file spooling")));
                }
                PG_RETURN_BOOL(success);
            } else {
                final_payload = build_parquet_from_spi(SPI_tuptable, SPI_processed, &final_len);
            }
        } else {
            // csv / txt string generation
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            
            // write header for csv
            if (strcmp(format, "csv") == 0) {
                for (int j = 1; j <= tupdesc->natts; j++) {
                    appendStringInfoString(&buf, NameStr(tupdesc->attrs[j - 1].attname));
                    if (j < tupdesc->natts) appendStringInfoChar(&buf, ',');
                }
                appendStringInfoChar(&buf, '\n');
            }

            // write rows , hope qoutes work !
            for (uint64 i = 0; i < SPI_processed; i++) {
                HeapTuple tuple = SPI_tuptable->vals[i];
                for (int j = 1; j <= tupdesc->natts; j++) {
                    char *val = SPI_getvalue(tuple, tupdesc, j);
                    if (val) {
                        if (strcmp(format, "csv") == 0) {
                            bool needs_quotes = strchr(val, ',') || strchr(val, '"') || strchr(val, '\n') || strchr(val, '\r');
                            if (needs_quotes) {
                                appendStringInfoChar(&buf, '"');
                                for (char *c = val; *c; c++) {
                                    if (*c == '"') appendStringInfoChar(&buf, '"');
                                    appendStringInfoChar(&buf, *c);
                                }
                                appendStringInfoChar(&buf, '"');
                            } else {
                                appendStringInfoString(&buf, val);
                            }
                        } else {
                            appendStringInfoString(&buf, val);
                        }
                        pfree(val);
                    }
                    if (j < tupdesc->natts) {
                        appendStringInfoChar(&buf, (strcmp(format, "csv") == 0) ? ',' : '\t');
                    }
                }
                appendStringInfoChar(&buf, '\n');
            }
            final_payload = buf.data;
            final_len = buf.len;
        }
    }

    if (final_payload) {
        success = s3_api_put(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, final_payload, final_len, content_type);
        if (strcmp(format, "parquet") == 0) {
            pfree(final_payload);
        } else {
            pfree(buf.data);
        }
    } else {
        success = s3_api_put(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, "", 0, content_type);
    }
    
    SPI_finish();

    if (!success) {
        ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to EXPORT query to S3")));
    }

    PG_RETURN_BOOL(success);
}

#include "funcapi.h"

/*
 * pg_s3_read
 * reads csv, json, or parquet files from s3 and yields them natively as a 
 * pg's virtual table SFI.
 */
PG_FUNCTION_INFO_V1(pg_s3_read);
PGDLLEXPORT Datum pg_s3_read(PG_FUNCTION_ARGS) {
    char *object_key;
    char *format;
    S3Config config;
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;
    size_t out_len = 0;
    char *response;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(ERROR, (errmsg("s3_read must be called as a set-returning function (e.g., FROM ext_pg_s3.s3_read(...))")));
    }

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(ERROR, (errmsg("return type must be a row type (use AS t(col1 type, ...))")));
    }

    object_key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    format = text_to_cstring(PG_GETARG_TEXT_PP(1));

    extract_s3_config(fcinfo, 2, &config);

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    MemoryContextSwitchTo(oldcontext);

    if (s3_use_temp_files) {
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "/tmp/pg_s3_read_%d.tmp", MyProcPid);

        PG_TRY();
        {
            bool status = s3_api_get_to_file(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, filepath);
            if (!status) {
                ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to fetch object from S3 into temporary file")));
            }
            
            build_tuplestore_from_arrow_file(filepath, format, tupdesc, tupstore);
            unlink(filepath);
        }
        PG_CATCH();
        {
            unlink(filepath);
            PG_RE_THROW();
        }
        PG_END_TRY();
    } else {
        response = s3_api_get(config.endpoint, config.region, config.access_key, config.secret_key, config.bucket, object_key, &out_len);
        
        if (!response) {
            ereport(ERROR, (errcode(ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION), errmsg("Failed to fetch object from S3 for reading")));
        }

        build_tuplestore_from_arrow(response, out_len, format, tupdesc, tupstore);
        free(response);
    }

    return (Datum) 0;
}
