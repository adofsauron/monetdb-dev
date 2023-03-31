#!/bin/bash


cat ./.monetdb > ~/.monetdb

rm /dbfarm  -rf

monetdbd create /dbfarm

monetdbd get all /dbfarm
