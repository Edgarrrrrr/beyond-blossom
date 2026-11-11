#include <immintrin.h>
__attribute__((target("avx2,no-popcnt")))
__m128i active_batch4(int a, int b, int c, int d) {
    return _mm_setr_epi32(a, b, c, d);
}
