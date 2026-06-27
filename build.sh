#!/bin/bash
set -e

echo "make cleaning ..."
make clean
rm -rf src/*.so src/*.bc *.bc *.so

echo "make and installing ..."
make
# Using sudo to install as it usually requires root permissions to write to postgres extension directory
# make CC=gcc VERBOSE=1
sudo make install

echo ">>>>>> restarting pg..."
sudo systemctl restart postgresql || true

echo "build and install complete."
echo "You can now run 'CREATE EXTENSION pg_s3;' in your PostgreSQL database."
