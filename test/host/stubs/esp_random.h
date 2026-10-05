#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline uint32_t esp_random() { return (uint32_t)rand() * 2654435761u + 1; }
static inline void esp_fill_random(void *buf, size_t len) { for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = rand(); }
