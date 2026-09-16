// Dense BLAS/LAPACK plumbing. On macOS the vendor's MKL entry points map to
// Apple Accelerate: Fortran LAPACK symbols plus vecLib CBLAS.
#pragma once

#if defined(__APPLE__)
#  ifndef ACCELERATE_NEW_LAPACK
#    define ACCELERATE_NEW_LAPACK 1
#  endif
#  include <Accelerate/Accelerate.h>
#  include <vecLib/cblas.h>
using LAPACK_int_t = __LAPACK_int;
#else
extern "C" {
    void dpotrf_(char* uplo, int* n, double* a, int* lda, int* info);
    void dpotrs_(char* uplo, int* n, int* nrhs, const double* a, int* lda, double* b, int* ldb, int* info);
    void spotrf_(char* uplo, int* n, float* a, int* lda, int* info);
    void spotrs_(char* uplo, int* n, int* nrhs, const float* a, int* lda, float* b, int* ldb, int* info);
}
#  include <cblas.h>
using LAPACK_int_t = int;
#endif

// MKL PARDISO is not used on this port; sparse direct solves go through CHOLMOD.
template<class T, class IntType> struct PardisoPolicy {};

template<class T> struct LAPACKPolicy;

template<> struct LAPACKPolicy<double> {
    static inline int fact(const int m, double* a) {
        char u = 'U'; LAPACK_int_t n = m, lda = m, info = 0;
        dpotrf_(&u, &n, a, &lda, &info);
        return static_cast<int>(info);
    }
    static inline int solve(const int m, const int nrhs, const double* a, double* b) {
        char u = 'U'; LAPACK_int_t n = m, lda = m, ldb = m, info = 0, nr = nrhs;
        dpotrs_(&u, &n, &nr, a, &lda, b, &ldb, &info);
        return static_cast<int>(info);
    }
};

template<> struct LAPACKPolicy<float> {
    static inline int fact(const int m, float* a) {
        char u = 'U'; LAPACK_int_t n = m, lda = m, info = 0;
        spotrf_(&u, &n, a, &lda, &info);
        return static_cast<int>(info);
    }
    static inline int solve(const int m, const int nrhs, const float* a, float* b) {
        char u = 'U'; LAPACK_int_t n = m, lda = m, ldb = m, info = 0, nr = nrhs;
        spotrs_(&u, &n, &nr, a, &lda, b, &ldb, &info);
        return static_cast<int>(info);
    }
};

template<class T> struct CBLASPolicy;
template<> struct CBLASPolicy<double> {
    static constexpr CBLAS_ORDER matrix_order = CblasRowMajor;
    static constexpr CBLAS_UPLO uplo = CblasUpper;
    using T = double;
    static inline void mutiplyAdd(T* result, const int n, const T alpha, const T* a, const T* x, const T beta) {
        cblas_dsymv(matrix_order, uplo, n, alpha, a, n, x, 1, beta, result, 1);
    }
};

template<> struct CBLASPolicy<float> {
    static constexpr CBLAS_ORDER matrix_order = CblasRowMajor;
    static constexpr CBLAS_UPLO uplo = CblasUpper;
    using T = float;
    static inline void mutiplyAdd(T* result, const int n, const T alpha, const T* a, const T* x, const T beta) {
        cblas_ssymv(matrix_order, uplo, n, alpha, a, n, x, 1, beta, result, 1);
    }
};
