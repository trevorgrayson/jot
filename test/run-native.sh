#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
jot_test_binary=$(mktemp /tmp/jot-core-test.XXXXXX)
trap 'rm -f "$jot_test_binary"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Ilib/JotCore/src lib/JotCore/src/JotCore.cpp test/core_test.cpp -o "$jot_test_binary"
"$jot_test_binary"
