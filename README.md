# lab3-minishell

A small Unix shell written in C++ (Operating Systems, Lab 3).

## Build and run

    make
    ./myshell

## What it supports

- Simple commands with arguments: `ls -al`
- Redirection: `>`  `>>`  `<`  `2>`  `>>&`
- Pipes: `ls | grep file | wc`
- Background commands: `sleep 5 &`
- Quotes: `echo "Hello, World!"`
- Wildcards: `echo *.cc`, `echo ?akefile`
- Built-in commands: `cd [dir]` and `exit`
- Ctrl-C does not close the shell
- A log file (`myshell.log`) gets a line every time a child process ends (SIGCHLD)
- A clear message for wrong input (for example `ls |` or `ls > a > b`)

The shell prints the command table before it runs each command.

## Files

- `command.cc`: parsing and execution (all the work is here)
- `command.h`, `tokenizer.cc`, `tokenizer.h`: base code given with the lab
- `Makefile`: builds `myshell`
- `tests/check.sh`: runs some commands in the shell and in bash, and compares the results

## Known limits

- Put spaces around `|`, `<`, `>` and `&` (the given tokenizer splits on spaces only).
- Redirection in the middle of a pipe is an error: `<` is only for the first command and `>` is only for the last one.
