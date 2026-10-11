#include "../numpy_mt19937.h"

#include <cstdio>

int main() {
    numpy_mt19937::NumpyMtRng rng(0);
    const int pool_sizes[] = {375066, 587991, 1000, 500, 100, 50, 10, 5, 2};
    for (int pool_n : pool_sizes) {
        std::printf("randint0(%d) = %d\n", pool_n, rng.randint0(pool_n));
    }
    return 0;
}
