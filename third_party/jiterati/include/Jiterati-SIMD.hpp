/** @file Jiterati-SIMD.hpp
 *  @brief Portable SIMD layer: CPU detection, batch math, vectorized loops.
 *
 *  Uses `third_party/xsimd` when its headers are visible, `third_party/simdette`
 *  as a secondary source of math kernels, and a scalar fallback otherwise.
 *  All entry points compile with or without either dependency; availability
 *  is reported by @ref have_xsimd() / @ref have_simdette().
 *
 *  Header-only, C++17. Include paths must cover `third_party` (CMake does).
 */
#ifndef JITERATI_SIMD_HPP_INCLUDED
#define JITERATI_SIMD_HPP_INCLUDED

#if defined(__has_include)
#if __has_include("xsimd/include/xsimd/xsimd.hpp")
#define JITERATI_HAS_XSIMD 1
#include "xsimd/include/xsimd/xsimd.hpp"
#elif __has_include("xsimd/xsimd.hpp")
#define JITERATI_HAS_XSIMD 1
#include "xsimd/xsimd.hpp"
#endif
#if __has_include("simdette/include/simdette/simdette.hpp")
#define JITERATI_HAS_SIMDETTE 1
#elif __has_include("simdette/simdette.hpp")
#define JITERATI_HAS_SIMDETTE 1
#endif
#endif

#ifndef JITERATI_HAS_XSIMD
#define JITERATI_HAS_XSIMD 0
#endif
#ifndef JITERATI_HAS_SIMDETTE
#define JITERATI_HAS_SIMDETTE 0
#endif

#include <cmath>
#include <cstddef>
#include <string>

namespace jiterati::simd {

/** Compile-time availability probes. */
inline bool have_xsimd() { return JITERATI_HAS_XSIMD != 0; }
inline bool have_simdette() { return JITERATI_HAS_SIMDETTE != 0; }

/** Runtime CPU capability snapshot. */
struct CpuFeatures {
    bool sse2 = false;
    bool avx = false;
    bool avx2 = false;
    bool avx512f = false;
    bool neon = false;
    bool sve = false;
    bool vsx = false;
    bool rvv = false;
    bool wasm_simd = false;
};

/** Detect host capabilities via compile-time macros + builtin queries. */
inline CpuFeatures cpu_features() {
    CpuFeatures f;
#if defined(__SSE2__) || defined(_M_X64)
    f.sse2 = true;
#endif
#if defined(__AVX__)
    f.avx = true;
#endif
#if defined(__AVX2__)
    f.avx2 = true;
#endif
#if defined(__AVX512F__)
    f.avx512f = true;
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
    f.neon = true;
#endif
#if defined(__ARM_FEATURE_SVE)
    f.sve = true;
#endif
#if defined(__VSX__)
    f.vsx = true;
#endif
#if defined(__riscv_vector)
    f.rvv = true;
#endif
#if defined(__wasm_simd128__)
    f.wasm_simd = true;
#endif
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx")) f.avx = true;
    if (__builtin_cpu_supports("avx2")) f.avx2 = true;
    if (__builtin_cpu_supports("avx512f")) f.avx512f = true;
#endif
    return f;
}

/** Preferred vector width in elements for `T` on this build. */
template <typename T>
inline std::size_t vector_width() {
#if JITERATI_HAS_XSIMD
    return xsimd::simd_traits<T>::size;
#else
    CpuFeatures f = cpu_features();
    std::size_t bytes = 16;
    if (f.avx512f) bytes = 64;
    else if (f.avx || f.avx2) bytes = 32;
    (void)f;
    return bytes / sizeof(T) > 0 ? bytes / sizeof(T) : 1;
#endif
}

inline std::string describe_cpu() {
    CpuFeatures f = cpu_features();
    std::string out;
    if (f.sse2) out += "sse2 ";
    if (f.avx) out += "avx ";
    if (f.avx2) out += "avx2 ";
    if (f.avx512f) out += "avx512f ";
    if (f.neon) out += "neon ";
    if (f.sve) out += "sve ";
    if (f.vsx) out += "vsx ";
    if (f.rvv) out += "rvv ";
    if (f.wasm_simd) out += "wasm-simd128 ";
    if (out.empty()) out = "scalar";
    return out;
}

/** Elementwise kernels. xsimd path fuses into batches; scalar path loops. */
template <typename T>
inline void batch_add(const T* a, const T* b, T* out, std::size_t n) {
#if JITERATI_HAS_XSIMD
    using batch = xsimd::simd_type<T>;
    constexpr std::size_t w = xsimd::simd_traits<T>::size;
    std::size_t i = 0;
    for (; i + w <= n; i += w) {
        batch va = xsimd::load_unaligned(a + i);
        batch vb = xsimd::load_unaligned(b + i);
        (va + vb).store_unaligned(out + i);
    }
    for (; i < n; ++i) out[i] = a[i] + b[i];
#else
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] + b[i];
#endif
}

template <typename T>
inline void batch_mul(const T* a, const T* b, T* out, std::size_t n) {
#if JITERATI_HAS_XSIMD
    using batch = xsimd::simd_type<T>;
    constexpr std::size_t w = xsimd::simd_traits<T>::size;
    std::size_t i = 0;
    for (; i + w <= n; i += w) {
        batch va = xsimd::load_unaligned(a + i);
        batch vb = xsimd::load_unaligned(b + i);
        (va * vb).store_unaligned(out + i);
    }
    for (; i < n; ++i) out[i] = a[i] * b[i];
#else
    for (std::size_t i = 0; i < n; ++i) out[i] = a[i] * b[i];
#endif
}

/** Fused multiply-add: out = a*b + c. */
template <typename T>
inline void batch_fma(const T* a, const T* b, const T* c, T* out, std::size_t n) {
#if JITERATI_HAS_XSIMD
    using batch = xsimd::simd_type<T>;
    constexpr std::size_t w = xsimd::simd_traits<T>::size;
    std::size_t i = 0;
    for (; i + w <= n; i += w) {
        batch va = xsimd::load_unaligned(a + i);
        batch vb = xsimd::load_unaligned(b + i);
        batch vc = xsimd::load_unaligned(c + i);
        xsimd::fma(va, vb, vc).store_unaligned(out + i);
    }
    for (; i < n; ++i) out[i] = std::fma(a[i], b[i], c[i]);
#else
    for (std::size_t i = 0; i < n; ++i) out[i] = std::fma(a[i], b[i], c[i]);
#endif
}

template <typename T>
inline void batch_sqrt(const T* a, T* out, std::size_t n) {
#if JITERATI_HAS_XSIMD
    using batch = xsimd::simd_type<T>;
    constexpr std::size_t w = xsimd::simd_traits<T>::size;
    std::size_t i = 0;
    for (; i + w <= n; i += w)
        xsimd::sqrt(xsimd::load_unaligned(a + i)).store_unaligned(out + i);
    for (; i < n; ++i) out[i] = std::sqrt(a[i]);
#else
    for (std::size_t i = 0; i < n; ++i) out[i] = std::sqrt(a[i]);
#endif
}

/** Apply `body(i)` over `[0, n)` in width-sized strides (hint for autovec). */
template <typename Body>
inline void vectorized_loop(std::size_t n, Body&& body) {
    std::size_t w = vector_width<float>();
    std::size_t i = 0;
    for (; i + w <= n; i += w)
        for (std::size_t k = 0; k < w; ++k) body(i + k);
    for (; i < n; ++i) body(i);
}

/** Autovectorization policy consumed by Passlib/AutoVectorize. */
enum class VectorPolicy {
    Auto,      ///< Pick widest width the CPU reports.
    Scalar,    ///< Force scalar lowering.
    Fixed128,  ///< Force 128-bit vectors.
    Fixed256,  ///< Force 256-bit vectors (x86 AVX) where legal.
    Fixed512,  ///< Force 512-bit vectors (AVX-512) where legal.
};

inline std::size_t policy_width(VectorPolicy policy, std::size_t elt_bytes) {
    switch (policy) {
        case VectorPolicy::Scalar: return 1;
        case VectorPolicy::Fixed128: return 16 / elt_bytes;
        case VectorPolicy::Fixed256: return 32 / elt_bytes;
        case VectorPolicy::Fixed512: return 64 / elt_bytes;
        case VectorPolicy::Auto:
        default: {
            CpuFeatures f = cpu_features();
            if (f.avx512f) return 64 / elt_bytes;
            if (f.avx || f.avx2) return 32 / elt_bytes;
            return 16 / elt_bytes;
        }
    }
}

} // namespace jiterati::simd

#endif // JITERATI_SIMD_HPP_INCLUDED
