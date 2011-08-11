#!/bin/bash
set -e
for ii in `ls test`
	do echo -n $ii': '
	OUTPUT=$(NODE_PATH=`pwd` ./bin/node-fibers test/$ii 2>&1)
	echo $OUTPUT
	if [[ "$OUTPUT" != "pass" ]]; then
		exit 1
	fi
done
