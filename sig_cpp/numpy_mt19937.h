#pragma once

#include <cstdint>

namespace numpy_mt19937 {

constexpr int kStateLen = 624;
constexpr int kN = 624;
constexpr int kM = 397;
constexpr uint32_t kMatrixA = 0x9908b0dfUL;
constexpr uint32_t kUpperMask = 0x80000000UL;
constexpr uint32_t kLowerMask = 0x7fffffffUL;

struct State {
    uint32_t key[kStateLen]{};
    int pos = kStateLen;
};

inline void seed(State* state, uint32_t seed_value) {
    uint32_t s = seed_value & 0xffffffffUL;
    for (int pos = 0; pos < kStateLen; ++pos) {
        state->key[pos] = s;
        s = (1812433253UL * (s ^ (s >> 30)) + static_cast<uint32_t>(pos + 1)) & 0xffffffffUL;
    }
    state->pos = kStateLen;
}

inline void generate(State* state) {
    for (int i = 0; i < kN - kM; ++i) {
        const uint32_t y = (state->key[i] & kUpperMask) | (state->key[i + 1] & kLowerMask);
        state->key[i] = state->key[i + kM] ^ (y >> 1) ^ (-(y & 1U) & kMatrixA);
    }
    for (int i = kN - kM; i < kN - 1; ++i) {
        const uint32_t y = (state->key[i] & kUpperMask) | (state->key[i + 1] & kLowerMask);
        state->key[i] = state->key[i + (kM - kN)] ^ (y >> 1) ^ (-(y & 1U) & kMatrixA);
    }
    const uint32_t y = (state->key[kN - 1] & kUpperMask) | (state->key[0] & kLowerMask);
    state->key[kN - 1] = state->key[kM - 1] ^ (y >> 1) ^ (-(y & 1U) & kMatrixA);
    state->pos = 0;
}

inline uint32_t next_uint32(State* state) {
    if (state->pos == kStateLen) {
        generate(state);
    }
    uint32_t y = state->key[state->pos++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y;
}

inline uint32_t randint_bounded(State* state, uint32_t high_exclusive) {
    if (high_exclusive == 0) {
        return 0;
    }
    uint32_t limit = high_exclusive - 1;
    uint32_t mask = limit;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    while (true) {
        const uint32_t val = next_uint32(state) & mask;
        if (val <= limit) {
            return val;
        }
    }
}

class NumpyMtRng {
public:
    explicit NumpyMtRng(int seed_value) { seed(&state_, static_cast<uint32_t>(seed_value)); }

    int randint0(int high) {
        if (high <= 0) {
            return 0;
        }
        return static_cast<int>(randint_bounded(&state_, static_cast<uint32_t>(high)));
    }

private:
    State state_{};
};

}  // namespace numpy_mt19937
