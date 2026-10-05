// Host stand-in for rweather's RNG (only referenced by Curve25519::dh1, which the firmware never calls).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
class RNGClass
{
  public:
    void rand(uint8_t *data, size_t len)
    {
        for (size_t i = 0; i < len; i++)
            data[i] = ::rand();
    }
};
extern RNGClass RNG;
