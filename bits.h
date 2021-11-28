uint8_t bit_set(uint8_t source, uint8_t bit) {
    return (uint8_t) (source | (1 << bit));
}

bool bit_check(uint8_t source, uint8_t bit) {
    return (source & (1 << bit)) != 0;
}

uint8_t bit_clear(uint8_t source, uint8_t bit) {
    return (uint8_t) (source & ~(1 << bit));
}

