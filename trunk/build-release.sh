#!/bin/bash

cd monetdb-11.41.31

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Release ..
make -j`nproc`
make install

cd ../..
