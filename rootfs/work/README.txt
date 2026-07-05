Orthox-64 persistent workspace

Use /work for files you want to keep across reboots when booted with:
make persist-run

Typical native build loop:
  kilo hello.c
  cd /work
  cc hello.c -o hello
  ./hello

Run `sync` in the shell before rebooting to flush xv6fs changes.
