/*
 * s3_parquet.cpp
 * Apache Arrow / Parquet native C++ integration.
 *
 * dev notes:
 * - This file takes PostgreSQL SPI Tuple Tables and maps them directly into
 *   Apache Arrow string builders in memory.
 * - This requires `-std=c++20` to compile because Arrow v24 relies on it.
 * - The buffer is returned via `palloc` so PostgreSQL can garbage collect it.
 */
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <arrow/csv/api.h>
#include <arrow/json/api.h>
#include <parquet/arrow/reader.h>
#include <arrow/io/memory.h>
#include <arrow/io/file.h>
#include <vector>
#include <memory>
#include <string>

extern "C" {
#include "postgres.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "access/htup_details.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
}


#include "s3_parquet.h"

extern "C" char* build_parquet_from_spi(void *tuptable_ptr, int processed, size_t *out_len) {
    SPITupleTable *tuptable = (SPITupleTable*)tuptable_ptr;
    TupleDesc tupdesc = tuptable->tupdesc;
    
    arrow::FieldVector fields;
    
    // For universal compatibility, we map all PostgreSQL columns to Arrow UTF8 (strings)
    // because SPI_getvalue perfectly stringifies UUIDs, JSONB, Timestamps, etc.
    for (int j = 1; j <= tupdesc->natts; j++) {
        std::string col_name = NameStr(tupdesc->attrs[j - 1].attname);
        fields.push_back(arrow::field(col_name, arrow::utf8()));
    }
    
    auto schema = std::make_shared<arrow::Schema>(fields);
    
    // Create Builders
    std::vector<std::unique_ptr<arrow::StringBuilder>> builders;
    for (int j = 1; j <= tupdesc->natts; j++) {
        builders.push_back(std::make_unique<arrow::StringBuilder>());
    }
    
    // Populate Builders by iterating over PostgreSQL tuples
    for (int i = 0; i < processed; i++) {
        HeapTuple tuple = tuptable->vals[i];
        for (int j = 1; j <= tupdesc->natts; j++) {
            char *val = SPI_getvalue(tuple, tupdesc, j);
            if (val) {
                (void)builders[j-1]->Append(val);
                pfree(val); // Clean up PG memory
            } else {
                (void)builders[j-1]->AppendNull();
            }
        }
    }
    
    // Finalize arrays
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (int j = 1; j <= tupdesc->natts; j++) {
        std::shared_ptr<arrow::Array> arr;
        (void)builders[j-1]->Finish(&arr);
        arrays.push_back(arr);
    }
    
    auto table = arrow::Table::Make(schema, arrays);
    
    // Write Parquet to an in-memory buffer
    auto buffer_stream = arrow::io::BufferOutputStream::Create().ValueOrDie();
    
    // Enable state-of-the-art ZSTD compression natively!
    std::shared_ptr<parquet::WriterProperties> props = 
        parquet::WriterProperties::Builder().compression(arrow::Compression::ZSTD)->build();
    
    (void)parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), buffer_stream, 1024 * 1024, props);
    
    auto buffer = buffer_stream->Finish().ValueOrDie();
    
    // Copy Arrow memory to PostgreSQL memory context
    *out_len = buffer->size();
    char *ret = (char*)palloc(*out_len);
    memcpy(ret, buffer->data(), *out_len);
    
    return ret;
}

extern "C" void build_parquet_from_spi_to_file(void *tuptable_ptr, int processed, const char *filepath) {
    SPITupleTable *tuptable = (SPITupleTable*)tuptable_ptr;
    TupleDesc tupdesc = tuptable->tupdesc;
    
    arrow::FieldVector fields;
    for (int j = 1; j <= tupdesc->natts; j++) {
        std::string col_name = NameStr(tupdesc->attrs[j - 1].attname);
        fields.push_back(arrow::field(col_name, arrow::utf8()));
    }
    
    auto schema = std::make_shared<arrow::Schema>(fields);
    std::vector<std::unique_ptr<arrow::StringBuilder>> builders;
    for (int j = 1; j <= tupdesc->natts; j++) {
        builders.push_back(std::make_unique<arrow::StringBuilder>());
    }
    
    for (int i = 0; i < processed; i++) {
        HeapTuple tuple = tuptable->vals[i];
        for (int j = 1; j <= tupdesc->natts; j++) {
            char *val = SPI_getvalue(tuple, tupdesc, j);
            if (val) {
                (void)builders[j-1]->Append(val);
                pfree(val);
            } else {
                (void)builders[j-1]->AppendNull();
            }
        }
    }
    
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (int j = 1; j <= tupdesc->natts; j++) {
        std::shared_ptr<arrow::Array> arr;
        (void)builders[j-1]->Finish(&arr);
        arrays.push_back(arr);
    }
    
    auto table = arrow::Table::Make(schema, arrays);
    
    // Write Parquet directly to disk!
    auto file_stream_res = arrow::io::FileOutputStream::Open(filepath);
    if (!file_stream_res.ok()) {
        ereport(ERROR, (errmsg("Failed to open temporary file for Parquet spooling")));
    }
    auto file_stream = std::move(file_stream_res.ValueOrDie());
    
    std::shared_ptr<parquet::WriterProperties> props = 
        parquet::WriterProperties::Builder().compression(arrow::Compression::ZSTD)->build();
    
    (void)parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file_stream, 1024 * 1024, props);
    (void)file_stream->Close();
}

// Helper to push Arrow table rows into a Postgres tuplestore
void push_arrow_table_to_tuplestore(std::shared_ptr<arrow::Table> table, TupleDesc tupdesc, Tuplestorestate *tupstore) {
    int64_t num_rows = table->num_rows();
    int num_cols = tupdesc->natts;
    
    for (int64_t row = 0; row < num_rows; row++) {
        Datum *values = (Datum *) palloc0(num_cols * sizeof(Datum));
        bool *nulls = (bool *) palloc0(num_cols * sizeof(bool));
        
        for (int col = 0; col < num_cols; col++) {
            if (col >= table->num_columns()) {
                nulls[col] = true;
                continue;
            }
            
            auto column = table->column(col);
            auto result = column->GetScalar(row);
            if (!result.ok() || result.ValueOrDie()->is_valid == false) {
                nulls[col] = true;
            } else {
                std::string val_str = result.ValueOrDie()->ToString();
                
                // Fetch PostgreSQL Input Function for this specific column type (e.g. textin, int4in)
                Oid typid = tupdesc->attrs[col].atttypid;
                Oid typioparam;
                Oid typin;
                getTypeInputInfo(typid, &typin, &typioparam);
                
                char *cstr = pstrdup(val_str.c_str());
                values[col] = OidFunctionCall3(typin, CStringGetDatum(cstr), ObjectIdGetDatum(typioparam), Int32GetDatum(-1));
                nulls[col] = false;
                pfree(cstr); // Postgres cleans this up, but good practice
            }
        }
        
        HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
        tuplestore_puttuple(tupstore, tuple);
        
        heap_freetuple(tuple);
        pfree(values);
        pfree(nulls);
    }
}

extern "C" void build_tuplestore_from_arrow(const char *buffer, size_t length, const char *format_cstr, void *tupdesc_ptr, void *tupstore_ptr) {
    TupleDesc tupdesc = (TupleDesc) tupdesc_ptr;
    Tuplestorestate *tupstore = (Tuplestorestate *) tupstore_ptr;
    std::string format = format_cstr;
    
    auto buffer_obj = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>(buffer), static_cast<int64_t>(length));
    auto input = std::make_shared<arrow::io::BufferReader>(buffer_obj);
    std::shared_ptr<arrow::Table> table;
    
    if (format == "parquet") {
        auto reader_res = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
        if (!reader_res.ok()) {
            ereport(ERROR, (errmsg("Failed to open Parquet file natively via Arrow")));
        }
        auto reader = std::move(reader_res.ValueOrDie());
        
        auto table_res = reader->ReadTable();
        if (!table_res.ok()) {
            ereport(ERROR, (errmsg("Failed to read Parquet table")));
        }
        table = table_res.ValueOrDie();
    } else if (format == "csv") {
        auto read_options = arrow::csv::ReadOptions::Defaults();
        auto parse_options = arrow::csv::ParseOptions::Defaults();
        auto convert_options = arrow::csv::ConvertOptions::Defaults();
        
        auto maybe_reader = arrow::csv::TableReader::Make(arrow::io::default_io_context(), input, read_options, parse_options, convert_options);
        if (!maybe_reader.ok()) ereport(ERROR, (errmsg("Failed to initialize Arrow CSV Reader")));
        
        auto maybe_table = maybe_reader.ValueOrDie()->Read();
        if (!maybe_table.ok()) ereport(ERROR, (errmsg("Failed to parse CSV natively via Arrow")));
        table = maybe_table.ValueOrDie();
    } else if (format == "json") {
        auto read_options = arrow::json::ReadOptions::Defaults();
        auto parse_options = arrow::json::ParseOptions::Defaults();
        
        auto maybe_reader = arrow::json::TableReader::Make(arrow::default_memory_pool(), input, read_options, parse_options);
        if (!maybe_reader.ok()) ereport(ERROR, (errmsg("Failed to initialize Arrow JSON Reader")));
        
        auto maybe_table = maybe_reader.ValueOrDie()->Read();
        if (!maybe_table.ok()) ereport(ERROR, (errmsg("Failed to parse JSON natively via Arrow")));
        table = maybe_table.ValueOrDie();
    } else {
        ereport(ERROR, (errmsg("Unsupported format for s3_read. Use 'parquet', 'csv', or 'json'.")));
    }
    
    push_arrow_table_to_tuplestore(table, tupdesc, tupstore);
}

extern "C" void build_tuplestore_from_arrow_file(const char *filepath, const char *format_cstr, void *tupdesc_ptr, void *tupstore_ptr) {
    TupleDesc tupdesc = (TupleDesc) tupdesc_ptr;
    Tuplestorestate *tupstore = (Tuplestorestate *) tupstore_ptr;
    std::string format = format_cstr;
    
    auto file_res = arrow::io::MemoryMappedFile::Open(filepath, arrow::io::FileMode::READ);
    if (!file_res.ok()) {
        ereport(ERROR, (errmsg("Failed to memory-map temporary file for reading")));
    }
    auto input = std::move(file_res.ValueOrDie());
    std::shared_ptr<arrow::Table> table;
    
    if (format == "parquet") {
        auto reader_res = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
        if (!reader_res.ok()) ereport(ERROR, (errmsg("Failed to open Parquet file natively via Arrow")));
        auto reader = std::move(reader_res.ValueOrDie());
        
        auto table_res = reader->ReadTable();
        if (!table_res.ok()) ereport(ERROR, (errmsg("Failed to read Parquet table")));
        table = table_res.ValueOrDie();
    } else if (format == "csv") {
        auto maybe_reader = arrow::csv::TableReader::Make(arrow::io::default_io_context(), input, arrow::csv::ReadOptions::Defaults(), arrow::csv::ParseOptions::Defaults(), arrow::csv::ConvertOptions::Defaults());
        if (!maybe_reader.ok()) ereport(ERROR, (errmsg("Failed to initialize Arrow CSV Reader")));
        auto maybe_table = maybe_reader.ValueOrDie()->Read();
        if (!maybe_table.ok()) ereport(ERROR, (errmsg("Failed to parse CSV natively via Arrow")));
        table = maybe_table.ValueOrDie();
    } else if (format == "json") {
        auto maybe_reader = arrow::json::TableReader::Make(arrow::default_memory_pool(), input, arrow::json::ReadOptions::Defaults(), arrow::json::ParseOptions::Defaults());
        if (!maybe_reader.ok()) ereport(ERROR, (errmsg("Failed to initialize Arrow JSON Reader")));
        auto maybe_table = maybe_reader.ValueOrDie()->Read();
        if (!maybe_table.ok()) ereport(ERROR, (errmsg("Failed to parse JSON natively via Arrow")));
        table = maybe_table.ValueOrDie();
    } else {
        ereport(ERROR, (errmsg("Unsupported format for s3_read. Use 'parquet', 'csv', or 'json'.")));
    }
    
    push_arrow_table_to_tuplestore(table, tupdesc, tupstore);
}
