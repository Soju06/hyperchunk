#include <stdio.h>

#include "hyperchunk.h"

int main(void) {
    printf("hyperchunk %s (ABI %d)\n", hc_version(), hc_abi_version());
    return 0;
}
