#!/bin/sh
# Shows, live, the power events xe-pblock sees (never locks). Touch the sensor a few times, wait, then press the
# button once: only the press may print ">>> PRESS".
#   sh tools/xe-pblock/probe.sh [seconds]    default 60
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/../build/xe-pblock.app/Contents/MacOS/xe-pblock"
[ -x "$B" ] || sh "$HERE/build.sh"
exec "$B" --probe "${1:-60}"
