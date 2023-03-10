# Shell

A simple implementation of a shell in C with support for:
- Some builtins (`cd`, `exit`, `bg`, `fg`, `kill`, `jobs`).
- Commands with space separated arguments.
- Running commands with relative/absolute paths (or searching `/usr/bin` and `/bin` in that order for the command).
- Running commands in the background (using `command [args] &`).
- Job control (`jobs`, `fg`, `bg`, `kill`ing jobs designated by their job id).
- Signal handling (`Ctrl-C`, `Ctrl-Z` work as expected).
- Passing of terminal control (e.g interactive terminal applications such as Vim/Nano work as expected).
- Basic error handling for unexpected situations (e.g command not found, jobs terminated by signals).

## Building and Running

This project comes with a `Makefile`. To build and run:

```bash
$ make
$ ./shell
```

`make clean` is also provided to delete the executable file.

## Demo

```bash
$ ./shell
> ls
Makefile        README.md       shell           shell.c
> cd ..
> pwd
/Users/me/workspace/cpp/c
> lss
lss: command not found
> sleep 5
^C[1] 14384 terminated by signal 2
> sleep 10
^Z> jobs
[1] 14385 Stopped sleep 10 
> kill %1
> jobs
[1] 14385 terminated by signal 15
> exit

$
```
