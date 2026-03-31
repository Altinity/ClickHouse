#include <stdlib.h>

/* glibc 2.32+ symbol used by libstdc++; sysroot may have older libc. */
__attribute__((weak))
char __libc_single_threaded = 0;

__attribute__((weak))
unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}

__attribute__((weak))
long long __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}
