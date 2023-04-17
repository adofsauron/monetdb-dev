#!/bin/bash

cd ../

cd tpch-scripts

bash  ./tpch_build.sh -s 10 -f  /dbfarm

bash  ./tpch_build.sh -s 0.001 -f  /dbfarm

