#!/bin/bash

cd monetdb-jul2021_29

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Release ..
make -j`nproc`
make install

cd ../..
