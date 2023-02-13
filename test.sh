#!/bin/bash

OP_NUM=20000000

mutex_arr=("ABT_mutex" "Mutex" "pthread_mutex" "K42Lock" "TTASLock" "HTTAS")
# mutex_arr=("Mutex" "Mutex_EXP")

rwlock_arr=("ABT_rwlock" "RWLock" "pthread_rwlock")
# rwlock_arr=("RWLock" "pthread_rwlock")

cd build
ninja simple_bench
cd ..

for target in ${mutex_arr[*]}
do
    for thread_num in 4 8 12 16
    do
        ./build/simple_bench $target $OP_NUM 0 $thread_num 0
    done
    for thread_num in 1 2 3 4
    do
        ./build/simple_bench $target $OP_NUM 0 $thread_num 0 24 48 72
    done
    echo
done


for read_p in 0.7 0.9 0.99 1
do
    for target in ${rwlock_arr[*]}
    do
        for thread_num in 4 8 12 16
        do
            ./build/simple_bench $target $OP_NUM $read_p $thread_num 0
        done
        for thread_num in 1 2 3 4
        do
            ./build/simple_bench $target $OP_NUM $read_p $thread_num 0 24 48 72
        done
        echo
    done
done
