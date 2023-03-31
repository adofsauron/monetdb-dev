#!/usr/bin/env bash

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0.  If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Copyright 2017-2018 MonetDB Solutions B.V.

usage() {
    echo "Usage: $0 --db <db> [--number <repeats>] [--tag <tag>] [--output <file>]"
    echo "Run the TPC-H queries a number of times and report timings."
    echo ""
    echo "Options:"
    echo "  -d, --db <db>                     The database"
    echo "  -n, --number <repeats>            How many times to run the queries. Default=1"
    echo "  -t, --tag <tag>                   An arbitrary string to distinguish this"
    echo "                                    run from others in the same results CSV."
    echo "  -o, --output <file>               Where to append the output. Default=timings.csv"
    echo "  -p, --port <port>                 Port number where the server is listening"
    echo "  -m, --optimizer <optimizer>       The optimizer pipeline to use"
    echo "  -v, --verbose                     More output"
    echo "  -h, --help                        This message"
}

dbname=
nruns=1
port=50000
tag="default"
pipeline="default_pipe"

while [ "$#" -gt 0 ]
do
    case "$1" in
        -d|--db)
            dbname=$2
            shift
            shift
            ;;
        -n|--number)
            nruns=$2
            shift
            shift
            ;;
        -t|--tag)
            tag=$2
            shift
            shift
            ;;
        -o|--output)
            output=$2
            shift
            shift
            ;;
        -p|--port)
            port=$2
            shift
            shift
            ;;
        -m|--optimizer)
            pipeline=$2
            shift
            shift
            ;;
        -v|--verbose)
            set -x
            set -v
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "$0: unknown argument $1"
            usage
            exit 1
            ;;
    esac
done

if [ -z "$dbname" ]; then
    usage
    exit 1
fi

output="$dbname.timings.csv"
optimizer="set optimizer='$pipeline';"
TIMEFORMAT="%R"

#today=$(date +%Y-%m-%d)
#dir=results/"$today_$dbname_$tag"
#mkdir -p "$dir"

#echo "### Free memory before the run :###" | tee -a "$output"
#echo "$(free -h)" | tee -a "$output"

echo "# Database,Tag,Query,Min,Max,Average" | tee -a "$output"
for i in $(ls ??.sql)
do
    echo "$optimizer" > "/tmp/$i"
    cat "$i" >> "/tmp/$i"

    # iostat -t -m > /tmp/iostat
    # avg=0


    max=0
    min=9999999
    sum=0
    
    for j in $(seq 1 $nruns)
    do
        s=$(date +%s.%N)
        mclient -d "$dbname" -p "$port" -f raw -w 80 -i < "/tmp/$i" 2>&1 >/dev/null
        x=$(date +%s.%N)
	elapsed=$(echo "scale=4; $x - $s" | bc)
	
	if [ $(echo "$elapsed > $max" | bc) -eq 1 ] 
	then
	    max=$elapsed
	fi

	if [ $(echo "$elapsed < $min" | bc) -eq 1 ]
        then
            min=$elapsed
        fi

	sum=$(echo "$elapsed + $sum" | bc)	
	
	#echo "$(free -h)" | tee -a "$output"
	#mclient -d "$dbname" -p "$port" -s "select * from sys.bbp () as deb_data;" | tee -a "$output"

    done

    avg=$(echo "scale=4; $sum/$nruns" | bc)

    echo "$dbname,$tag,"$(basename $i .sql)",$min,$max,$avg" | tee -a "$output"

done


