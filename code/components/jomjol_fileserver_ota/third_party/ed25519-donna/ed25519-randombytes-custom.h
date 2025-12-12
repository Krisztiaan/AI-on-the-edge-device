// Custom randombytes implementation for ESP32 (ed25519-donna).
#pragma once

#include <string.h>

#include "esp_random.h"

void ED25519_FN(ed25519_randombytes_unsafe)(void *p, size_t len)
{
    unsigned char *out = (unsigned char *)p;
    while (len > 0) {
        const uint32_t r = esp_random();
        const size_t n = len < sizeof(r) ? len : sizeof(r);
        memcpy(out, &r, n);
        out += n;
        len -= n;
    }
}
