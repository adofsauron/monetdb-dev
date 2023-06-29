#!/bin/bash

# https://github.com/MonetDBSolutions/tpch-scripts

cd ../3rd/tpch-scripts

chmod +x ./02_load/*.sh
chmod +x ./03_run/*.sh
chmod +x ./04_run/*.sh

bash  ./tpch_build.sh -s 10 -f  /dbfarm

bash  ./tpch_build.sh -s 0.001 -f  /dbfarm

cd -
