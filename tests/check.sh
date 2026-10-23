#!/bin/bash
# Compares the mini shell with bash on a few commands.
# Usage:  make  &&  ./tests/check.sh

SHELL_BIN="$(cd "$(dirname "$0")/.." && pwd)/myshell"
M=$(mktemp -d)   # folder for the mini shell
B=$(mktemp -d)   # folder for bash

# both folders start with the same files
for d in "$M" "$B"; do
    printf 'hello\nfile one\n' > "$d/input.txt"
    touch "$d/file1" "$d/file2" "$d/abc.cc" "$d/myshell.log"
    cp "$SHELL_BIN" "$d/"
done

# run the commands with bash
cd "$B"
ls > o1
cat < input.txt > o2
echo "Hello, World!" | grep "Hello" > o3
ls | grep file | grep 1 > o4
ls nofile 2> o5
ls nofile >> o6 2>&1
echo *.txt > o7

# run the same commands with the mini shell
cd "$M"
./myshell > /dev/null 2>&1 << 'COMMANDS'
ls > o1
cat < input.txt > o2
echo "Hello, World!" | grep "Hello" > o3
ls | grep file | grep 1 > o4
ls nofile 2> o5
ls nofile >>& o6
echo *.txt > o7
exit
COMMANDS

# compare the result files
fail=0
for f in o1 o2 o3 o4 o5 o6 o7; do
    if cmp -s "$M/$f" "$B/$f"; then
        echo "$f OK"
    else
        echo "$f FAIL"
        fail=1
    fi
done

rm -rf "$M" "$B"
exit $fail
