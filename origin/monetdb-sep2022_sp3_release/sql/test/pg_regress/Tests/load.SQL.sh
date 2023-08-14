#!/usr/bin/env bash

$SQL_CLIENT < $TSTSRCDIR/create_table.sql

# don't load stud_emp, as it's the only data file with \N entries
# (PostgreSQL NULL notation)
sed \
	-e "s+@abs_srcdir@+$TSTSRCBASE/$TSTDIR+g" \
	-e '/@abs_builddir@/d' \
	-e '/DELETE FROM/d' \
	-e "s/[Cc][Oo][Pp][Yy] \\(.*\\);/COPY INTO \\1 USING DELIMITERS '\\\\t', '\\\\n';/" \
	-e '/stud_emp/d' \
	$TSTSRCDIR/../input/copy.source \
	| $SQL_CLIENT

$SQL_CLIENT -s 'select count(*) from aggtest;'
