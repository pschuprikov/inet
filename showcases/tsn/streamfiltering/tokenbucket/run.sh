#!/bin/sh

inet_dbg -s -u Cmdenv -c SingleRateTwoColorMeter --sim-time-limit=1s --cmdenv-redirect-output=true &
inet_dbg -s -u Cmdenv -c DualRateThreeColorMeter --sim-time-limit=1s --cmdenv-redirect-output=true