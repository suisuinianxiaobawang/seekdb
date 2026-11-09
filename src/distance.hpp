#pragma once
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

namespace mini {
inline double distance_tail(const char* data, const float* target, size_t size, size_t start,
                            double (&sums)[4]) {
    for (size_t i = start; i < size; ++i) {
        float value;
        std::memcpy(&value, data + i * sizeof value, sizeof value);
        double delta = static_cast<double>(value) - static_cast<double>(target[i]);
        sums[i % 4] += delta * delta;
    }
    return (sums[0] + sums[1]) + (sums[2] + sums[3]);
}

inline double scalar_distance(const char* data, const float* target, size_t size) {
    double sums[4]{};
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
        float values[4];
        std::memcpy(values, data + i * sizeof(float), sizeof values);
        for (size_t j = 0; j < 4; ++j) {
            double delta = static_cast<double>(values[j]) - static_cast<double>(target[i + j]);
            sums[j] += delta * delta;
        }
    }
    return distance_tail(data, target, size, i, sums);
}

#if defined(__aarch64__)
inline double neon_distance(const char* data, const float* target, size_t size) {
    auto lo = vdupq_n_f64(0);
    auto hi = lo;
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
        float32x4_t values;
        std::memcpy(&values, data + i * sizeof(float), sizeof values);
        auto q = vld1q_f32(target + i);
        auto d0 = vsubq_f64(vcvt_f64_f32(vget_low_f32(values)), vcvt_f64_f32(vget_low_f32(q)));
        auto d1 = vsubq_f64(vcvt_high_f64_f32(values), vcvt_high_f64_f32(q));
        lo = vaddq_f64(lo, vmulq_f64(d0, d0));
        hi = vaddq_f64(hi, vmulq_f64(d1, d1));
    }
    double sums[4];
    vst1q_f64(sums, lo);
    vst1q_f64(sums + 2, hi);
    return distance_tail(data, target, size, i, sums);
}
#elif defined(__x86_64__)
__attribute__((target("avx"))) inline double avx_distance(const char* data, const float* target, size_t size) {
    auto sum = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= size; i += 4) {
        __m128 values;
        std::memcpy(&values, data + i * sizeof(float), sizeof values);
        auto q = _mm_loadu_ps(target + i);
        auto delta = _mm256_sub_pd(_mm256_cvtps_pd(values), _mm256_cvtps_pd(q));
        sum = _mm256_add_pd(sum, _mm256_mul_pd(delta, delta));
    }
    double sums[4];
    _mm256_storeu_pd(sums, sum);
    return distance_tail(data, target, size, i, sums);
}
#endif

using DistanceFunction = double (*)(const char*, const float*, size_t);
inline DistanceFunction distance_function() {
#if defined(__aarch64__)
    return neon_distance;
#elif defined(__x86_64__)
    return __builtin_cpu_supports("avx") ? avx_distance : scalar_distance;
#else
    return scalar_distance;
#endif
}

inline double squared_distance(std::string_view bytes, const std::vector<float>& target) {
    static const auto calculate = distance_function();
    return calculate(bytes.data(), target.data(), target.size());
}
} // namespace mini
