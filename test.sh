#!/bin/bash
for ii in `ls test`
	do echo -n $ii': '
	OUTPUT=$(NODE_PATH=`pwd` node test/$ii 2>&1)
	echo "$OUTPUT"
	if [[ "$OUTPUT" != "pass" ]]; then
		exit 1
	fi
done
