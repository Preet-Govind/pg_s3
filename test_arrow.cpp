#include <arrow/api.h>
#include <arrow/csv/api.h>
#include <arrow/json/api.h>
#include <parquet/arrow/reader.h>
#include <arrow/io/memory.h>
#include <iostream>

int main() {
    auto field = arrow::field("A", arrow::int32());
    auto schema = arrow::schema({field});
    std::shared_ptr<arrow::Table> table = arrow::Table::MakeEmpty(schema).ValueOrDie();
    auto scalar_res = table->column(0)->GetScalar(0);
    if (scalar_res.ok()) {
        std::cout << scalar_res.ValueOrDie()->ToString() << std::endl;
    }
    return 0;
}
