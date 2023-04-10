#!/bin/bash

pid=`ps -ef | grep mserver5 | awk -F ' ' '{print $2}'`

echo pid=$pid

gdb -p $pid

