#!/bin/bash

mkdir -p /dbfarm/logs

# monetdbd set logfile=/dbfarm/logs/monetdb.log  /dbfarm
# monetdbd start /dbfarm 

nohup /usr/local/bin/mserver5 \
    --dbpath=/dbfarm/SF-10 \
    --dbtrace=/dbfarm/logs \
    --config=/root/.monetdb \
    --set merovingian_uri=mapi:monetdb://localhost.localdomain:50000/SF-10 \
    --set mapi_listenaddr=localhost \
    --set mapi_usock=/dbfarm/SF-10/.mapi.sock \
    --set monet_vault_key=/dbfarm/SF-10/.vaultkey \
    --set gdk_nr_threads=32 \
    --set max_clients=64 \
    --set sql_optimizer=default_pipe \
    --debug=2097152 \
    >> /dbfarm/logs/monetdb.log &



