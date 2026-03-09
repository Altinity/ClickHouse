#include <stdlib.h>

unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}

long long __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}
