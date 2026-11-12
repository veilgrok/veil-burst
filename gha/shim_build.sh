#!/bin/bash
# Build gha/bin/dsm_shim.so for the QEMU-gvm chroot (glibc 2.27) on a newer host: link against the chroot's own libc so every
# symbol version exists there. Newer glibc headers redirect sscanf/strtol to __isoc23_* names that 2.27 lacks; two wrappers cover that.
set -e; cd "$(dirname "$0")"; T=$(mktemp -d); tar xzf bin/gvm/l2root.tar.gz -C $T --wildcards 'lib/x86_64-linux-gnu/libc[.-]*' 'lib/x86_64-linux-gnu/libpthread*' 'lib/x86_64-linux-gnu/libdl*'
cat > $T/compat.c <<'C'
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#undef sscanf
#undef strtol
int __isoc23_sscanf(const char *s, const char *f, ...) { va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }
extern long __strtol_internal(const char *, char **, int, int);
long __isoc23_strtol(const char *s, char **e, int b) { return __strtol_internal(s, e, b, 0); }
C
L=$T/lib/x86_64-linux-gnu
gcc -O2 -fPIC -c -o $T/compat.o $T/compat.c
gcc -O2 -shared -fPIC -o bin/dsm_shim.so dsm_shim.c $T/compat.o -nodefaultlibs -L$L -l:libpthread.so.0 -l:libdl.so.2 -l:libc.so.6 -lgcc
max=$(objdump -T bin/dsm_shim.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1); echo "dsm_shim.so built; newest symbol version needed: $max"
objdump -T bin/dsm_shim.so | grep -q "UND.*__isoc23" && { echo "unresolved __isoc23 symbol"; exit 1; }; rm -rf $T
