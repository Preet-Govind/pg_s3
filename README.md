# pg_s3

`pg_s3` is a native PostgreSQL extension that provides access to S3-compatible object storage directly from SQL. It is being built as an alternative to Boto3 that runs natively inside PostgreSQL, minimizing external dependencies by using pure C with `libcurl` and `openssl`.

## Why
Isn't it be boring to write 50+ lines of code just to move files of a few MBs , keep orchestration apart ?

## Features
- **Native SQL API:** Simple, intuitive functions for managing S3 objects.
- **AWS Signature Version 4:** Fully authenticated requests to any S3-compatible storage.
- **Stateless/Configurable:** Connection properties are configured via GUC variables for ease of use, with support for per-query credential overrides.
- **Botocore-Grade Errors:** S3 XML errors are natively propagated to the PostgreSQL error output via `ereport(ERROR)`.
- **Zero-Memory Copying:** Native support for the S3 `CopyObject` API for instant, memory-free transfers within the same ecosystem.
- **Cross-Cloud Migration:** Ability to pipe data between isolated S3 clusters using pure SQL.
- **Pagination Support:** Advanced `continuation_token` handling to seamlessly query buckets with >1,000 objects.
- **Dedicated Schema:** Automatically isolates all functions into the `ext_pg_s3` schema to prevent namespace pollution.

---

## Testing Environment (MinIO)

If you need a local testing playground, you can instantly spin up MinIO instances using Docker. 

```bash
# Setup Primary Cluster (Port 9000)
sudo docker run -p 9000:9000 -p 9001:9001 \
  -e MINIO_ROOT_USER=minioadmin \
  -e MINIO_ROOT_PASSWORD=minioadmin \
  quay.io/minio/minio server /data --console-address ":9001"

# Setup Secondary/Client Cluster for Cross-Cloud Testing (Port 10000)
sudo docker run -p 10000:10000 -p 10001:10001 \
  -e MINIO_ROOT_USER=minioadmin \
  -e MINIO_ROOT_PASSWORD=minioadmin \
  quay.io/minio/minio server /data --console-address ":10001"
```

---

## Configuration

Set the global configuration variables in `postgresql.conf` or using `SET` in your session:
```sql
SET pg_s3.endpoint = 'https://s3.us-east-1.amazonaws.com';
SET pg_s3.region = 'us-east-1';
SET pg_s3.access_key = 'YOUR_ACCESS_KEY';
SET pg_s3.secret_key = 'YOUR_SECRET_KEY';
SET pg_s3.bucket = 'your-bucket-name';
SET pg_s3.use_temp_files = false; -- SET to true for streaming massive files without OOM crashes 
```

The extension creates a schema called `ext_pg_s3`, had to prepend `ext_` as `pg_` is reserved.
---

## SQL API

You can rely on the GUC variables set above, or you can pass credentials dynamically to the functions via named parameters to connect to multiple different buckets/endpoints simultaneously. 

### Object Operations

**1. Upload an Object (`s3_put`)**
```sql
-- Using default GUC credentials
select ext_pg_s3.s3_put('my-folder/hello.txt', 'hello, world!');

-- Overriding credentials explicitly (Connecting to custom MinIO)
SELECT ext_pg_s3.s3_put(
    object_key => 'reports/Q3.pdf', content => 'Binary or Text Data', content_type => 'application/pdf', 
    endpoint => 'http://172.17.0.2:9000', region => 'us-east-1', 
    access_key => 'minioadmin', secret_key => 'minioadmin', 
    bucket => 'test-bkt'
);
```

**2. Download an Object (`s3_get`)**
```sql
SELECT ext_pg_s3.s3_get(
    object_key => 'reports/Q3.pdf',
    bucket => 'test-bkt',endpoint => 'http://172.17.0.2:9000',region => 'us-east-1',
    access_key => 'minioadmin',secret_key => 'minioadmin'
);
```

**3. Delete an Object (`s3_delete`)**
```sql
SELECT ext_pg_s3.s3_delete('reports/Q3.pdf');
```

**4. Generate Presigned URL (`s3_presign`)**
Instantly create an authenticated URL valid for a given duration (in seconds), without making any external HTTP calls.
```sql
SELECT ext_pg_s3.s3_presign(
    object_key => 'reports/Q3.pdf', bucket => 'test-bkt', expires => 3600 --1hr
);
```

**5. Native Server-Side Copy (`s3_copy`)**
Natively copy objects between buckets or keys directly on the S3 backend without downloading the file to PostgreSQL. Consumes 0 bytes of memory.
```sql
SELECT ext_pg_s3.s3_copy(
    source_key => 'old_folder/data.csv', target_key => 'new_folder/data.csv',
    source_bucket => 'my-source-bucket', target_bucket => 'my-target-bucket'
);
```

**6. Cross-Cloud Migration (Moving data between isolated clusters)**
If you need to push data from your S3 bucket to a client's entirely separate S3 bucket, PostgreSQL can bridge the gap dynamically:
```sql
SELECT ext_pg_s3.s3_put(
    -- 1. Where it's going (Target Cluster on port 10000)
    object_key => 'q3_stats.pdf',bucket => 'client-bucket',
    endpoint => 'http://172.17.0.3:9000',region => 'us-east-1',
    access_key => 'minioadmin',secret_key => 'minioadmin',
    
    -- 2. Where it's coming from (Source Cluster on port 9000)
    content => ext_pg_s3.s3_get(
        object_key => 'reports/final_q3.pdf',
        bucket => 'test-bkt',endpoint => 'http://172.17.0.2:9000',region => 'us-east-1',
        access_key => 'minioadmin',secret_key => 'minioadmin'
    )
);
```
Please pay attention to the endpoints.

**7. Native Query Export (`s3_export`)**
Stream PostgreSQL query results natively into S3. Support formats include `csv`, `text` (tab-separated), and `parquet`. For Parquet, it leverages the embedded Apache Arrow C++ engine with ZSTD compression. By default, it operates entirely in RAM for instant, zero-disk serialization. For massive queries, you can enable `SET pg_s3.use_temp_files = true;` to safely spool the export via disk `/tmp` and stream it chunk-by-chunk to S3!
```sql
SELECT ext_pg_s3.s3_export(
    query => 'SELECT * FROM <table_name> WHERE condition = true',
    object_key => 'exports/data.parquet', format => 'parquet', -- Supported: 'csv', 'text', 'parquet' 
    bucket => 'test-bkt', endpoint => 'http://172.17.0.2:9000',region => 'us-east-1',
    access_key => 'minioadmin', secret_key => 'minioadmin'
);
```

**8. Native Set Returning Importer (`s3_read`)**
Ingest `csv`, `json`, and `parquet` files directly from S3 into PostgreSQL memory natively as virtual tables using the Apache Arrow C++ parser. This allows you to query cloud data structures with standard SQL statements! For massive gigabyte-scale files, ensure you run `SET pg_s3.use_temp_files = true;` first, which forces the system to stream the S3 file into a temporary `/tmp` file, memory-map it, and instantly unlink it via `PG_TRY()` garbage collection without blowing up your server RAM.
```sql
SELECT * FROM ext_pg_s3.s3_read(
    object_key => 'data.parquet', format => 'parquet', -- Supported: 'csv', 'json', 'parquet'
    bucket => 'test-bkt'
) AS t(id int, name text);
```

### Bucket Operations

**1. Create a Bucket (`s3_create_bucket`)**
```sql
SELECT ext_pg_s3.s3_create_bucket(
    bucket => 'client-bucket', 
    endpoint => 'http://172.17.0.3:9000', region => 'us-east-1',
    access_key => 'minioadmin', secret_key => 'minioadmin'
);
```

**2. Delete a Bucket (`s3_delete_bucket`)**
```sql
SELECT ext_pg_s3.s3_delete_bucket(bucket => 'client-bucket');
```

**3. List Objects with Pagination (`s3_list`)**
Returns a parsed table of objects in the bucket. Since S3 restricts lists to 1000 items, you can pass a `continuation_token` to fetch the next batch.
```sql
-- Fetch first batch
SELECT * FROM ext_pg_s3.s3_list(
    prefix => '',
    region => 'us-east-1', endpoint => 'http://172.17.0.2:9000',access_key => 'minioadmin',secret_key => 'minioadmin',
    bucket => 'test-bkt'
);

-- Fetch next batch using the token from the previous call
SELECT * FROM ext_pg_s3.s3_list(
    prefix => '',continuation_token => '1ueGcxL9...',bucket => 'test-bkt'
);
```

---

## Compilation

Have provided a convenient `build.sh` script to clean, compile, and install the extension into your PostgreSQL directory, but prior to that ensure get_libs.sh is executed.

```bash
chmod +x get_libs.sh
chmod +x build.sh

sudo ./get_libs.sh
sudo ./build.sh
```

Then in PostgreSQL:
```sql
drop extension if exists  pg_s3 ; 
create extension pg_s3 ;
```



### What intentionally DID NOT implement:
1. **Multipart Uploads (`upload_part`, `create_multipart_upload`):** S3 allows single PUTs up to 5 GB. Since PostgreSQL itself has a hard limit of 1 GB for a single `text` cell, we don't need multipart uploads. A standard `s3_put` is already perfectly optimized for anything PostgreSQL can hold.
2. **Advanced Bucket Administration:** Things like `put_bucket_cors`, `put_bucket_lifecycle_configuration`, `put_object_acl`, and `put_bucket_versioning`. These are usually infrastructure-as-code tasks (like Terraform) and don't typically belong in daily SQL queries.
3. **DeleteObjects (Multi-delete):** We only have single object deletion right now. 

### Conclusion
it isolates itself into the `ext_pg_s3` schema, properly handles AWS Signature V4 without external libraries, parses native S3 XML errors directly into PostgreSQL error logs, handles pagination gracefully, and allows dynamic cross-cloud migration. 

PS : Check for the docker MinIO ports carefully

---

## Developer Notes / Contributing

If you wish to contribute to the extension or understand the codebase, here is a concise guide to the architecture:

**Core Architecture & Files:**
*   **`src/pg_s3.c`**: The primary PostgreSQL interface. This file registers the SQL functions (e.g., `pg_s3_get`, `pg_s3_export`), extracts arguments, sets up GUC (Global User Configuration) variables, and connects to the S3 logic. For `s3_export`, it uses the PostgreSQL Server Programming Interface (SPI) to execute queries natively.
*   **`src/s3_api.c`**: The abstraction layer mapping PostgreSQL tasks (e.g., PUT, GET) into lower-level HTTP requests.
*   **`src/s3_http.c`**: The core HTTP engine. It leverages `libcurl` to make network requests. It processes memory safely using PostgreSQL's `palloc` and returns `bytea` buffers.
*   **`src/s3_auth.c`**: Implementation of AWS Signature Version 4. It uses OpenSSL (`libcrypto`) to compute SHA256 hashes and HMAC signatures dynamically for every request.
*   **`src/s3_parquet.cpp`**: C++ integration with Apache Arrow. Because Arrow requires C++20, this file bridges the C-based PostgreSQL SPI with modern C++ memory arrays to build ZSTD-compressed Parquet files in RAM.
*   **`Makefile`**: Configured to use PGXS (PostgreSQL Extension System). Note the special compilation targets for `src/s3_parquet.o` and `src/s3_parquet.bc` which enforce the `clang++-19` / `g++ -std=c++20` requirement.

**Adding New Features:**
1.  **C Logic:** Add your feature logic in `pg_s3.c` and create corresponding functions in `s3_api.c`.
2.  **Memory Management:** Always use `palloc()` and `pfree()` instead of standard `malloc()` when within the PostgreSQL context to prevent memory leaks during query aborts. Use `StringInfo` for dynamic string building.
3.  **SQL Binding:** After adding a C function with `PG_FUNCTION_INFO_V1(my_new_func)`, update `sql/pg_s3--1.0.sql` to expose the function to SQL.
4.  **Error Handling:** Use `ereport(ERROR, ...)` so errors seamlessly propagate to the SQL client.
5.  **Rebuilding:** Simply run `sudo ./build.sh` and `DROP EXTENSION pg_s3; CREATE EXTENSION pg_s3;` to reload your changes.

