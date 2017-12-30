#!/bin/bash

ndncatchunks -f --cc-debug-stats . -S -t cubic /lan/100mb > /dev/null &
sleep 1
ndncatchunks -f --vegas-alpha 10 --vegas-beta 20 --cc-debug-stats . -S -t vegas /lan/100mb > /dev/null &
ndncatchunks -f --cc-debug-stats . -S -t aimd /lan/100mb > /dev/null &

