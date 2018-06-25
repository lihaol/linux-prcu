#!/bin/bash

dur=10
run=10
torture="rcuperf"
rest="1m"
path=`pwd`

for cpu in 4 8 16
do
       for type in PRCU TREE
       do
               folder="$path/res/rcuperf/cpu-$cpu/$type"
               if ! test -d $folder
               then
                       echo "$folder does not exist..."
                       exit
               fi

               echo "Running rcuperf-$type-${cpu}cpus..."
               `./kvm.sh --torture $torture --duration $dur --configs ${run}*${type}-${cpu} --results $folder &> $folder/$type-cpu${cpu}-${dur}min.out`

               echo "Sleep $rest..."
               `sleep $rest`
       done
done
