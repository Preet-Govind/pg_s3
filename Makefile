EXTENSION = pg_s3
DATA = sql/pg_s3--1.0.sql
MODULE_big = pg_s3
OBJS = src/pg_s3.o src/s3_api.o src/s3_auth.o src/s3_http.o src/s3_parquet.o
SHLIB_LINK += -lcurl -lcrypto -lparquet -larrow
CXX = g++ -std=c++20

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

src/s3_parquet.o: src/s3_parquet.cpp
	g++ -std=c++20 $(CXXFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<

src/s3_parquet.bc: src/s3_parquet.cpp
	clang++-19 -std=c++20 $(BITCODE_CXXFLAGS) $(CPPFLAGS) -fPIC -emit-llvm -c -o $@ $<

