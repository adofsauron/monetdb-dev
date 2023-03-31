#!/bin/bash

monetdb create mytest

monetdb status

netstat -anp|grep 50000

monetdb release mytest

monetdb status

