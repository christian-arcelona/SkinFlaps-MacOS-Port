//#####################################################################
// Copyright (c) 2019, Eftychios Sifakis, Yutian Tao, Qisi Wang
// Distributed under the FreeBSD license (see license.txt)
//#####################################################################
// CHOLMOD-backed implementation of the PardisoWrapper interface. MKL PARDISO
// forms the Schur complement natively (iparm[35]); CHOLMOD has no equivalent,
// so the m>0 path factors A11 with CHOLMOD and forms the dense Schur block
// S = A22 - A21 A11^-1 A12 explicitly, factored with LAPACK. An augmented
// sparse factorization of the full system was measured 3.35x slower than this
// architecture, so the explicit Schur formation is load-bearing.
#pragma once
#include <iostream>
#include <vector>
#include <algorithm>
#include <cholmod.h>
#include <Eigen/Sparse>
#include <Eigen/CholmodSupport>

// Exposes the raw cholmod_factor Eigen holds internally (a protected member of
// Eigen::CholmodBase), so parallel column batches can run cholmod_solve against
// the one shared, read-only A11 factor with private cholmod_common workspaces.
template <class MatrixType>
struct ExposedCholmodLLT : Eigen::CholmodSupernodalLLT<MatrixType> {
    cholmod_factor* rawFactor() const { return this->m_cholmodFactor; }
};

template <class T, class IntType_> struct PardisoWrapper {
    using IntType = IntType_;

    IntType  n = 0;               // dimension of the matrix
    int      m = 0;               // number of nodes in the Schur complement part
    IntType *schurNodes = nullptr;

    IntType *rowIndex = nullptr;  // upper-triangular CSR
    IntType *column = nullptr;
    T       *value = nullptr;
    // Dense Schur block, kept in double end-to-end. The local-global iteration
    // sits near its stability edge on post-cut topologies; the double-precision
    // factor and solve absorb platform reduction-order differences that a float
    // Schur block does not.
    double  *schur_d = nullptr;

    cholmod_common common{};
    cholmod_factor* chol_factor = nullptr;      // full-system factor, m==0 solve path
    // Baseline matrix the current factors reflect. numericFact diffs the new
    // matrix against it: a small low-rank pattern-preserving change (a suture)
    // is folded into the existing factors incrementally instead of refactoring.
    cholmod_sparse* chol_A_prev = nullptr;
    // Schur complement from the previous m>0 numericFact; the baseline for the
    // rank-k Woodbury update. Valid only while the partition is unchanged.
    Eigen::MatrixXd schur_prev;
    bool schur_prev_valid = false;

    // Persistent Schur block-eliminate state (populated during numericFact when m > 0)
    Eigen::SparseMatrix<double> eigen_A11;  // (n-m) x (n-m), SPD
    Eigen::SparseMatrix<double> eigen_A12;  // (n-m) x m
    Eigen::SparseMatrix<double> eigen_A21;  // m x (n-m)
    ExposedCholmodLLT<Eigen::SparseMatrix<double>> eigen_A11_solver;
    bool eigen_schur_initialized = false;
    bool analyzed = false;

    void initialize(const IntType _n, const IntType _nnz, const IntType _m = 0);

    void factSchur();

    void factorize() {
        symbolicFact();
        numericFact();
    }

    void symbolicFact();
    void numericFact();

    void releasePardisoInternal();
    void deallocate();

    void forwardSubstitution(T* const _rhs, T* const _x);
    void diagSolve(T* const _rhs, T* const _x);
    void backwardSubstitution(T* const _rhs, T* const _x);

    // Full Schur split-solve (forward + diag + backward in one call) using a
    // caller-provided cholmod_common, so the three coordinate solves can run
    // in parallel against the shared, read-only A11 factor. The shared Eigen
    // solve workspace is not thread-safe; this path bypasses it. m>0 only.
    void solveSchurOneShot_tls(const T* rhs, T* x, cholmod_common& cc) const;
};
