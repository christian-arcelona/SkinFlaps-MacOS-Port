//#####################################################################
// Copyright (c) 2019, Eftychios Sifakis, Yutian Tao, Qisi Wang
// Distributed under the FreeBSD license (see license.txt)
//#####################################################################
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>

#include "MKLWrapper.h"
#include "PardisoWrapper.h"
#include "SimulationFlags.h"
#include "PDConstraints.h"

#include "Discretization.h"

// for debug
#include "dumper.h"
#include <chrono>

namespace PhysBAM {

template <class Discretization, class IntType> struct SchurSolver {

    using DiscretizationType = Discretization;
    using StateVariableType = typename DiscretizationType::StateVariableType;
    using IteratorType = Iterator<StateVariableType>;

    using IndexType = typename IteratorType::IndexType;
    using VectorType = typename IteratorType::DataType;
    using T = typename VectorType::ELEMENT;
    static constexpr int d = VectorType::dimension;

    using GradientMatrixType = MATRIX<T, d>;

    static constexpr int elementNodes = d + 1;
    using ElementType = std::array<IndexType, elementNodes>;
    using ElementIndexType = std::array<IndexType, elementNodes>;
    using NodeArrayType = typename IteratorType::template ContainerType<NodeType>;
    using NumberingArrayType = typename IteratorType::template ContainerType<IntType>;

    using Constraint = SoftConstraint<VectorType, elementNodes, IndexType>;
    using Suture = SutureConstraint<VectorType, elementNodes, IndexType>;
    using CollisionSuture = SlidingConstraint <VectorType, elementNodes, IndexType>;
    using InternodeConstraint = NodeToNodesConstraint<VectorType, IndexType>;


    IntType schurSize = IntType(0);
    NumberingArrayType m_numbering; // only number the active nodes, collisionNodes at the bottom
    std::vector<std::map<int, T>> m_tensor;
    // Baseline of the double-precision Schur block (post-numericFact, before any
    // collision-stiffness contributions). updatePardiso restores from it each
    // step, so the per-frame collision deltas never accumulate.
    double *m_originalValue_d = nullptr;
    T *m_x = nullptr;
    T *m_rhs = nullptr;
    mutable PardisoWrapper<T, IntType> m_pardiso;
    // Fingerprint of the collision constraint set baked into the current Schur
    // factor. On a match the whole restore + delta + refactor cycle is skipped.
    // Invalidated whenever the baseline changes (initializePardiso /
    // reInitializePardiso).
    mutable uint64_t m_last_collision_fingerprint = 0;
    mutable bool m_collision_fingerprint_valid = false;

    void initialize(const NodeArrayType& nodeType);

    template <int elementNodesN>
    void accumToTensor(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix,
        const std::array<IndexType, elementNodesN>& elementIndex);

    template <int elementNodesN>
    void accumToTensor_debug(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix, const std::array<IndexType, elementNodesN>& elementIndex);  // COURT added to separate out microNode crash in Release

    template <int elementNodesN>
    void updateTensor(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix,
        const std::array<IndexType, elementNodesN>& elementIndex);

    void updatePardiso(
        const std::vector<Constraint> &collisionConstraints,
        const std::vector<CollisionSuture>& collisionSutures
    );

    void computeTensor(const std::vector<ElementType>& elements, const std::vector<GradientMatrixType>& gradients, const std::vector<T>& restVol, const T mu, const std::vector<Suture>& sutures, const std::vector<InternodeConstraint>& microNodes);

    void computeTensor(const std::vector<ElementType>& elements, const std::vector<GradientMatrixType>& gradients, const std::vector<T>& restVol, const std::vector<T>& muLow, const std::vector<T>& muHigh, const std::vector<Suture>& sutures, const std::vector<InternodeConstraint>& microNodes);

    inline void reInitializePardiso(const std::vector<Constraint>& constraints, const std::vector<Suture>& sutures, const std::vector<Constraint>& fakeSutures, const std::vector<InternodeConstraint>& microNodes) {
        factPardiso(constraints, sutures, fakeSutures, microNodes);
        if (schurSize) {
            if (m_originalValue_d && m_pardiso.schur_d) {
                for (IntType i = 0; i < schurSize * schurSize; i++)
                    m_originalValue_d[i] = m_pardiso.schur_d[i];
            }
            m_pardiso.factSchur();
        }
        m_collision_fingerprint_valid = false;
    }

    template <int elementNodesN>
    void accumToPardiso(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix,
        const std::array<IndexType, elementNodesN>& elementIndex);

    void copyIn(const StateVariableType &f, const int v) const {
        // copy in x
        for (Iterator<StateVariableType> iterator(f); !iterator.isEnd(); iterator.next()) {
            const int number = iterator.value(m_numbering);
            if (number >= 0) {
                const T val = iterator.value(f)(v + 1);
                // Surface an exploding simulation as a catchable exception
                // instead of feeding NaN into the factorization.
                if (!std::isfinite(static_cast<double>(val))) {
                    std::ostringstream oss;
                    oss << "Non-finite RHS before solver copyIn"
                        << " solver_index=" << number
                        << " component=" << v;
                    throw std::runtime_error(oss.str());
                }
                m_rhs[number] = val;
            }
        }
    }

    // Parameterized copyIn / copyOut for the parallel three-coordinate solve:
    // each coordinate gets its own rhs/x buffer so the solves can run
    // concurrently.
    void copyInTo(const StateVariableType& f, const int v, T* rhs_buf) const {
        for (Iterator<StateVariableType> iterator(f); !iterator.isEnd(); iterator.next()) {
            const int number = iterator.value(m_numbering);
            if (number >= 0)
                rhs_buf[number] = iterator.value(f)(v + 1);
        }
    }

    void copyOutFrom(StateVariableType& f, const int v, const T* x_buf) const {
        for (Iterator<StateVariableType> iterator(f); !iterator.isEnd(); iterator.next()) {
            const int number = iterator.value(m_numbering);
            if (number >= 0)
                iterator.value(f)(v + 1) = x_buf[number];
        }
    }

    void copyOut(StateVariableType &f, const int v) const {
        // copy out x
        for (Iterator<StateVariableType> iterator(f); !iterator.isEnd(); iterator.next()) {
            const int number = iterator.value(m_numbering);
            if (number >= 0)
                iterator.value(f)(v + 1) = m_x[number];
        }
    }

    void solve() const {
        m_pardiso.forwardSubstitution(m_rhs, m_x);
        m_pardiso.diagSolve(m_x, m_rhs);
        m_pardiso.backwardSubstitution(m_rhs, m_x);
    }

    void inline releasePardiso() {
        m_pardiso.releasePardisoInternal();
        m_pardiso.deallocate();
    }


    void inline deallocate() {
        if (m_originalValue_d) {
            delete[] m_originalValue_d;
            m_originalValue_d = nullptr;
        }
        if (m_x) {
            delete[] m_x;
            m_x = nullptr;
        }
        if (m_rhs) {
            delete[] m_rhs;
            m_rhs = nullptr;
        }
    }

    void initializePardiso(const std::vector<Constraint>& constraints, const std::vector<Suture>& sutures, const std::vector<Constraint>& fakeSutures, const std::vector<InternodeConstraint>& microNodes);

    void factPardiso(const std::vector<Constraint>& constraints, const std::vector<Suture>& sutures, const std::vector<Constraint>& fakeSutures, const std::vector<InternodeConstraint>& microNodes);
};


} // namespace PhysBAM
