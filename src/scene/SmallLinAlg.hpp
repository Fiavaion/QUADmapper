#pragma once

// Small dense linear algebra for the projector calibration solver (ADR-052 §4).
// Double precision, sized for problems of twelve unknowns or fewer. Internal to the
// scene module; not a general-purpose matrix library.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace quadmapper::linalg {

// Row-major dense matrix.
struct Matrix {
    int rows = 0;
    int cols = 0;
    std::vector<double> data;

    Matrix() = default;
    Matrix(int r, int c) : rows(r), cols(c), data(static_cast<size_t>(r) * c, 0.0) {}

    double& operator()(int r, int c) { return data[static_cast<size_t>(r) * cols + c]; }
    double operator()(int r, int c) const { return data[static_cast<size_t>(r) * cols + c]; }

    static Matrix identity(int n) {
        Matrix m(n, n);
        for (int i = 0; i < n; ++i) m(i, i) = 1.0;
        return m;
    }
};

// Eigen decomposition of a symmetric matrix by cyclic Jacobi rotations. Eigenvalues come
// back ascending; column i of `vectors` is the unit eigenvector of values[i]. Returns
// false if the sweeps did not converge (the output is then the best estimate so far).
inline bool jacobiEigen(const Matrix& a, std::vector<double>& values, Matrix& vectors,
                        int maxSweeps = 100) {
    const int n = a.rows;
    Matrix m = a;
    Matrix v = Matrix::identity(n);

    double total = 0.0;
    for (double x : m.data) total += x * x;

    bool converged = false;
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += m(p, q) * m(p, q);
        if (off <= 1e-30 * total || off == 0.0) {
            converged = true;
            break;
        }
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = m(p, q);
                if (apq == 0.0) continue;
                // Rotation that zeroes m(p, q) (Numerical Recipes §11.1).
                const double theta = (m(q, q) - m(p, p)) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double mkp = m(k, p), mkq = m(k, q);
                    m(k, p) = c * mkp - s * mkq;
                    m(k, q) = s * mkp + c * mkq;
                }
                for (int k = 0; k < n; ++k) {
                    const double mpk = m(p, k), mqk = m(q, k);
                    m(p, k) = c * mpk - s * mqk;
                    m(q, k) = s * mpk + c * mqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = v(k, p), vkq = v(k, q);
                    v(k, p) = c * vkp - s * vkq;
                    v(k, q) = s * vkp + c * vkq;
                }
            }
        }
    }

    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int i, int j) { return m(i, i) < m(j, j); });
    values.assign(static_cast<size_t>(n), 0.0);
    vectors = Matrix(n, n);
    for (int i = 0; i < n; ++i) {
        values[static_cast<size_t>(i)] = m(order[static_cast<size_t>(i)], order[static_cast<size_t>(i)]);
        for (int k = 0; k < n; ++k) vectors(k, i) = v(k, order[static_cast<size_t>(i)]);
    }
    return converged;
}

// Solves a * x = b by Gaussian elimination with partial pivoting. Returns false when a
// is singular to working precision.
inline bool solveLinear(Matrix a, std::vector<double> b, std::vector<double>& x) {
    const int n = a.rows;
    double scale = 0.0;
    for (double e : a.data) scale = std::max(scale, std::abs(e));
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;

    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs(a(r, col)) > std::abs(a(pivot, col))) pivot = r;
        if (std::abs(a(pivot, col)) <= 1e-14 * scale) return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c) std::swap(a(col, c), a(pivot, c));
            std::swap(b[static_cast<size_t>(col)], b[static_cast<size_t>(pivot)]);
        }
        for (int r = col + 1; r < n; ++r) {
            const double f = a(r, col) / a(col, col);
            if (f == 0.0) continue;
            for (int c = col; c < n; ++c) a(r, c) -= f * a(col, c);
            b[static_cast<size_t>(r)] -= f * b[static_cast<size_t>(col)];
        }
    }
    x.assign(static_cast<size_t>(n), 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double sum = b[static_cast<size_t>(r)];
        for (int c = r + 1; c < n; ++c) sum -= a(r, c) * x[static_cast<size_t>(c)];
        x[static_cast<size_t>(r)] = sum / a(r, r);
    }
    return true;
}

// glm matrices are column-major: m[col][row]. These helpers read and write by (row, col).
inline double at(const glm::dmat3& m, int row, int col) { return m[col][row]; }

// RQ decomposition of a 3x3 matrix by Givens rotations (Hartley & Zisserman A4.1.1):
// m = upper * rotation, with `upper` upper-triangular with a non-negative diagonal and
// `rotation` orthogonal. det(rotation) = sign(det(m)).
inline void rq3(const glm::dmat3& m, glm::dmat3& upper, glm::dmat3& rotation) {
    glm::dmat3 k = m;
    glm::dmat3 q(1.0);
    // Right-multiplies by a rotation in the (j, c) column plane that zeroes k(row, j).
    const auto zero = [&](int row, int j, int c) {
        const double a = at(k, row, j);
        const double b = at(k, row, c);
        const double r = std::hypot(a, b);
        if (r == 0.0) return;
        const double cs = b / r;
        const double sn = -a / r;
        glm::dmat3 g(1.0);
        g[j][j] = cs;
        g[j][c] = sn;   // row c, column j
        g[c][j] = -sn;  // row j, column c
        g[c][c] = cs;
        k = k * g;
        q = q * g;
    };
    zero(2, 1, 2);
    zero(2, 0, 2);
    zero(1, 0, 1);

    // k = m * q is now upper-triangular; make its diagonal non-negative.
    glm::dmat3 d(1.0);
    for (int i = 0; i < 3; ++i)
        if (at(k, i, i) < 0.0) d[i][i] = -1.0;
    upper = k * d;
    rotation = d * glm::transpose(q);
    for (int col = 0; col < 3; ++col)
        for (int row = col + 1; row < 3; ++row) upper[col][row] = 0.0;
}

// The skew-symmetric matrix [v]x, so that skew(v) * w = cross(v, w).
inline glm::dmat3 skew(const glm::dvec3& v) {
    return glm::dmat3(glm::dvec3(0.0, v.z, -v.y), glm::dvec3(-v.z, 0.0, v.x),
                      glm::dvec3(v.y, -v.x, 0.0));
}

// Rodrigues rotation vector (axis * angle in radians) -> rotation matrix.
inline glm::dmat3 rodriguesToMatrix(const glm::dvec3& r) {
    const double theta = glm::length(r);
    if (theta < 1e-12) return glm::dmat3(1.0) + skew(r);
    const glm::dmat3 k = skew(r / theta);
    return glm::dmat3(1.0) + std::sin(theta) * k + (1.0 - std::cos(theta)) * (k * k);
}

// Rotation matrix -> Rodrigues vector, stable for all angles in [0, pi].
inline glm::dvec3 matrixToRodrigues(const glm::dmat3& m) {
    const glm::dvec3 v(at(m, 2, 1) - at(m, 1, 2), at(m, 0, 2) - at(m, 2, 0),
                       at(m, 1, 0) - at(m, 0, 1));
    const double sinTheta = 0.5 * glm::length(v);
    const double cosTheta =
        std::clamp(0.5 * (at(m, 0, 0) + at(m, 1, 1) + at(m, 2, 2) - 1.0), -1.0, 1.0);
    const double theta = std::atan2(sinTheta, cosTheta);
    if (sinTheta > 1e-6) return v * (theta / (2.0 * sinTheta));
    if (cosTheta > 0.0) return 0.5 * v;  // near identity: first order is exact enough

    // Near pi: (m + I) / 2 = axis * axis^T. Take the column with the largest diagonal.
    const glm::dmat3 b = 0.5 * (m + glm::dmat3(1.0));
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (at(b, i, i) > at(b, best, best)) best = i;
    glm::dvec3 axis = b[best] / std::sqrt(std::max(at(b, best, best), 1e-300));
    axis = glm::normalize(axis);
    if (glm::dot(axis, v) < 0.0) axis = -axis;
    return axis * theta;
}

// The orthogonal matrix nearest to m (polar decomposition, m * (m^T m)^-1/2). Returns
// false when m is singular. The result keeps the sign of det(m).
inline bool nearestOrthogonal(const glm::dmat3& m, glm::dmat3& out) {
    const glm::dmat3 s = glm::transpose(m) * m;
    Matrix a(3, 3);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) a(r, c) = at(s, r, c);
    std::vector<double> values;
    Matrix vectors;
    jacobiEigen(a, values, vectors);
    if (!(values[0] > 1e-12 * values[2])) return false;
    glm::dmat3 invSqrt(0.0);
    for (int i = 0; i < 3; ++i) {
        const glm::dvec3 e(vectors(0, i), vectors(1, i), vectors(2, i));
        invSqrt += glm::outerProduct(e, e) / std::sqrt(values[static_cast<size_t>(i)]);
    }
    out = m * invSqrt;
    return true;
}

}  // namespace quadmapper::linalg
