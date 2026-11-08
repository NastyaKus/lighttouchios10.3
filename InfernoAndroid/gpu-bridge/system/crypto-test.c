/* SPDX-License-Identifier: GPL-3.0-or-later
 * The builtin SHA-256/HMAC used by the Android EGP2 client must equal OpenSSL. */
#include "crypto.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    uint8_t data[1200], key[32], a[32], b[32];
    srand(7);
    for (unsigned n = 0; n <= sizeof(data); n += (n < 300 ? 1 : 97)) {
        for (unsigned i = 0; i < n; i++)
            data[i] = (uint8_t)rand();
        for (unsigned i = 0; i < 32; i++)
            key[i] = (uint8_t)rand();
        egp2_builtin_sha256(data, n, a);
        SHA256(data, n, b);
        if (memcmp(a, b, 32))
            return printf("FAIL sha256 n=%u\n", n), 1;
        unsigned got = 32;
        egp2_builtin_hmac(key, data, n, a);
        HMAC(EVP_sha256(), key, 32, data, n, b, &got);
        if (memcmp(a, b, 32))
            return printf("FAIL hmac n=%u\n", n), 2;
    }
    puts("PASS builtin SHA-256/HMAC equal OpenSSL");
    return 0;
}
