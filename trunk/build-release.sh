#!/bin/bash

cd monetdb-sep2022_sp3_release

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Release ..
make -j`nproc`
make install

cd ../..
