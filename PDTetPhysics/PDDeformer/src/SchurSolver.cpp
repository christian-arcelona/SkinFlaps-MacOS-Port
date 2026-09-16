#include "SchurSolver.h"
#include <cassert>
#include <cstring>

namespace PhysBAM {
    template<class Discretization, class IntType>
    inline void SchurSolver<Discretization, IntType>::initialize(const NodeArrayType& nodeType) {
        using IteratorType = Iterator<NodeArrayType>;
        IteratorType iterator(nodeType);
        iterator.resize(m_numbering);

        int numOfActiveNodes = 0;
        schurSize = 0;
        for (iterator.begin(); !iterator.isEnd(); iterator.next())
            if (iterator.value(nodeType) == NodeType::Active) {
                numOfActiveNodes++;
            }
            else if (iterator.value(nodeType) == NodeType::Collision) {
                numOfActiveNodes++;
                schurSize++;
            }
        std::cout << "    schursize   = " << schurSize << std::endl;
        std::cout << "    matrixsize  = " << numOfActiveNodes << std::endl;
        int activeIdx = 0;
        int collisionIdx = 0;
        for (iterator.begin(); !iterator.isEnd(); iterator.next())
            if (iterator.value(nodeType) == NodeType::Active)
                iterator.value(m_numbering) = activeIdx++;
            else if (iterator.value(nodeType) == NodeType::Collision)
                iterator.value(m_numbering) = numOfActiveNodes - schurSize + collisionIdx++;
            else
                iterator.value(m_numbering) = -1;

        m_tensor.clear();
        m_tensor.resize(numOfActiveNodes);

        if (schurSize) {
            m_originalValue_d = new double[static_cast<size_t>(schurSize) * static_cast<size_t>(schurSize)]();
        }

        m_rhs = new T[numOfActiveNodes];
        m_x = new T[numOfActiveNodes]();
    }

    template<class Discretization, class IntType>
    template<int elementNodesN>
    inline void SchurSolver<Discretization, IntType>::
        accumToTensor(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix, const std::array<IndexType, elementNodesN>& elementIndex) {
        using IteratorType = Iterator<NodeArrayType>;
        for (int i = 0; i < elementNodesN; i++) {
            int row = IteratorType::at(m_numbering, elementIndex[i]);
            if (row >= 0) {
                for (int j = 0; j < elementNodesN; j++) {
                    int col = IteratorType::at(m_numbering, elementIndex[j]);
                    if (col >= row) {
                        if (m_tensor[row].find(col) == m_tensor[row].end())
                            m_tensor[row].insert(std::pair<int, T>(col, stiffnessMatrix(i + 1, j + 1)));
                        else
                            m_tensor[row][col] += stiffnessMatrix(i + 1, j + 1);
                    }
                }
            }
        }
    }


    template<class Discretization, class IntType>  // COURT added this debug subroutine to isolate Release only crash of physics thread
    template<int elementNodesN>
    inline void SchurSolver<Discretization, IntType>::
        accumToTensor_debug(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix, const std::array<IndexType, elementNodesN>& elementIndex) {
        using IteratorType = Iterator<NodeArrayType>;
        for (int i = 0; i < elementNodesN; i++) {
            int row = IteratorType::at(m_numbering, elementIndex[i]);
            if (row >= 0) {
                for (int j = 0; j < elementNodesN; j++) {
                    int col = IteratorType::at(m_numbering, elementIndex[j]);
                    if (col >= row) {
                        if (m_tensor[row].find(col) == m_tensor[row].end())
                            m_tensor[row].insert(std::pair<int, T>(col, stiffnessMatrix(i + 1, j + 1)));
                        else
                            m_tensor[row][col] += stiffnessMatrix(i + 1, j + 1);
                    }
                }
            }
        }
    }



    template<class Discretization, class IntType>
    template<int elementNodesN>
    inline void SchurSolver<Discretization, IntType>::
        updateTensor(
           const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix,
          const std::array<IndexType, elementNodesN>& elementIndex) {
        IntType& n = m_pardiso.n;
        using IteratorType = Iterator<NodeArrayType>;
        if (schurSize) {
            for (int i = 0; i < elementNodesN; i++) {
                int row = IteratorType::at(m_numbering, elementIndex[i]);
                if (row < 0)
                    continue;

                for (int j = 0; j < elementNodesN; j++) {
                    int col = IteratorType::at(m_numbering, elementIndex[j]);
                    if (col < row)
                        continue;

                    assert(row >= n - schurSize);
                    const T delta = stiffnessMatrix(i + 1, j + 1);
                    const IntType lr = row - n + schurSize;
                    const IntType lc = col - n + schurSize;
                    // Mirror onto both triangles: LAPACK potrf reads one
                    // triangle, but the Fortran column-major interface against
                    // this row-major storage flips which bytes are read, so
                    // keeping the block fully symmetric makes the factorization
                    // invariant under that choice.
                    const double delta_d = static_cast<double>(delta);
                    m_pardiso.schur_d[lr * schurSize + lc] -= delta_d;
                    if (lr != lc) {
                        m_pardiso.schur_d[lc * schurSize + lr] -= delta_d;
                    }
                }
            }
        }
        else {
            for (int i = 0; i < elementNodesN; i++) {
                int row = IteratorType::at(m_numbering, elementIndex[i]);
                if (row < 0)
                    continue;

                for (int j = 0; j < elementNodesN; j++) {
                    int col = IteratorType::at(m_numbering, elementIndex[j]);
                    if (col < row)
                        continue;

                    int index = -1;
                    for (int k = m_pardiso.rowIndex[row]; k < m_pardiso.rowIndex[row + 1]; k++) {
                        if (m_pardiso.column[k] == col) {
                            index = k;
                            break;
                        }
                    }

                    if (index == -1) {
                        std::cerr << "cannot find the entry" << std::endl;
                        exit(1);
                    }
                    else
                        // stiffnessMatrix is negative definite
                        m_pardiso.value[index] -= stiffnessMatrix(i + 1, j + 1);
                }
            }
        }
    }

    template<class Discretization, class IntType>
    template<int elementNodesN>
    void SchurSolver<Discretization, IntType>::accumToPardiso(const PhysBAM::MATRIX_MXN<T>& stiffnessMatrix, const std::array<IndexType, elementNodesN>& elementIndex)
    {
        // need to check if repeated entry matters or not
        using IteratorType = Iterator<NodeArrayType>;
        for (int i = 0; i < elementNodesN; i++) {
            int row = IteratorType::at(m_numbering, elementIndex[i]);
            if (row >= 0) {
                for (int j = 0; j < elementNodesN; j++) {
                    int col = IteratorType::at(m_numbering, elementIndex[j]);
                    if (col >= row) {
                        bool found = false; // for debugging purposes
                        // maybe binary search here?
                        for (IntType jj = m_pardiso.rowIndex[row]; jj < m_pardiso.rowIndex[row + 1]; jj++)
                            if (col == m_pardiso.column[jj]) {
                                found = true;
                                m_pardiso.value[jj] -= stiffnessMatrix(i + 1, j + 1);
                            }
                        if (!found) {
                            std::cout << stiffnessMatrix << std::endl;
                            std::cout << "index = [ ";
                            for (int ii = 0; ii < elementNodesN; ii++)
                                std::cout << elementIndex[ii] << " ";
                            std::cout << "]" << std::endl;
                            std::cout << "row = " << row << " col = " << col << std::endl;
                            throw std::logic_error("entry (" + std::to_string(row) + " , " + std::to_string(col) + ") not found");
                        }
                    }
                }
            }
        }

    }

    template<class Discretization, class IntType>
    inline void SchurSolver<Discretization, IntType>::
        updatePardiso(const std::vector<Constraint>& collisionConstraints,
             const std::vector<CollisionSuture>& collisionSutures) {
        // When the collision constraint set is identical to the previous frame,
        // the work below would produce a byte-identical Schur factor. Compare an
        // FNV-1a fingerprint of the set and short-circuit on a match -- the
        // cached factor in schur_d is still valid.
        auto fnv64 = [](uint64_t h, const void* data, size_t bytes) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i) {
                h ^= static_cast<uint64_t>(p[i]);
                h *= 1099511628211ull;
            }
            return h;
        };
        uint64_t fp = 0;
        if (schurSize > 0) {
            fp = 1469598103934665603ull;
            const size_t nc = collisionConstraints.size();
            fp = fnv64(fp, &nc, sizeof(nc));
            for (size_t c = 0; c < nc; ++c) {
                const auto& cc = collisionConstraints[c];
                fp = fnv64(fp, cc.m_elementIndex.data(),
                           sizeof(cc.m_elementIndex[0]) * cc.m_elementIndex.size());
                fp = fnv64(fp, &cc.m_stiffness, sizeof(cc.m_stiffness));
                fp = fnv64(fp, cc.m_weights.data(),
                           sizeof(cc.m_weights[0]) * cc.m_weights.size());
            }
            const size_t ns = collisionSutures.size();
            fp = fnv64(fp, &ns, sizeof(ns));
            for (size_t s = 0; s < ns; ++s) {
                const auto& cs = collisionSutures[s];
                fp = fnv64(fp, cs.m_elementIndex1.data(),
                           sizeof(cs.m_elementIndex1[0]) * cs.m_elementIndex1.size());
                fp = fnv64(fp, cs.m_elementIndex2.data(),
                           sizeof(cs.m_elementIndex2[0]) * cs.m_elementIndex2.size());
                fp = fnv64(fp, &cs.m_stiffness, sizeof(cs.m_stiffness));
                fp = fnv64(fp, cs.m_weights1.data(),
                           sizeof(cs.m_weights1[0]) * cs.m_weights1.size());
                fp = fnv64(fp, cs.m_weights2.data(),
                           sizeof(cs.m_weights2[0]) * cs.m_weights2.size());
            }
            if (m_collision_fingerprint_valid && fp == m_last_collision_fingerprint) {
                return;
            }
        }

        if (schurSize) {
            if (m_originalValue_d && m_pardiso.schur_d) {
                std::memcpy(m_pardiso.schur_d, m_originalValue_d,
                            static_cast<size_t>(schurSize) *
                            static_cast<size_t>(schurSize) * sizeof(double));
            }

            for (int c = 0; c < collisionConstraints.size(); c++) {
                auto& constraint = collisionConstraints[c];
                if (constraint.m_stiffness != 0) {
                    MATRIX_MXN<T> stiffnessMatrix;
                    DiscretizationType::computeConstraintTensor(stiffnessMatrix, constraint);
                    updateTensor<elementNodes>(stiffnessMatrix, constraint.m_elementIndex);
                }
            }

            for (int c = 0; c < collisionSutures.size(); c++)
            if (collisionSutures[c].m_stiffness) {
                MATRIX_MXN<T> stiffnessMatrix;
                std::array<IndexType, elementNodes * 2> elementIndex;
                DiscretizationType::computeCollisionSutureTensor(stiffnessMatrix, elementIndex, collisionSutures[c]);
                updateTensor<elementNodes * 2>(stiffnessMatrix,
                    elementIndex);
            }

            m_pardiso.factSchur();
            m_last_collision_fingerprint = fp;
            m_collision_fingerprint_valid = true;
        }
        else {
            m_pardiso.factorize();
        }
    }

    template<class Discretization, class IntType>
    inline void SchurSolver<Discretization, IntType>::
        computeTensor(
            const std::vector<ElementType>& elements,
            const std::vector<GradientMatrixType>& gradients,
            const std::vector<T>& restVol, const T mu,
            const std::vector<Suture>& sutures,
            const std::vector<InternodeConstraint>& microNodes
        ) {
        for (int e = 0; e < elements.size(); e++) {
            MATRIX_MXN<T> stiffnessMatrix;
            DiscretizationType::computeElementTensor(stiffnessMatrix, gradients[e], -2 * mu * restVol[e]);
            accumToTensor<elementNodes>(stiffnessMatrix, DiscretizationType::getElementIndex(elements[e]));
        }

        for (int c = 0; c < sutures.size(); c++) {
            MATRIX_MXN<T> stiffnessMatrix;
            std::array<IndexType, elementNodes * 2> elementIndex;
            Suture tmp = sutures[c];
            tmp.m_stiffness = 0;
            DiscretizationType::computeSutureTensor(stiffnessMatrix, elementIndex, tmp);
            accumToTensor<elementNodes * 2>(stiffnessMatrix,
                elementIndex);
        }


        for (const auto& c : microNodes) {
            MATRIX_MXN<T> stiffnessMatrix;
            std::array<IndexType, d + 1> elementIndex;
            auto tmp = c;
            tmp.m_stiffness = 0;
            DiscretizationType::computeMicroNodeTensor(stiffnessMatrix, elementIndex, tmp);
            accumToTensor_debug<d + 1>(stiffnessMatrix,
                elementIndex);  // COURT created this routine to isolate Release only exception in accumToTensor()
        }


    }

    template<class Discretization, class IntType>
    void SchurSolver<Discretization, IntType>::computeTensor(const std::vector<ElementType>& elements,
        const std::vector<GradientMatrixType>& gradients,
        const std::vector<T>& restVol,
        const std::vector<T>& muLow,
        const std::vector<T>& muHigh,
        const std::vector<Suture>& sutures,
        const std::vector<InternodeConstraint>& microNodes)
    {
        // only include things that will change the sparsity of stiffness matrix
        for (int e = 0; e < elements.size(); e++) {
            MATRIX_MXN<T> stiffnessMatrix;
            DiscretizationType::computeElementTensor(stiffnessMatrix, gradients[e], -2 * (muLow[e] + muHigh[e]) * restVol[e]); // computeElementTensor
            accumToTensor<elementNodes>(stiffnessMatrix, DiscretizationType::getElementIndex(elements[e])); // accumToTensor
        }

        // It seems that I don't actually need to compute the matrix here, just need the sparsity
        for (int c = 0; c < sutures.size(); c++) {
            MATRIX_MXN<T> stiffnessMatrix;
            std::array<IndexType, elementNodes * 2> elementIndex;
            Suture tmp = sutures[c];
            tmp.m_stiffness = 0;
            DiscretizationType::computeSutureTensor(stiffnessMatrix, elementIndex, tmp);
            accumToTensor<elementNodes * 2>(stiffnessMatrix,
                elementIndex);
        }

        for (const auto& c:microNodes) {
            MATRIX_MXN<T> stiffnessMatrix;
            std::array<IndexType, d+1> elementIndex;
            auto tmp = c;
            tmp.m_stiffness = 0;
            DiscretizationType::computeMicroNodeTensor(stiffnessMatrix, elementIndex, tmp);
            accumToTensor_debug<d+1>(stiffnessMatrix,
                elementIndex);
        }

    }


    template<class Discretization, class IntType>
    inline void SchurSolver<Discretization, IntType>::initializePardiso(
        const std::vector<Constraint>& constraints,
        const std::vector<Suture>& sutures,
        const std::vector<Constraint>& fakeSutures,
        const std::vector<InternodeConstraint>& microNodes
    ) {

        IntType nnz = 0;
        for (int i = 0; i < m_tensor.size(); i++)
            nnz += (IntType)m_tensor[i].size();

        std::cout << "nnz = " << nnz << std::endl;
        m_pardiso.initialize((IntType)m_tensor.size(), nnz, schurSize);

        m_pardiso.rowIndex[0] = 0;
        for (int i = 0; i < m_pardiso.n; i++)
            m_pardiso.rowIndex[i + 1] = m_pardiso.rowIndex[i] + (IntType)m_tensor[i].size();

        size_t idx = 0;
        for (const auto& r : m_tensor)
            for (const auto& e : r) {
                m_pardiso.column[idx] = e.first;
                idx++;
            }
        if (idx != static_cast<size_t>(m_pardiso.rowIndex[m_pardiso.n])) {
            throw std::runtime_error("CSR assembly mismatch: nnz count does not equal rowIndex[n]");
        }

        m_pardiso.symbolicFact();

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

    template<class Discretization, class IntType>
    void SchurSolver<Discretization, IntType>::factPardiso(const std::vector<Constraint>& constraints, const std::vector<Suture>& sutures, const std::vector<Constraint>& fakeSutures, const std::vector<InternodeConstraint>& microNodes)
    {
        size_t idx = 0;
        for (const auto& r : m_tensor)
            for (const auto& e : r) {
                m_pardiso.value[idx] = -e.second;
                idx++;
            }

        for (int c = 0; c < constraints.size(); c++)
            if (constraints[c].m_stiffness != 0) {
                MATRIX_MXN<T> stiffnessMatrix;
                DiscretizationType::computeConstraintTensor(stiffnessMatrix, constraints[c]);
                accumToPardiso<elementNodes>(stiffnessMatrix,
                    constraints[c].m_elementIndex);
            }

        for (int c = 0; c < fakeSutures.size(); c++)
            if (fakeSutures[c].m_stiffness != 0) {
                MATRIX_MXN<T> stiffnessMatrix;
                DiscretizationType::computeConstraintTensor(stiffnessMatrix, fakeSutures[c]);
                accumToPardiso<elementNodes>(stiffnessMatrix,
                    fakeSutures[c].m_elementIndex);
            }

        for (int c = 0; c < sutures.size(); c++)
            if (sutures[c].m_stiffness != 0) {
                MATRIX_MXN<T> stiffnessMatrix;
                std::array<IndexType, elementNodes * 2> elementIndex;
                DiscretizationType::computeSutureTensor(stiffnessMatrix, elementIndex, sutures[c]);
                accumToPardiso<elementNodes * 2>(stiffnessMatrix,
                    elementIndex);
            }

        for (const auto& c:microNodes)
            if (c.m_stiffness != 0) {
                MATRIX_MXN<T> stiffnessMatrix;
                std::array<IndexType, d+1> elementIndex;
                DiscretizationType::computeMicroNodeTensor(stiffnessMatrix, elementIndex, c);
                accumToPardiso<d+1>(stiffnessMatrix,
                    elementIndex);
            }

        m_pardiso.numericFact();
    }

}

namespace PhysBAM {
    template struct SchurSolver<TetrahedralDiscretization<std::vector<VECTOR<float, 3>>>, int>;
}
