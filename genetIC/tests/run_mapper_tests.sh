#!/usr/bin/env bash

for i in mapper_test_*
do
    rm $i/map_out.txt 2>/dev/null
    echo "Running test on $i"
    head -1 $i/paramfile.txt
    cd $i
    time ../../IC paramfile.txt > IC_output.txt 2>&1
    if [ $? -ne 0 ]
    then
        echo "TEST FAILED"
        cat IC_output.txt
        exit
    fi
    cd ..
    echo "Testing output"
    DIFF_RESULT=$(diff $i/reference.txt $i/output.txt)
    if [ "$DIFF_RESULT" != "" ]
    then
        echo $DIFF_RESULT
        echo
        echo "--> TEST FAILED"
        echo
        echo "The IC generator output follows"
        echo
        cat $i/IC_output.txt
        exit 1
    fi
done

echo "Tests seem OK"
