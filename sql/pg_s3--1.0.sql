-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_s3" to load this file. \quit

SET pg_s3.use_temp_files = true;

SET pg_s3.use_virtual_host = true; -- when locally set to false

-- SET pg_s3.endpoint = 'https://s3.us-east-1.amazonaws.com';
-- SET pg_s3.region = 'us-east-1';
-- SET pg_s3.access_key = 'YOUR_ACCESS_KEY';
-- SET pg_s3.secret_key = 'YOUR_SECRET_KEY';
-- SET pg_s3.bucket = 'your-bucket-name';


CREATE FUNCTION s3_get(
    object_key text,
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_s3_get'
LANGUAGE C;

CREATE FUNCTION s3_put(
    object_key text,
    content bytea,
    content_type text DEFAULT 'application/octet-stream',
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_put'
LANGUAGE C;

CREATE FUNCTION s3_delete(
    object_key text,
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_delete'
LANGUAGE C;

CREATE FUNCTION s3_create_bucket(
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_create_bucket'
LANGUAGE C;

CREATE FUNCTION s3_delete_bucket(
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_delete_bucket'
LANGUAGE C;

CREATE FUNCTION s3_presign(
    object_key text,
    expires bigint DEFAULT 3600,
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_s3_presign'
LANGUAGE C;

CREATE FUNCTION s3_list_raw(
    prefix text DEFAULT '',
    continuation_token text DEFAULT '',
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_s3_list'
LANGUAGE C;

CREATE FUNCTION s3_list(
    prefix text DEFAULT '',
    continuation_token text DEFAULT '',
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS TABLE (
    key text,
    last_modified timestamp,
    etag text,
    size bigint,
    storage_class text,
    is_truncated boolean,
    next_continuation_token text
)
AS $$
DECLARE
    raw_xml text;
    ns text[][] := ARRAY[ARRAY['s3', 'http://s3.amazonaws.com/doc/2006-03-01/']];
BEGIN
    raw_xml := @extschema@.s3_list_raw(prefix, continuation_token, endpoint, region, access_key, secret_key, bucket);
    
    RETURN QUERY
    SELECT 
        (xpath('//s3:Key/text()', item, ns))[1]::text AS key,
        (xpath('//s3:LastModified/text()', item, ns))[1]::text::timestamp AS last_modified,
        REPLACE((xpath('//s3:ETag/text()', item, ns))[1]::text, '"', '') AS etag,
        (xpath('//s3:Size/text()', item, ns))[1]::text::bigint AS size,
        (xpath('//s3:StorageClass/text()', item, ns))[1]::text AS storage_class,
        (xpath('//s3:IsTruncated/text()', raw_xml::xml, ns))[1]::text::boolean AS is_truncated,
        (xpath('//s3:NextContinuationToken/text()', raw_xml::xml, ns))[1]::text AS next_continuation_token
    FROM (
        SELECT unnest(xpath('//s3:Contents', raw_xml::xml, ns)) AS item
    ) t;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION s3_copy(
    source_key text,
    target_key text,
    source_bucket text DEFAULT NULL,
    target_bucket text DEFAULT NULL,
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
) RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_copy'
LANGUAGE C STRICT;

CREATE FUNCTION s3_export(
    query text,
    object_key text,
    format text DEFAULT 'csv',
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
) RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_s3_export'
LANGUAGE C STRICT;

CREATE FUNCTION s3_read(
    object_key text,
    format text,
    endpoint text DEFAULT NULL,
    region text DEFAULT NULL,
    access_key text DEFAULT NULL,
    secret_key text DEFAULT NULL,
    bucket text DEFAULT NULL
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_s3_read'
LANGUAGE C;
