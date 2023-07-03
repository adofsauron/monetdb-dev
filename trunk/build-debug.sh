#!/bin/bash

cd monetdb-11.41.31

mkdir -p build
cd build
cmake  -DCMAKE_BUILD_TYPE=Debug -DASSERT=ON -DSTRICT=ON --enable-gdk --enable-monetdb5 --enable-rdf --enable-datacell --enable-fits --enable-sql --enable-geom --enable-jaql --enable-gsl --enable-odbc --enable-testing --enable-console --enable-jdbc ..
make -j`nproc`
make install

cd ../..
