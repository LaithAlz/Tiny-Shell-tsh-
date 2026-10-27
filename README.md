# tsh — a tiny UNIX shell

A job-control shell in C. Parses a command line, runs builtins in the shell
process and external programs via `fork`/`execve`, and manages foreground and
background jobs the way a real shell does.

## What it handles

**Job control.** `jobs`, `bg`, `fg`, addressed by either PID or `%jobid`. One
foreground job at a time, any number of background jobs.

**Signals, without races.** `SIGCHLD`, `SIGINT` and `SIGTSTP` are blocked with
`sigprocmask` before `fork`, so the parent cannot be interrupted by a child that
exits before the job is added to the job list. The child unblocks them, calls
`setpgid(0, 0)` to leave the shell's process group, and resets the handlers to
`SIG_DFL`.

That `setpgid` is the reason Ctrl-C and Ctrl-Z reach the foreground job and not
the background ones: the kernel delivers terminal signals to the foreground
process group only.

**Waiting without spinning.** `waitfg` blocks in `sigsuspend` rather than
polling, so an idle shell uses no CPU. `sigchld_handler` reaps every available
child with `waitpid(-1, &status, WNOHANG | WUNTRACED)` and distinguishes exited,
signalled and stopped children.

## Build and run

```bash
gcc -Wall -O2 -o tsh tsh.c
./tsh
```

```
tsh> /bin/sleep 30 &
tsh> jobs
[1] (12345) Running /bin/sleep 30 &
tsh> fg %1
```

Written for a systems programming course at the University of Toronto.
