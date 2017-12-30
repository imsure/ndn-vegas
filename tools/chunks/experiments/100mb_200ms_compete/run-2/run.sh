#!/bin/bash

ndncatchunks -f --vegas-alpha 5 --vegas-beta 10 --cc-debug-stats . -S -t vegas /lan/100mb > /dev/null &
ndncatchunks -f --cc-debug-stats . -S -t aimd /lan/100mb > /dev/null &
ndncatchunks -f --cc-debug-stats . -S -t cubic /lan/100mb > /dev/null &
