// C reference for examples/fib.hoshi.
#include <stdio.h>

long long fib(long long n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    for (int i = 0; i < 15; i++)
        printf("%lld ", fib(i));
    printf("\nfib(40) = %lld\n", fib(40));
    return 0;
}
