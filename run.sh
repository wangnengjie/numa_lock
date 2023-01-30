#!/bin/bash

OP_NUM=20000000

echo "" > res

for i in 2 4 8 16
do
    # for mu in "Mutex" "ABT_mutex" "std::mutex" "std::shared_mutex"
    for mu in "std::shared_mutex"
    do
        echo "./build/simple_bench $mu $OP_NUM $i 40" >> res
        ./build/simple_bench $mu $OP_NUM $i 40 >> res
    done
done

for i in 1 2 4 8
do
    # for mu in "Mutex" "ABT_mutex" "std::mutex" "std::shared_mutex"
    for mu in "std::shared_mutex"
    do
        echo "./build/simple_bench $mu $OP_NUM $i 0 20" >> res
        ./build/simple_bench $mu $OP_NUM $i 0 20 >> res
    done
done
