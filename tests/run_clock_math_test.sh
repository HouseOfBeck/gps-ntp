#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/gps_ntp_clock_math_test"

cc -std=c11 -Wall -Wextra -Werror \
    -I"$project_dir/main" \
    "$project_dir/tests/clock_math_test.c" \
    "$project_dir/main/clock_math.c" \
    -o "$test_binary"

"$test_binary"

