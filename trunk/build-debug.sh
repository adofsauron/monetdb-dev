#!/bin/bash

cd monetdb-jul2021_29

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Debug ..
make -j`nproc`

cd ../..
