#!/bin/bash

mkdir -p /dbfarm/logs

touch /dbfarm/logs/trace.log

# https://www.monetdb.org/documentation-Jan2022/admin-guide/manpages/monetdbd/

# monetdbd set logfile=/dbfarm/logs/monetdb.log  /dbfarm
# monetdbd start /dbfarm 

# https://www.monetdb.org/documentation-Jan2022/admin-guide/manpages/mserver5/

nohup /usr/local/bin/mserver5 \
    --dbpath=/dbfarm/SF-0_1 \
    --dbtrace=/dbfarm/logs/trace.log \
    --config=/root/.monetdb \
    --set merovingian_uri=mapi:monetdb://localhost.localdomain:50000/SF-0_1 \
    --set mapi_listenaddr=localhost \
    --set mapi_usock=/dbfarm/SF-0_1/.mapi.sock \
    --set monet_vault_key=/dbfarm/SF-0_1/.vaultkey \
    --set gdk_nr_threads=32 \
    --set max_clients=64 \
    --set sql_optimizer=default_pipe \
    --debug=2101392 \
    >> /dbfarm/logs/monetdb.log &


