#ifndef S3_PARQUET_H
#define S3_PARQUET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Takes SPI_tuptable and SPI_processed, returns a palloc'd Parquet buffer, sets out_len
char* build_parquet_from_spi(void *tuptable, int processed, size_t *out_len);

// Writes SPI_tuptable to a Parquet file on disk
void build_parquet_from_spi_to_file(void *tuptable, int processed, const char *filepath);

// Parses CSV/JSON/Parquet from an S3 memory buffer into a PostgreSQL Tuplestore
void build_tuplestore_from_arrow(const char *buffer, size_t length, const char *format, void *tupdesc_ptr, void *tupstore_ptr);

// Parses CSV/JSON/Parquet from a file on disk into a PostgreSQL Tuplestore
void build_tuplestore_from_arrow_file(const char *filepath, const char *format, void *tupdesc_ptr, void *tupstore_ptr);

#ifdef __cplusplus
}
#endif

#endif
