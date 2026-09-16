//#####################################################################
// CHOLMOD-backed implementation of the PardisoWrapper interface.
// See PardisoWrapper.h for the architecture rationale.
//#####################################################################

#include "PardisoWrapper.h"
#include "MKLWrapper.h"
#include <vector>
#include <stdexcept>
#include <type_traits>
#include <cstring>
#include <sstream>
#include <cmath>
#include <limits>
#include <atomic>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <Eigen/Dense>

namespace {

template<typename IntType>
cholmod_sparse* csr_upper_to_csc(long n, const IntType* rowptr, const IntType* colind, const double* avals, cholmod_common* cc) {
  const long nnz = static_cast<long>(rowptr[static_cast<size_t>(n)]);
  std::vector<long> counts(static_cast<size_t>(n), 0);
  for (long i = 0; i < n; ++i) {
    const IntType r0 = rowptr[i];
    const IntType r1 = rowptr[i+1];
    for (IntType k = r0; k < r1; ++k) {
      const long j = static_cast<long>(colind[k]);
      counts[static_cast<size_t>(j)]++;
    }
  }
  cholmod_sparse* A = cholmod_allocate_sparse(n, n, static_cast<size_t>(nnz), /*sorted=*/1, /*packed=*/1, /*stype=*/1, CHOLMOD_REAL, cc);
  if (!A) return nullptr;
  // CHOLMOD's index width is a runtime property (common.itype); writing through
  // the wrong pointer type corrupts the arrays on 32-bit-index builds.
  const bool use_long = (cc->itype == CHOLMOD_LONG);
  double* Ax = static_cast<double*>(A->x);
  if (use_long) {
    auto Ap = static_cast<SuiteSparse_long*>(A->p);
    auto Ai = static_cast<SuiteSparse_long*>(A->i);
    Ap[0] = 0;
    for (long j = 0; j < n; ++j) Ap[j+1] = Ap[j] + counts[static_cast<size_t>(j)];
    std::vector<SuiteSparse_long> cursor(static_cast<size_t>(n));
    for (long j = 0; j < n; ++j) cursor[static_cast<size_t>(j)] = Ap[j];
    for (long i = 0; i < n; ++i) {
      const IntType r0 = rowptr[i];
      const IntType r1 = rowptr[i+1];
      for (IntType k = r0; k < r1; ++k) {
        const long j = static_cast<long>(colind[k]);
        const SuiteSparse_long p = cursor[static_cast<size_t>(j)]++;
        Ai[p] = static_cast<SuiteSparse_long>(i);
        Ax[p] = avals[k];
      }
    }
  } else {
    auto Ap = static_cast<int*>(A->p);
    auto Ai = static_cast<int*>(A->i);
    Ap[0] = 0;
    for (long j = 0; j < n; ++j) Ap[j+1] = Ap[j] + static_cast<int>(counts[static_cast<size_t>(j)]);
    std::vector<int> cursor(static_cast<size_t>(n));
    for (long j = 0; j < n; ++j) cursor[static_cast<size_t>(j)] = Ap[j];
    for (long i = 0; i < n; ++i) {
      const IntType r0 = rowptr[i];
      const IntType r1 = rowptr[i+1];
      for (IntType k = r0; k < r1; ++k) {
        const long j = static_cast<long>(colind[k]);
        const int p = cursor[static_cast<size_t>(j)]++;
        Ai[p] = static_cast<int>(i);
        Ax[p] = avals[k];
      }
    }
  }
  return A;
}

template<typename T>
cholmod_dense* wrap_dense_vector(T* x, long n, cholmod_common* cc) {
  cholmod_dense* b = cholmod_allocate_dense(n, 1, n, CHOLMOD_REAL, cc);
  if (!b) return nullptr;
  if constexpr (std::is_same<T,double>::value) {
    std::memcpy(b->x, x, sizeof(double)*n);
  } else {
    double* bx = static_cast<double*>(b->x);
    for (long i=0;i<n;++i) bx[i] = static_cast<double>(x[i]);
  }
  return b;
}

template<typename T>
void unwrap_dense_vector(const cholmod_dense* dx, T* out, long n) {
  const double* x = static_cast<const double*>(dx->x);
  if constexpr (std::is_same<T,double>::value) {
    std::memcpy(out, x, sizeof(double)*n);
  } else {
    for (long i=0;i<n;++i) out[i] = static_cast<float>(x[i]);
  }
}

// Incremental refactorization via cholmod_updown: fold the change
// (A_new - A_old) into the existing simplicial LDL' factor L instead of
// refactoring from scratch. Applied only when the change is small, low-rank
// and pattern-preserving -- the signature of a suture. Returns true iff L now
// reflects A_new exactly (to double rounding); false means the caller must do
// a full refactor (a failed/partial updown is wiped by cholmod_factorize).
static bool cholmod_updown_refactor(cholmod_sparse* A_old, cholmod_sparse* A_new,
                                    cholmod_factor* L, cholmod_common* cc,
                                    long max_touched_rows) {
  if (!A_old || !A_new || !L) return false;
  if (L->is_super) return false;                 // updown needs simplicial
  if (L->xtype == CHOLMOD_PATTERN) return false; // factor not yet numeric
  if (cc->itype != CHOLMOD_INT) return false;    // helper assumes int indices
  if (L->n != A_new->nrow || A_old->nrow != A_new->nrow) return false;
  const long n = static_cast<long>(A_new->nrow);

  double one[2] = {1.0, 0.0}, mone[2] = {-1.0, 0.0};
  cholmod_sparse* delta = cholmod_add(A_new, A_old, one, mone,
                                      /*values=*/1, /*sorted=*/1, cc);
  if (!delta) return false;
  delta->stype = 1;

  const int*    Dp = static_cast<const int*>(delta->p);
  const int*    Di = static_cast<const int*>(delta->i);
  const double* Dx = static_cast<const double*>(delta->x);
  // cholmod_add keeps explicit zeros where values cancelled; suture entries
  // are O(stiffness), many orders of magnitude above this cutoff.
  const double ztol = 1e-300;

  std::vector<int> touched;
  for (long j = 0; j < n; ++j) {
    for (int p = Dp[j]; p < Dp[j+1]; ++p) {
      if (std::fabs(Dx[p]) <= ztol) continue;
      touched.push_back(Di[p]);
      touched.push_back(static_cast<int>(j));
    }
  }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  if (touched.empty()) {             // matrix unchanged -- factor already valid
    cholmod_free_sparse(&delta, cc);
    return true;
  }
  if (static_cast<long>(touched.size()) > max_touched_rows) {
    cholmod_free_sparse(&delta, cc);
    return false;                    // too large -- not a suture-shaped update
  }
  const int r = static_cast<int>(touched.size());
  auto local_of = [&](int g) -> int {
    return static_cast<int>(std::lower_bound(touched.begin(), touched.end(), g)
                            - touched.begin());
  };

  // Dense r x r symmetric block of the delta restricted to the touched rows.
  Eigen::MatrixXd D = Eigen::MatrixXd::Zero(r, r);
  for (long j = 0; j < n; ++j) {
    if (Dp[j] == Dp[j+1]) continue;
    auto jt = std::lower_bound(touched.begin(), touched.end(), (int)j);
    if (jt == touched.end() || *jt != (int)j) continue;
    const int cj = static_cast<int>(jt - touched.begin());
    for (int p = Dp[j]; p < Dp[j+1]; ++p) {
      const double v = Dx[p];
      if (std::fabs(v) <= ztol) continue;
      const int ci = local_of(Di[p]);
      D(ci, cj) += v;
      if (ci != cj) D(cj, ci) += v;  // mirror (delta upper-stored, i <= j)
    }
  }
  cholmod_free_sparse(&delta, cc);

  // Eigendecompose the small symmetric block: delta = sum_k w_k v_k v_k^T.
  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(D);
  if (es.info() != Eigen::Success) return false;
  const Eigen::VectorXd w = es.eigenvalues();
  const Eigen::MatrixXd V = es.eigenvectors();
  double wmax = 0.0;
  for (int k = 0; k < r; ++k) wmax = std::max(wmax, std::fabs(w[k]));
  if (wmax == 0.0) return true;      // delta numerically zero -- factor valid
  const double weps = wmax * 1e-13;  // drop negligible eigenpairs

  // Positive eigenpairs apply as a rank update (+C C^T), negatives as a
  // downdate (-C C^T). A pure suture is PSD, so usually positives only.
  for (int sign = 0; sign < 2; ++sign) {
    std::vector<int> cols;
    for (int k = 0; k < r; ++k) {
      if (std::fabs(w[k]) <= weps) continue;
      if ((sign == 0 && w[k] > 0.0) || (sign == 1 && w[k] < 0.0))
        cols.push_back(k);
    }
    if (cols.empty()) continue;
    const int kc = static_cast<int>(cols.size());
    cholmod_triplet* TC = cholmod_allocate_triplet(
        (size_t)n, (size_t)kc, (size_t)r * (size_t)kc,
        /*stype=*/0, CHOLMOD_REAL, cc);
    if (!TC) return false;
    int*    Ti = static_cast<int*>(TC->i);
    int*    Tj = static_cast<int*>(TC->j);
    double* Tx = static_cast<double*>(TC->x);
    size_t cnz = 0;
    for (int c = 0; c < kc; ++c) {
      const int k = cols[c];
      const double scale = std::sqrt(std::fabs(w[k]));
      for (int t = 0; t < r; ++t) {
        const double val = scale * V(t, k);
        if (val == 0.0) continue;
        Ti[cnz] = touched[t];
        Tj[cnz] = c;
        Tx[cnz] = val;
        ++cnz;
      }
    }
    TC->nnz = cnz;
    cholmod_sparse* C = cholmod_triplet_to_sparse(TC, cnz, cc);
    cholmod_free_triplet(&TC, cc);
    if (!C) return false;
    // cholmod_updown requires C's rows permuted into L's fill-reducing ordering.
    cholmod_sparse* Cperm = cholmod_submatrix(
        C, static_cast<int*>(L->Perm), (int)L->n,
        /*cset=*/nullptr, /*csize=*/-1, /*values=*/1, /*sorted=*/1, cc);
    cholmod_free_sparse(&C, cc);
    if (!Cperm) return false;
    const int ok = cholmod_updown(/*update=*/ sign == 0 ? 1 : 0, Cperm, L, cc);
    cholmod_free_sparse(&Cperm, cc);
    if (!ok) return false;           // updown failed -- caller must refactor
  }
  return true;
}

// The suture delta extracted from A_new - A_old: the touched global rows and a
// signed low-rank factorization with E placed on the touched rows and
// J = diag(sign), so that E*J*E^T == delta A. A suture is PSD in exact
// arithmetic, but the FEM matrix is stored in float, so the rounded delta can
// carry float-epsilon negative eigenvalues -- the signed form keeps the
// reconstruction exact instead of clamping.
struct SutureDelta {
  std::vector<int>    touched;   // sorted global row indices
  Eigen::MatrixXd     E_local;   // touched.size() x k : columns sqrt(|lambda|)*u
  std::vector<double> sign;      // k entries, +1/-1 : the J diagonal
  bool empty_delta = false;
};

// Returns false when the change is not a small low-rank update (too large or a
// precondition miss) -- the caller then does the full Schur formation.
static bool extract_suture_delta(cholmod_sparse* A_old, cholmod_sparse* A_new,
                                 cholmod_common* cc, long max_touched_rows,
                                 SutureDelta& out) {
  out.touched.clear();
  out.E_local.resize(0, 0);
  out.sign.clear();
  out.empty_delta = false;
  if (!A_old || !A_new) return false;
  if (cc->itype != CHOLMOD_INT) return false;
  if (A_old->nrow != A_new->nrow) return false;
  const long n = static_cast<long>(A_new->nrow);

  double one[2] = {1.0, 0.0}, mone[2] = {-1.0, 0.0};
  cholmod_sparse* delta = cholmod_add(A_new, A_old, one, mone,
                                      /*values=*/1, /*sorted=*/1, cc);
  if (!delta) return false;
  delta->stype = 1;
  const int*    Dp = static_cast<const int*>(delta->p);
  const int*    Di = static_cast<const int*>(delta->i);
  const double* Dx = static_cast<const double*>(delta->x);
  const double ztol = 1e-300;

  std::vector<int> touched;
  for (long j = 0; j < n; ++j)
    for (int p = Dp[j]; p < Dp[j+1]; ++p)
      if (std::fabs(Dx[p]) > ztol) {
        touched.push_back(Di[p]);
        touched.push_back(static_cast<int>(j));
      }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  if (touched.empty()) { cholmod_free_sparse(&delta, cc); out.empty_delta = true; return true; }
  if (static_cast<long>(touched.size()) > max_touched_rows) {
    cholmod_free_sparse(&delta, cc);
    return false;
  }
  const int r = static_cast<int>(touched.size());
  auto loc = [&](int g) {
    return static_cast<int>(std::lower_bound(touched.begin(), touched.end(), g)
                            - touched.begin());
  };
  Eigen::MatrixXd D = Eigen::MatrixXd::Zero(r, r);
  for (long j = 0; j < n; ++j) {
    if (Dp[j] == Dp[j+1]) continue;
    auto jt = std::lower_bound(touched.begin(), touched.end(), (int)j);
    if (jt == touched.end() || *jt != (int)j) continue;
    const int cj = static_cast<int>(jt - touched.begin());
    for (int p = Dp[j]; p < Dp[j+1]; ++p) {
      const double v = Dx[p];
      if (std::fabs(v) <= ztol) continue;
      const int ci = loc(Di[p]);
      D(ci, cj) += v;
      if (ci != cj) D(cj, ci) += v;
    }
  }
  cholmod_free_sparse(&delta, cc);

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(D);
  if (es.info() != Eigen::Success) return false;
  const Eigen::VectorXd w = es.eigenvalues();
  const Eigen::MatrixXd V = es.eigenvectors();
  double wmax = 0.0;
  for (int i = 0; i < r; ++i) wmax = std::max(wmax, std::fabs(w[i]));
  if (wmax == 0.0) { out.empty_delta = true; return true; }
  const double weps = wmax * 1e-13;
  std::vector<int> cols;
  for (int i = 0; i < r; ++i) if (std::fabs(w[i]) > weps) cols.push_back(i);
  const int k = static_cast<int>(cols.size());
  Eigen::MatrixXd E(r, k);
  std::vector<double> sgn(static_cast<size_t>(k));
  for (int c = 0; c < k; ++c) {
    const double lam = w[cols[c]];
    sgn[static_cast<size_t>(c)] = (lam >= 0.0) ? 1.0 : -1.0;
    const double s = std::sqrt(std::fabs(lam));
    for (int t = 0; t < r; ++t) E(t, c) = s * V(t, cols[c]);
  }
  out.touched = std::move(touched);
  out.E_local = std::move(E);
  out.sign = std::move(sgn);
  return true;
}

// Rank-k Woodbury update of the Schur complement. With A_new = A_old + E J E^T
// (J = diag(sign)) split by the active/collision partition (E = [E1; E2]):
//   Y = A11_old^-1 E1,   M = J + E1^T Y,   G = M^-1 (Y^T A12_old - E2^T),
//   S_new = S_old + (A21_old Y - E2) G.
// A11_old_solver must still hold the previous numericFact's A11 factor.
// Returns false on a precondition miss -- caller does the full formation.
static bool woodbury_schur(
    const SutureDelta& d, int n_active, int m_s,
    Eigen::CholmodSupernodalLLT<Eigen::SparseMatrix<double>>& A11_old_solver,
    const Eigen::SparseMatrix<double>& A12_old,
    const Eigen::SparseMatrix<double>& A21_old,
    const Eigen::MatrixXd& S_old,
    Eigen::MatrixXd& S_new_out) {
  if (S_old.rows() != m_s || S_old.cols() != m_s) return false;
  if (A12_old.rows() != n_active || A12_old.cols() != m_s) return false;
  if (A21_old.rows() != m_s || A21_old.cols() != n_active) return false;
  if (d.empty_delta || d.E_local.cols() == 0) { S_new_out = S_old; return true; }
  const int k = static_cast<int>(d.E_local.cols());
  if (static_cast<int>(d.sign.size()) != k) return false;

  Eigen::MatrixXd E1 = Eigen::MatrixXd::Zero(n_active, k);
  Eigen::MatrixXd E2 = Eigen::MatrixXd::Zero(m_s, k);
  for (size_t t = 0; t < d.touched.size(); ++t) {
    const int g = d.touched[t];
    if (g < n_active) {
      E1.row(g) = d.E_local.row(static_cast<int>(t));
    } else {
      const int gc = g - n_active;
      if (gc < 0 || gc >= m_s) return false;
      E2.row(gc) = d.E_local.row(static_cast<int>(t));
    }
  }
  Eigen::MatrixXd Y = A11_old_solver.solve(E1);          // k solves, OLD factor
  if (A11_old_solver.info() != Eigen::Success) return false;
  // M is symmetric but indefinite (signed J), so use a general LU.
  Eigen::MatrixXd M = E1.transpose() * Y;
  for (int i = 0; i < k; ++i) M(i, i) += d.sign[static_cast<size_t>(i)];
  Eigen::MatrixXd rhsG = (Y.transpose() * A12_old) - E2.transpose();   // k x m_s
  Eigen::PartialPivLU<Eigen::MatrixXd> luM(M);
  Eigen::MatrixXd G = luM.solve(rhsG);
  Eigen::MatrixXd P = (A21_old * Y) - E2;                              // m_s x k
  S_new_out = S_old + P * G;
  return S_new_out.allFinite();
}

template<typename Scalar>
void throw_if_non_finite_array(const Scalar* values, const size_t count, const char* label) {
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(static_cast<double>(values[i]))) {
      std::ostringstream oss;
      oss << label << " contains non-finite value at index " << i;
      throw std::runtime_error(oss.str());
    }
  }
}

} // namespace

template<class T, class IntType>
void PardisoWrapper<T, IntType>::initialize(const IntType _n, const IntType _nnz, const IntType _m) {
  n = _n;
  m = static_cast<int>(_m);
  rowIndex = new IntType[n+1];
  column = new IntType[_nnz];
  value = new T[_nnz];
  if (m) schurNodes = new IntType[n];
  analyzed = false;
  chol_factor = nullptr;
  chol_A_prev = nullptr;
  schur_prev_valid = false;
  cholmod_start(&common);
  common.nmethods = 1;
  common.method[0].ordering = CHOLMOD_AMD;
  common.postorder = 1;
  // Simplicial LDL' rather than supernodal LL': cholmod_updown (the
  // incremental suture path) only operates on simplicial factors.
  common.supernodal = CHOLMOD_SIMPLICIAL;
  common.final_ll = 0;
  eigen_A11.resize(0, 0);
  eigen_A12.resize(0, 0);
  eigen_A21.resize(0, 0);
  eigen_schur_initialized = false;
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::symbolicFact() {
  std::vector<double> a_double(rowIndex[n]);
  for (IntType k=0;k<rowIndex[n];++k) a_double[static_cast<size_t>(k)] = static_cast<double>(value[k]);
  cholmod_sparse* A = csr_upper_to_csc<IntType>(static_cast<long>(n), rowIndex, column, a_double.data(), &common);
  if (!A) throw std::runtime_error("CHOLMOD: failed to build sparse matrix");
  (void)cholmod_check_sparse(A, &common);
  if (chol_factor) { cholmod_free_factor(&chol_factor, &common); chol_factor = nullptr; }
  chol_factor = cholmod_analyze(A, &common);
  cholmod_free_sparse(&A, &common);
  if (!chol_factor) throw std::runtime_error("CHOLMOD: analyze failed");
  analyzed = true;
  // A re-analyze means the topology / partition changed: drop the incremental
  // baselines so the next numericFact does a full refactor and re-arms them.
  if (chol_A_prev) { cholmod_free_sparse(&chol_A_prev, &common); chol_A_prev = nullptr; }
  schur_prev_valid = false;
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::numericFact() {
  if (!analyzed) symbolicFact();
  std::vector<double> a_double(rowIndex[n]);
  for (IntType k = 0; k < rowIndex[n]; ++k) {
    const double v = static_cast<double>(value[k]);
    if (!std::isfinite(v)) {
      std::ostringstream oss;
      oss << "CHOLMOD: matrix value[] has non-finite entry at nnz index " << k;
      throw std::runtime_error(oss.str());
    }
    a_double[static_cast<size_t>(k)] = v;
  }
  cholmod_sparse* new_A =
      csr_upper_to_csc<IntType>(static_cast<long>(n), rowIndex, column, a_double.data(), &common);
  if (!new_A) throw std::runtime_error("CHOLMOD: failed to rebuild sparse matrix");
  (void)cholmod_check_sparse(new_A, &common);

  if (m == 0) {
    // Fold a suture-shaped change into the existing factor; refactor otherwise.
    const bool did_updown = (chol_A_prev != nullptr) &&
        cholmod_updown_refactor(chol_A_prev, new_A, chol_factor, &common,
                                /*max_touched_rows=*/256);
    if (!did_updown) {
      if (!cholmod_factorize(new_A, chol_factor, &common)) {
        cholmod_free_sparse(&new_A, &common);
        throw std::runtime_error("CHOLMOD: factorization failed");
      }
    }
  }
  // m > 0: chol_factor is never read on the Schur path (the solve goes through
  // eigen_A11_solver + the dense Schur block), so the full-system factorize is
  // skipped entirely. A later m==0 epoch re-analyzes and refactors anyway.

  // Extract the suture delta for the Woodbury Schur update while chol_A_prev
  // still holds the previous matrix.
  SutureDelta suture_delta;
  bool suture_delta_ok = false;
  if (m > 0 && chol_A_prev != nullptr) {
    suture_delta_ok = extract_suture_delta(chol_A_prev, new_A, &common,
                                           /*max_touched_rows=*/256, suture_delta);
  }
  if (chol_A_prev) cholmod_free_sparse(&chol_A_prev, &common);
  chol_A_prev = new_A;

  eigen_schur_initialized = false;
  if (m) {
    const IntType n_active_int = n - static_cast<IntType>(m);
    if (n_active_int < 0) {
      throw std::runtime_error("CHOLMOD: invalid Schur block size");
    }
    const int n_active = static_cast<int>(n_active_int);
    const int m_s = static_cast<int>(m);

    // The previous numericFact's A11 factor, A12/A21 and S are still in place
    // here -- compute the rank-k Woodbury update of S before they are
    // overwritten below. Any eligibility miss falls back to full formation.
    bool schur_woodbury_done = false;
    Eigen::MatrixXd schur_woodbury;
    {
      const bool elig_dims =
          schur_prev.rows() == m_s && schur_prev.cols() == m_s &&
          eigen_A12.rows() == n_active && eigen_A12.cols() == m_s &&
          eigen_A21.rows() == m_s && eigen_A21.cols() == n_active;
      if (n_active > 0 && suture_delta_ok && schur_prev_valid && elig_dims) {
        schur_woodbury_done = woodbury_schur(
            suture_delta, n_active, m_s, eigen_A11_solver, eigen_A12, eigen_A21,
            schur_prev, schur_woodbury);
      }
    }

    // Assemble the full symmetric matrix in Eigen from upper-triangular CSR.
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(static_cast<size_t>(rowIndex[n]) * 2);
    for (IntType i = 0; i < n; ++i) {
      for (IntType kk = rowIndex[i]; kk < rowIndex[i + 1]; ++kk) {
        const IntType j = column[kk];
        const double aij = static_cast<double>(value[kk]);
        triplets.emplace_back(static_cast<int>(i), static_cast<int>(j), aij);
        if (j != i) {
          triplets.emplace_back(static_cast<int>(j), static_cast<int>(i), aij);
        }
      }
    }
    Eigen::SparseMatrix<double> fullA(static_cast<int>(n), static_cast<int>(n));
    fullA.setFromTriplets(
      triplets.begin(),
      triplets.end(),
      [](const double lhs, const double rhs) { return lhs + rhs; });
    fullA.makeCompressed();

    Eigen::MatrixXd schur_dense = Eigen::MatrixXd(fullA.block(n_active, n_active, m_s, m_s));
    if (n_active > 0) {
      eigen_A11 = fullA.block(0, 0, n_active, n_active);
      eigen_A12 = fullA.block(0, n_active, n_active, m_s);
      eigen_A21 = eigen_A12.transpose();
      eigen_A11.makeCompressed();
      eigen_A12.makeCompressed();
      eigen_A21.makeCompressed();

      eigen_A11_solver.compute(eigen_A11);
      if (eigen_A11_solver.info() != Eigen::Success) {
        throw std::runtime_error("Eigen CHOLMOD: A11 factorization failed");
      }

      if (schur_woodbury_done) {
        // The O(m_s)-RHS Schur formation is skipped entirely.
        schur_dense = schur_woodbury;
      } else {
        const Eigen::MatrixXd A12_dense = Eigen::MatrixXd(eigen_A12);
        // Column-batched parallel Schur formation. The m_s RHS columns of
        // A11 X = A12 are mutually independent: each batch solves its slice on
        // a private cholmod_common over the shared, read-only A11 factor, then
        // folds it into disjoint output columns of S -- no locks, per-column
        // arithmetic identical to a serial sweep.
        cholmod_factor* Lfac = eigen_A11_solver.rawFactor();
        if (!Lfac) {
          throw std::runtime_error("Eigen CHOLMOD: A11 factor unavailable");
        }
        std::atomic<bool> solve_failed{false};
        std::atomic<bool> nonfinite{false};
        tbb::parallel_for(
            tbb::blocked_range<int>(0, m_s, /*grainsize=*/256),
            [&](const tbb::blocked_range<int>& rng) {
              const int c0 = rng.begin();
              const int w = rng.end() - rng.begin();
              cholmod_common cc;
              cholmod_start(&cc);
              cholmod_dense B;
              std::memset(&B, 0, sizeof(B));
              B.nrow = static_cast<size_t>(n_active);
              B.ncol = static_cast<size_t>(w);
              B.nzmax = static_cast<size_t>(n_active) * static_cast<size_t>(w);
              B.d = static_cast<size_t>(n_active);
              B.x = const_cast<double*>(A12_dense.data()) +
                    static_cast<size_t>(c0) * static_cast<size_t>(n_active);
              B.xtype = CHOLMOD_REAL;
              B.dtype = CHOLMOD_DOUBLE;
              cholmod_dense* X = cholmod_solve(CHOLMOD_A, Lfac, &B, &cc);
              if (!X || !X->x) {
                solve_failed = true;
                if (X) cholmod_free_dense(&X, &cc);
                cholmod_finish(&cc);
                return;
              }
              // cholmod_solve may return a padded leading dimension (X->d);
              // map with the actual outer stride.
              Eigen::Map<const Eigen::MatrixXd, 0, Eigen::OuterStride<>> Xmap(
                  static_cast<const double*>(X->x), n_active, w,
                  Eigen::OuterStride<>(static_cast<Eigen::Index>(X->d)));
              if (!Xmap.array().isFinite().all()) nonfinite = true;
              schur_dense.middleCols(c0, w).noalias() -= eigen_A21 * Xmap;
              cholmod_free_dense(&X, &cc);
              cholmod_finish(&cc);
            });
        if (solve_failed) {
          throw std::runtime_error("Eigen CHOLMOD: parallel solve(A11, A12) failed");
        }
        if (nonfinite) {
          throw std::runtime_error("Eigen CHOLMOD: parallel solve(A11, A12) produced non-finite values");
        }
      }
    } else {
      eigen_A11.resize(0, 0);
      eigen_A12.resize(0, m_s);
      eigen_A21.resize(m_s, 0);
    }

    if (!schur_dense.array().isFinite().all()) {
      throw std::runtime_error("Eigen CHOLMOD: computed Schur complement contains non-finite values");
    }
    if (!schur_d) {
      schur_d = new double[static_cast<size_t>(m_s) * static_cast<size_t>(m_s)];
    }
    for (int i = 0; i < m_s; ++i) {
      for (int j = 0; j < m_s; ++j) {
        schur_d[i * m_s + j] = schur_dense(i, j);
      }
    }
    eigen_schur_initialized = true;

    // Cache S as the baseline for the next suture's Woodbury update.
    if (n_active > 0) {
      schur_prev = schur_dense;
      schur_prev_valid = true;
    } else {
      schur_prev_valid = false;
    }
  }
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::factSchur() {
  if (m) {
    if (!schur_d) {
      throw std::runtime_error("factSchur: schur_d not populated; numericFact must run first");
    }
    const int info = LAPACKPolicy<double>::fact(m, schur_d);
    // Match vendor: log and continue with the partial factor on a non-PD pivot
    // instead of throwing.
    if (info != 0) {
      std::cerr << "info after potrf = " << info << std::endl;
    }
  }
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::releasePardisoInternal() {
  if (chol_factor) { cholmod_free_factor(&chol_factor, &common); chol_factor = nullptr; }
  if (chol_A_prev) { cholmod_free_sparse(&chol_A_prev, &common); chol_A_prev = nullptr; }
  eigen_A11.resize(0, 0);
  eigen_A12.resize(0, 0);
  eigen_A21.resize(0, 0);
  eigen_schur_initialized = false;
  schur_prev_valid = false;
  cholmod_finish(&common);
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::deallocate() {
  if (value) { delete[] value; value = nullptr; }
  if (column) { delete[] column; column = nullptr; }
  if (rowIndex) { delete[] rowIndex; rowIndex = nullptr; }
  if (m && schurNodes) { delete[] schurNodes; schurNodes = nullptr; }
  if (schur_d) { delete[] schur_d; schur_d = nullptr; }
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::forwardSubstitution(T* const _rhs, T* const _x) {
  if (m && eigen_schur_initialized) {
    throw_if_non_finite_array(_rhs, static_cast<size_t>(n), "forwardSubstitution RHS");
    const IntType n_active_int = n - static_cast<IntType>(m);
    if (n_active_int < 0) {
      throw std::runtime_error("forwardSubstitution: invalid Schur block size");
    }
    const int n_active = static_cast<int>(n_active_int);
    const int m_s = static_cast<int>(m);

    if (n_active > 0) {
      Eigen::VectorXd b1(n_active);
      for (int i = 0; i < n_active; ++i) {
        b1[i] = static_cast<double>(_rhs[i]);
      }
      Eigen::VectorXd t = eigen_A11_solver.solve(b1);
      if (eigen_A11_solver.info() != Eigen::Success) {
        throw std::runtime_error("forwardSubstitution: solve(A11, b1) failed");
      }
      if (!t.array().isFinite().all()) {
        throw std::runtime_error("forwardSubstitution: A11^{-1}b1 is non-finite");
      }
      // Match PARDISO's Schur phase-331 convention: keep the top block in
      // b1-space, reduce only the Schur RHS.
      for (int i = 0; i < n_active; ++i) {
        _x[i] = _rhs[i];
      }
      Eigen::VectorXd b2(m_s);
      for (int i = 0; i < m_s; ++i) {
        b2[i] = static_cast<double>(_rhs[n_active + i]);
      }
      const Eigen::VectorXd r2 = b2 - (eigen_A21 * t);
      if (!r2.array().isFinite().all()) {
        throw std::runtime_error("forwardSubstitution: reduced Schur RHS is non-finite");
      }
      for (int i = 0; i < m_s; ++i) {
        _x[n_active + i] = static_cast<T>(r2[i]);
      }
      return;
    }
    for (int i = 0; i < m_s; ++i) {
      _x[i] = _rhs[i];
    }
    return;
  }
  // CHOLMOD factorizes PAP', so the split solve must apply the permutation:
  // forward = P then L, backward = L' then P'. Omitting P/P' silently returns
  // a wrong-but-plausible solution.
  cholmod_dense* b = wrap_dense_vector(_rhs, static_cast<long>(n), &common);
  cholmod_dense* pb = cholmod_solve(CHOLMOD_P, chol_factor, b, &common);
  cholmod_dense* y = cholmod_solve(CHOLMOD_L, chol_factor, pb, &common);
  unwrap_dense_vector(y, _x, static_cast<long>(n));
  cholmod_free_dense(&y, &common);
  cholmod_free_dense(&pb, &common);
  cholmod_free_dense(&b, &common);
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::diagSolve(T* const _rhs, T* const _x) {
  if (m && eigen_schur_initialized) {
    const IntType n_active_int = n - static_cast<IntType>(m);
    if (n_active_int < 0) {
      throw std::runtime_error("diagSolve: invalid Schur block size");
    }
    const int n_active = static_cast<int>(n_active_int);
    const int m_s = static_cast<int>(m);
    if (!schur_d) {
      throw std::runtime_error("diagSolve: schur_d not populated; factSchur must run first");
    }
    throw_if_non_finite_array(_rhs, static_cast<size_t>(n), "diagSolve RHS");

    std::vector<double> schur_rhs_d(static_cast<size_t>(m_s));
    for (int i = 0; i < m_s; ++i) {
      schur_rhs_d[static_cast<size_t>(i)] = static_cast<double>(_rhs[n_active + i]);
    }
    const int info = LAPACKPolicy<double>::solve(m_s, 1, schur_d, schur_rhs_d.data());
    if (info != 0) {
      throw std::runtime_error("diagSolve: dpotrs failed with info = " + std::to_string(info));
    }
    for (size_t i = 0; i < schur_rhs_d.size(); ++i) {
      if (!std::isfinite(schur_rhs_d[i])) {
        throw std::runtime_error("diagSolve: Schur output contains non-finite values");
      }
    }
    for (int i = 0; i < n_active; ++i) {
      _x[i] = _rhs[i];
    }
    for (int i = 0; i < m_s; ++i) {
      _x[n_active + i] = static_cast<T>(schur_rhs_d[static_cast<size_t>(i)]);
    }
    return;
  }
  cholmod_dense* y = wrap_dense_vector(_rhs, static_cast<long>(n), &common);
  cholmod_dense* z = cholmod_solve(CHOLMOD_D, chol_factor, y, &common);
  unwrap_dense_vector(z, _x, static_cast<long>(n));
  cholmod_free_dense(&z, &common);
  cholmod_free_dense(&y, &common);
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::backwardSubstitution(T* const _rhs, T* const _x) {
  if (m && eigen_schur_initialized) {
    throw_if_non_finite_array(_rhs, static_cast<size_t>(n), "backwardSubstitution RHS");
    const IntType n_active_int = n - static_cast<IntType>(m);
    if (n_active_int < 0) {
      throw std::runtime_error("backwardSubstitution: invalid Schur block size");
    }
    const int n_active = static_cast<int>(n_active_int);
    const int m_s = static_cast<int>(m);

    Eigen::VectorXd x2(m_s);
    for (int i = 0; i < m_s; ++i) {
      x2[i] = static_cast<double>(_rhs[n_active + i]);
    }
    if (n_active > 0) {
      Eigen::VectorXd rhs1(n_active);
      for (int i = 0; i < n_active; ++i) {
        rhs1[i] = static_cast<double>(_rhs[i]);
      }
      const Eigen::VectorXd rhs1_reduced = rhs1 - (eigen_A12 * x2);
      Eigen::VectorXd x1 = eigen_A11_solver.solve(rhs1_reduced);
      if (eigen_A11_solver.info() != Eigen::Success) {
        throw std::runtime_error("backwardSubstitution: solve(A11, rhs1-A12*x2) failed");
      }
      if (!x1.array().isFinite().all()) {
        throw std::runtime_error("backwardSubstitution: x1 is non-finite");
      }
      for (int i = 0; i < n_active; ++i) {
        _x[i] = static_cast<T>(x1[i]);
      }
    }
    for (int i = 0; i < m_s; ++i) {
      _x[n_active + i] = static_cast<T>(x2[i]);
    }
    return;
  }
  cholmod_dense* z = wrap_dense_vector(_rhs, static_cast<long>(n), &common);
  cholmod_dense* y = cholmod_solve(CHOLMOD_Lt, chol_factor, z, &common);
  cholmod_dense* x = cholmod_solve(CHOLMOD_Pt, chol_factor, y, &common);
  unwrap_dense_vector(x, _x, static_cast<long>(n));
  cholmod_free_dense(&x, &common);
  cholmod_free_dense(&y, &common);
  cholmod_free_dense(&z, &common);
}

template<class T, class IntType>
void PardisoWrapper<T, IntType>::solveSchurOneShot_tls(
    const T* rhs, T* x, cholmod_common& cc) const {
  if (!m || !eigen_schur_initialized) {
    throw std::runtime_error(
        "solveSchurOneShot_tls: only valid on the m>0 Schur path");
  }
  if (!schur_d) {
    throw std::runtime_error("solveSchurOneShot_tls: schur_d not populated");
  }
  const IntType n_active_int = n - static_cast<IntType>(m);
  if (n_active_int <= 0) {
    throw std::runtime_error("solveSchurOneShot_tls: invalid Schur block size");
  }
  const int n_active = static_cast<int>(n_active_int);
  const int m_s = static_cast<int>(m);

  cholmod_factor* Lfac = eigen_A11_solver.rawFactor();
  if (!Lfac) {
    throw std::runtime_error("solveSchurOneShot_tls: A11 factor unavailable");
  }

  std::vector<double> b1(static_cast<size_t>(n_active));
  for (int i = 0; i < n_active; ++i) b1[i] = static_cast<double>(rhs[i]);

  cholmod_dense B1;
  std::memset(&B1, 0, sizeof(B1));
  B1.nrow = static_cast<size_t>(n_active);
  B1.ncol = 1; B1.nzmax = B1.nrow; B1.d = B1.nrow;
  B1.x = b1.data();
  B1.xtype = CHOLMOD_REAL; B1.dtype = CHOLMOD_DOUBLE;
  cholmod_dense* T1 = cholmod_solve(CHOLMOD_A, Lfac, &B1, &cc);
  if (!T1 || !T1->x) {
    if (T1) cholmod_free_dense(&T1, &cc);
    throw std::runtime_error("solveSchurOneShot_tls: forward A11 solve failed");
  }
  Eigen::Map<const Eigen::VectorXd> t(
      static_cast<const double*>(T1->x), n_active);
  Eigen::VectorXd r2(m_s);
  for (int i = 0; i < m_s; ++i) r2[i] = static_cast<double>(rhs[n_active + i]);
  r2 -= (eigen_A21 * t);

  const int info = LAPACKPolicy<double>::solve(m_s, 1, schur_d, r2.data());
  if (info != 0) {
    cholmod_free_dense(&T1, &cc);
    throw std::runtime_error(
        "solveSchurOneShot_tls: dpotrs failed with info = " +
        std::to_string(info));
  }
  // r2 now holds x2 (the Schur block of the solution).

  Eigen::Map<const Eigen::VectorXd> b1_map(b1.data(), n_active);
  Eigen::VectorXd rhs1_red = b1_map - (eigen_A12 * r2);
  cholmod_dense B1r;
  std::memset(&B1r, 0, sizeof(B1r));
  B1r.nrow = static_cast<size_t>(n_active);
  B1r.ncol = 1; B1r.nzmax = B1r.nrow; B1r.d = B1r.nrow;
  B1r.x = rhs1_red.data();
  B1r.xtype = CHOLMOD_REAL; B1r.dtype = CHOLMOD_DOUBLE;
  cholmod_dense* X1 = cholmod_solve(CHOLMOD_A, Lfac, &B1r, &cc);
  if (!X1 || !X1->x) {
    cholmod_free_dense(&T1, &cc);
    if (X1) cholmod_free_dense(&X1, &cc);
    throw std::runtime_error("solveSchurOneShot_tls: backward A11 solve failed");
  }
  Eigen::Map<const Eigen::VectorXd> x1(
      static_cast<const double*>(X1->x), n_active);
  for (int i = 0; i < n_active; ++i) x[i] = static_cast<T>(x1[i]);
  for (int i = 0; i < m_s; ++i) x[n_active + i] = static_cast<T>(r2[i]);
  cholmod_free_dense(&X1, &cc);
  cholmod_free_dense(&T1, &cc);
}

template struct PardisoWrapper<float, int>;
template struct PardisoWrapper<double, int>;
