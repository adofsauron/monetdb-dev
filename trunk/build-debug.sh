#!/bin/bash

cd monetdb-sep2022_sp3_release

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Debug -DASSERT=ON -DSTRICT=ON ..
make -j`nproc`
make install

cd ../..
