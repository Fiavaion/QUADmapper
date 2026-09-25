#include "ProjectorCalibration.hpp"

#include "SmallLinAlg.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>

namespace quadmapper {

namespace {

using linalg::Matrix;

// Shape tests on the centred world points, as ratios of their RMS extents along the
// principal axes (sigma1 >= sigma2 >= sigma3). Points within 2% of a plane (sigma3/sigma1)
// do not constrain the lens: the DLT becomes ill-conditioned well before exact
// coplanarity, so they are treated as coplanar. Points within 1% of a line (sigma2/sigma1)
// cannot fix the roll about that line even with a known lens.
constexpr double COPLANAR_RATIO = 0.02;
constexpr double COLLINEAR_RATIO = 0.01;

constexpr int MAX_LM_ITERATIONS = 200;

// Outlier handling (see calibrateProjector): a least-squares fit whose worst pair is off
// by more than OUTLIER_MIN_PX is checked for a single mismatched pair. A pair is taken as
// the outlier when, fitted without it, it is off by more than OUTLIER_FACTOR times the RMS
// of the rest; the final fit then uses a Cauchy loss with a scale of CAUCHY_SCALE_FACTOR
// times that RMS (at least CAUCHY_MIN_PX).
constexpr double OUTLIER_MIN_PX = 3.0;
constexpr double OUTLIER_FACTOR = 5.0;
constexpr double CAUCHY_SCALE_FACTOR = 3.0;
constexpr double CAUCHY_MIN_PX = 1.0;

struct Pair {
    glm::dvec3 world;
    glm::dvec2 pixel;
};

struct Lens {
    double fx, fy, cx, cy;
};

struct Pose {
    glm::dmat3 rotation{1.0};  // world -> camera
    glm::dvec3 translation{0.0};
};

struct Spread {
    glm::dvec3 centroid{0.0};
    glm::dvec3 axes[3];  // principal axes, largest extent first
    double sigma[3] = {0.0, 0.0, 0.0};
};

Spread analyseSpread(const std::vector<Pair>& pairs) {
    Spread s;
    for (const Pair& p : pairs) s.centroid += p.world;
    s.centroid /= static_cast<double>(pairs.size());
    Matrix cov(3, 3);
    for (const Pair& p : pairs) {
        const glm::dvec3 d = p.world - s.centroid;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) cov(r, c) += d[r] * d[c];
    }
    for (double& x : cov.data) x /= static_cast<double>(pairs.size());
    std::vector<double> values;
    Matrix vectors;
    linalg::jacobiEigen(cov, values, vectors);
    for (int i = 0; i < 3; ++i) {
        const int src = 2 - i;  // eigenvalues are ascending
        s.sigma[i] = std::sqrt(std::max(values[static_cast<size_t>(src)], 0.0));
        s.axes[i] = glm::dvec3(vectors(0, src), vectors(1, src), vectors(2, src));
    }
    return s;
}

// Ratio of the smaller to the larger RMS extent of the pixels (0 = all on one line).
double pixelSpreadRatio(const std::vector<Pair>& pairs, double& largest) {
    glm::dvec2 mean(0.0);
    for (const Pair& p : pairs) mean += p.pixel;
    mean /= static_cast<double>(pairs.size());
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (const Pair& p : pairs) {
        const glm::dvec2 d = p.pixel - mean;
        sxx += d.x * d.x;
        sxy += d.x * d.y;
        syy += d.y * d.y;
    }
    const double half = 0.5 * (sxx + syy);
    const double root = std::sqrt(0.25 * (sxx - syy) * (sxx - syy) + sxy * sxy);
    largest = std::sqrt(std::max(half + root, 0.0) / static_cast<double>(pairs.size()));
    const double smallest = std::sqrt(std::max(half - root, 0.0) / static_cast<double>(pairs.size()));
    return largest > 0.0 ? smallest / largest : 0.0;
}

// Hartley normalisation: translate to the centroid and scale so the mean distance from
// it is sqrt(dim). Returns the scale; `centre` receives the centroid.
template <typename V>
double hartleyScale(const std::vector<V>& pts, V& centre) {
    centre = V(0.0);
    for (const V& p : pts) centre += p;
    centre /= static_cast<double>(pts.size());
    double meanDist = 0.0;
    for (const V& p : pts) meanDist += glm::length(p - centre);
    meanDist /= static_cast<double>(pts.size());
    return meanDist > 0.0 ? std::sqrt(static_cast<double>(V::length())) / meanDist : 0.0;
}

// Unit null vector of A (via the normal matrix A^T A). Fails when the smallest two
// eigenvalues are both negligible, i.e. the solution is not unique.
bool nullVector(const Matrix& normal, std::vector<double>& out) {
    std::vector<double> values;
    Matrix vectors;
    linalg::jacobiEigen(normal, values, vectors);
    const double largest = values.back();
    if (!(largest > 0.0) || values[1] <= 1e-12 * largest) return false;
    out.resize(static_cast<size_t>(normal.rows));
    for (int i = 0; i < normal.rows; ++i) out[static_cast<size_t>(i)] = vectors(i, 0);
    return true;
}

void accumulate(Matrix& normal, const double* row) {
    for (int r = 0; r < normal.rows; ++r) {
        if (row[r] == 0.0) continue;
        for (int c = 0; c < normal.cols; ++c) normal(r, c) += row[r] * row[c];
    }
}

// Normalised DLT for the 3x4 projection matrix. Output: m (left 3x3) and p4 (last column).
bool solveDlt(const std::vector<Pair>& pairs, glm::dmat3& m, glm::dvec3& p4) {
    std::vector<glm::dvec3> world;
    std::vector<glm::dvec2> pixel;
    for (const Pair& p : pairs) {
        world.push_back(p.world);
        pixel.push_back(p.pixel);
    }
    glm::dvec3 wc;
    glm::dvec2 pc;
    const double ws = hartleyScale(world, wc);
    const double ps = hartleyScale(pixel, pc);
    if (ws <= 0.0 || ps <= 0.0) return false;

    Matrix normal(12, 12);
    for (size_t i = 0; i < pairs.size(); ++i) {
        const glm::dvec3 x = ws * (world[i] - wc);
        const glm::dvec2 u = ps * (pixel[i] - pc);
        const double r1[12] = {x.x, x.y, x.z, 1, 0, 0, 0, 0, -u.x * x.x, -u.x * x.y, -u.x * x.z, -u.x};
        const double r2[12] = {0, 0, 0, 0, x.x, x.y, x.z, 1, -u.y * x.x, -u.y * x.y, -u.y * x.z, -u.y};
        accumulate(normal, r1);
        accumulate(normal, r2);
    }
    std::vector<double> p;
    if (!nullVector(normal, p)) return false;

    // Undo the normalisation: P = Tpix^-1 * Pn * Tworld.
    double pn[3][4];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) pn[r][c] = p[static_cast<size_t>(r * 4 + c)];
    const double tpixInv[3][3] = {{1.0 / ps, 0, pc.x}, {0, 1.0 / ps, pc.y}, {0, 0, 1}};
    const double tworld[4][4] = {
        {ws, 0, 0, -ws * wc.x}, {0, ws, 0, -ws * wc.y}, {0, 0, ws, -ws * wc.z}, {0, 0, 0, 1}};
    double tmp[3][4] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 3; ++k) tmp[r][c] += tpixInv[r][k] * pn[k][c];
    double full[3][4] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) full[r][c] += tmp[r][k] * tworld[k][c];

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m[c][r] = full[r][c];
        p4[r] = full[r][3];
    }
    // P is known only up to scale. With det(M) > 0 the scale is positive, which is what
    // puts points in front of the camera at positive depth.
    const double det = glm::determinant(m);
    if (!std::isfinite(det) || std::abs(det) < 1e-300) return false;
    if (det < 0.0) {
        m = -m;
        p4 = -p4;
    }
    return true;
}

// K, R, t from a DLT result (m has positive determinant).
bool decomposeProjection(const glm::dmat3& m, const glm::dvec3& p4, Lens& lens, Pose& pose) {
    glm::dmat3 k, r;
    linalg::rq3(m, k, r);
    const double scale = k[2][2];
    if (!(scale > 0.0) || !(k[0][0] > 0.0) || !(k[1][1] > 0.0)) return false;
    pose.rotation = r;
    pose.translation = glm::inverse(k) * p4;
    lens = {k[0][0] / scale, k[1][1] / scale, k[2][0] / scale, k[2][1] / scale};
    return glm::determinant(r) > 0.0;
}

// The pose alone from a DLT result and a known lens.
bool poseFromDlt(const glm::dmat3& m, const glm::dvec3& p4, const Lens& lens, Pose& pose) {
    glm::dmat3 kInv(1.0);
    kInv[0][0] = 1.0 / lens.fx;
    kInv[1][1] = 1.0 / lens.fy;
    kInv[2][0] = -lens.cx / lens.fx;
    kInv[2][1] = -lens.cy / lens.fy;
    const glm::dmat3 a = kInv * m;
    const double det = glm::determinant(a);
    if (!(det > 0.0)) return false;
    const double s = std::cbrt(det);
    if (!linalg::nearestOrthogonal(a / s, pose.rotation)) return false;
    pose.translation = (kInv * p4) / s;
    return true;
}

// The translation that best fits the pairs for a given rotation and lens. Linear: each
// pair gives (R X + t).x - x (R X + t).z = 0 and the same for y, x and y being the
// normalised image coordinates.
bool translationForRotation(const std::vector<Pair>& pairs, const Lens& lens,
                            const glm::dmat3& rotation, glm::dvec3& translation) {
    Matrix normal(3, 3);
    std::vector<double> rhs(3, 0.0);
    for (const Pair& p : pairs) {
        const glm::dvec3 y = rotation * p.world;
        const double xn = (p.pixel.x - lens.cx) / lens.fx;
        const double yn = (p.pixel.y - lens.cy) / lens.fy;
        const double rows[2][4] = {{1.0, 0.0, -xn, xn * y.z - y.x}, {0.0, 1.0, -yn, yn * y.z - y.y}};
        for (const auto& row : rows) {
            for (int r = 0; r < 3; ++r) {
                rhs[static_cast<size_t>(r)] += row[r] * row[3];
                for (int c = 0; c < 3; ++c) normal(r, c) += row[r] * row[c];
            }
        }
    }
    std::vector<double> t;
    if (!linalg::solveLinear(normal, rhs, t)) return false;
    translation = glm::dvec3(t[0], t[1], t[2]);
    return true;
}

// Starting poses from the homography between the best-fit plane of the world points and
// the normalised image (Zhang). Exact for coplanar points; a guess otherwise. A plane seen
// through noise has two nearby poses that fit about equally well (the normal tilted
// either way about the line of sight), so both are returned for refinement.
std::vector<Pose> posesFromPlane(const std::vector<Pair>& pairs, const Spread& spread,
                                 const Lens& lens) {
    const glm::dvec3 e1 = spread.axes[0];
    const glm::dvec3 e2 = spread.axes[1];
    const glm::dmat3 basis(e1, e2, glm::cross(e1, e2));  // columns; a proper rotation

    std::vector<glm::dvec2> src, dst;
    for (const Pair& p : pairs) {
        const glm::dvec3 d = p.world - spread.centroid;
        src.emplace_back(glm::dot(d, e1), glm::dot(d, e2));
        dst.emplace_back((p.pixel.x - lens.cx) / lens.fx, (p.pixel.y - lens.cy) / lens.fy);
    }
    glm::dvec2 sc, dc;
    const double ss = hartleyScale(src, sc);
    const double ds = hartleyScale(dst, dc);
    if (ss <= 0.0 || ds <= 0.0) return {};

    Matrix normal(9, 9);
    for (size_t i = 0; i < src.size(); ++i) {
        const glm::dvec2 x = ss * (src[i] - sc);
        const glm::dvec2 u = ds * (dst[i] - dc);
        const double r1[9] = {x.x, x.y, 1, 0, 0, 0, -u.x * x.x, -u.x * x.y, -u.x};
        const double r2[9] = {0, 0, 0, x.x, x.y, 1, -u.y * x.x, -u.y * x.y, -u.y};
        accumulate(normal, r1);
        accumulate(normal, r2);
    }
    std::vector<double> h;
    if (!nullVector(normal, h)) return {};

    glm::dmat3 hn;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) hn[c][r] = h[static_cast<size_t>(r * 3 + c)];
    glm::dmat3 tdstInv(1.0), tsrc(1.0);
    tdstInv[0][0] = tdstInv[1][1] = 1.0 / ds;
    tdstInv[2][0] = dc.x;
    tdstInv[2][1] = dc.y;
    tsrc[0][0] = tsrc[1][1] = ss;
    tsrc[2][0] = -ss * sc.x;
    tsrc[2][1] = -ss * sc.y;
    glm::dmat3 hm = tdstInv * hn * tsrc;  // columns: lambda * [r1 r2 t]

    const double lambda = 0.5 * (glm::length(hm[0]) + glm::length(hm[1]));
    if (!(lambda > 0.0)) return {};
    if (hm[2].z < 0.0) hm = -hm;  // the plane's centre must be in front of the lens
    const glm::dvec3 r1 = hm[0] / lambda;
    const glm::dvec3 r2 = hm[1] / lambda;
    glm::dmat3 planeRotation;
    if (!linalg::nearestOrthogonal(glm::dmat3(r1, r2, glm::cross(r1, r2)), planeRotation))
        return {};

    // The mirrored tilt: reflect the plane normal about the ray to the plane's centre.
    const glm::dvec3 normalDir = planeRotation[2];
    const glm::dvec3 ray = glm::normalize(hm[2]);
    const glm::dvec3 mirrored = 2.0 * glm::dot(normalDir, ray) * ray - normalDir;
    const glm::dvec3 axis = glm::cross(normalDir, mirrored);
    const double angle = std::atan2(glm::length(axis), glm::dot(normalDir, mirrored));

    std::vector<Pose> out;
    for (const glm::dmat3& rotation :
         {planeRotation, linalg::rodriguesToMatrix(glm::length(axis) > 1e-12
                                                       ? glm::normalize(axis) * angle
                                                       : glm::dvec3(0.0)) *
                             planeRotation}) {
        // camera = rotation * basis^T * world + t
        Pose pose;
        pose.rotation = rotation * glm::transpose(basis);
        if (translationForRotation(pairs, lens, pose.rotation, pose.translation))
            out.push_back(pose);
    }
    return out;
}

// A starting pose for points at different depths by POSIT (DeMenthon & Davis 1995):
// scaled-orthographic pose, then iterative correction of the perspective terms. Needs
// four or more points that are not coplanar.
bool poseFromPosit(const std::vector<Pair>& pairs, const Spread& spread, const Lens& lens,
                   Pose& pose) {
    // Reference point: the one nearest the centroid.
    size_t ref = 0;
    for (size_t i = 1; i < pairs.size(); ++i)
        if (glm::length(pairs[i].world - spread.centroid) <
            glm::length(pairs[ref].world - spread.centroid))
            ref = i;
    const glm::dvec3 origin = pairs[ref].world;
    std::vector<glm::dvec3> arm;
    std::vector<glm::dvec2> image;
    for (const Pair& p : pairs) {
        arm.push_back(p.world - origin);
        image.emplace_back((p.pixel.x - lens.cx) / lens.fx, (p.pixel.y - lens.cy) / lens.fy);
    }
    // Pseudo-inverse of the arm matrix, through its 3x3 normal matrix.
    glm::dmat3 ata(0.0);
    for (const glm::dvec3& a : arm) ata += glm::outerProduct(a, a);
    if (!(std::abs(glm::determinant(ata)) > 0.0)) return false;
    const glm::dmat3 ataInv = glm::inverse(ata);

    std::vector<double> eps(pairs.size(), 0.0);
    glm::dvec3 i, j, k;
    double z0 = 0.0;
    for (int iter = 0; iter < 30; ++iter) {
        glm::dvec3 bi(0.0), bj(0.0);
        for (size_t n = 0; n < pairs.size(); ++n) {
            bi += arm[n] * (image[n].x * (1.0 + eps[n]) - image[ref].x);
            bj += arm[n] * (image[n].y * (1.0 + eps[n]) - image[ref].y);
        }
        const glm::dvec3 vi = ataInv * bi;
        const glm::dvec3 vj = ataInv * bj;
        const double li = glm::length(vi), lj = glm::length(vj);
        if (!(li > 0.0) || !(lj > 0.0)) return false;
        z0 = 1.0 / std::sqrt(li * lj);
        i = vi / li;
        j = vj / lj;
        k = glm::normalize(glm::cross(i, j));
        for (size_t n = 0; n < pairs.size(); ++n) eps[n] = glm::dot(arm[n], k) / z0;
    }
    glm::dmat3 rows;
    if (!linalg::nearestOrthogonal(glm::transpose(glm::dmat3(i, glm::cross(k, i), k)), rows))
        return false;
    pose.rotation = rows;
    pose.translation = z0 * glm::dvec3(image[ref], 1.0) - rows * origin;
    return std::isfinite(z0) && z0 > 0.0;
}

// IRLS weight of a point whose reprojection distance is `r` pixels under a Cauchy loss of
// scale c (1 = plain least squares). c <= 0 means plain least squares.
double cauchyWeight(double r, double c) {
    return c > 0.0 ? 1.0 / (1.0 + (r / c) * (r / c)) : 1.0;
}

// Sum over points of the squared reprojection distance, or with c > 0 the Cauchy cost
// c^2 log(1 + r^2 / c^2); +inf if any point is not in front of the camera.
double reprojectionCost(const std::vector<Pair>& pairs, const Lens& lens, const Pose& pose,
                        double c = 0.0) {
    double cost = 0.0;
    for (const Pair& p : pairs) {
        const glm::dvec3 pc = pose.rotation * p.world + pose.translation;
        if (!(pc.z > 1e-9)) return std::numeric_limits<double>::infinity();
        const double du = lens.fx * pc.x / pc.z + lens.cx - p.pixel.x;
        const double dv = lens.fy * pc.y / pc.z + lens.cy - p.pixel.y;
        const double sq = du * du + dv * dv;
        cost += c > 0.0 ? c * c * std::log1p(sq / (c * c)) : sq;
    }
    return std::isfinite(cost) ? cost : std::numeric_limits<double>::infinity();
}

// Reprojection distance of every pair, in pixels (the pose must have every point in front).
std::vector<double> residualDistances(const std::vector<Pair>& pairs, const Lens& lens,
                                      const Pose& pose) {
    std::vector<double> out;
    for (const Pair& p : pairs) {
        const glm::dvec3 pc = pose.rotation * p.world + pose.translation;
        out.push_back(std::hypot(lens.fx * pc.x / pc.z + lens.cx - p.pixel.x,
                                 lens.fy * pc.y / pc.z + lens.cy - p.pixel.y));
    }
    return out;
}

// Levenberg-Marquardt on the reprojection error with an analytic Jacobian. Parameters are
// a rotation increment (applied on the left of the current rotation, so there is no
// Rodrigues singularity), the translation and, when solveLens is set, fx, fy, cx, cy.
// With cauchy > 0 it minimises the Cauchy cost of that scale instead, by iteratively
// reweighted least squares. Returns the final cost, +inf when the start has points
// behind the camera.
double refine(const std::vector<Pair>& pairs, Lens& lens, Pose& pose, bool solveLens,
              double cauchy = 0.0) {
    const int np = solveLens ? 10 : 6;
    double cost = reprojectionCost(pairs, lens, pose, cauchy);
    if (!std::isfinite(cost)) return cost;

    double damping = 1e-3;
    for (int iter = 0; iter < MAX_LM_ITERATIONS; ++iter) {
        Matrix jtj(np, np);
        std::vector<double> jtr(static_cast<size_t>(np), 0.0);
        for (const Pair& p : pairs) {
            const glm::dvec3 y = pose.rotation * p.world;
            const glm::dvec3 pc = y + pose.translation;
            const double iz = 1.0 / pc.z;
            const double xn = pc.x * iz, yn = pc.y * iz;
            const double res[2] = {lens.fx * xn + lens.cx - p.pixel.x,
                                   lens.fy * yn + lens.cy - p.pixel.y};
            // d(pixel)/d(pc)
            const glm::dvec3 du(lens.fx * iz, 0.0, -lens.fx * xn * iz);
            const glm::dvec3 dv(0.0, lens.fy * iz, -lens.fy * yn * iz);
            // d(pc)/d(omega) = -[y]x, so d(pixel)/d(omega) = -(dpix/dpc)^T [y]x = cross(y, dpix/dpc)
            const glm::dvec3 duw = glm::cross(y, du);
            const glm::dvec3 dvw = glm::cross(y, dv);
            const double ju[10] = {duw.x, duw.y, duw.z, du.x, du.y, du.z, xn, 0.0, 1.0, 0.0};
            const double jv[10] = {dvw.x, dvw.y, dvw.z, dv.x, dv.y, dv.z, 0.0, yn, 0.0, 1.0};
            const double w = cauchyWeight(std::hypot(res[0], res[1]), cauchy);
            for (int r = 0; r < np; ++r) {
                jtr[static_cast<size_t>(r)] += w * (ju[r] * res[0] + jv[r] * res[1]);
                for (int c = 0; c < np; ++c) jtj(r, c) += w * (ju[r] * ju[c] + jv[r] * jv[c]);
            }
        }

        bool accepted = false;
        while (!accepted && damping < 1e12) {
            Matrix a = jtj;
            for (int i = 0; i < np; ++i) a(i, i) += damping * std::max(jtj(i, i), 1e-12);
            std::vector<double> rhs(jtr.size());
            for (size_t i = 0; i < jtr.size(); ++i) rhs[i] = -jtr[i];
            std::vector<double> step;
            if (!linalg::solveLinear(a, rhs, step)) {
                damping *= 10.0;
                continue;
            }
            Pose trialPose;
            trialPose.rotation =
                linalg::rodriguesToMatrix(glm::dvec3(step[0], step[1], step[2])) * pose.rotation;
            trialPose.translation = pose.translation + glm::dvec3(step[3], step[4], step[5]);
            Lens trialLens = lens;
            if (solveLens) {
                trialLens = {lens.fx + step[6], lens.fy + step[7], lens.cx + step[8], lens.cy + step[9]};
            }
            const double trialCost = reprojectionCost(pairs, trialLens, trialPose, cauchy);
            if (trialCost < cost) {
                const double previous = cost;
                pose = trialPose;
                lens = trialLens;
                cost = trialCost;
                damping = std::max(damping * 0.1, 1e-12);
                accepted = true;
                if (previous - cost <= 1e-12 * previous) return cost;
            } else {
                damping *= 10.0;
            }
        }
        if (!accepted) break;
    }
    return cost;
}

// A mismatched pair drags a least-squares fit towards itself and spreads its error over
// the good pairs, so the per-point errors no longer point at it. When the worst pair is
// well off, find the one pair whose removal lets the rest fit tightly, then refit all
// pairs from that start with a Cauchy loss: the good pairs fit as if the bad one were
// absent, and it keeps its own large error for the user to see. No pair is dropped, and
// clean data never reaches the refit. Returns true when lens/pose were replaced.
// Pairs left after removing one suspect must still over-determine the solve.
size_t minPairsToLocateOutlier(bool solveLens) { return (solveLens ? 8 : 5) + 1; }

bool reweightSingleOutlier(const std::vector<Pair>& pairs, Lens& lens, Pose& pose, bool solveLens) {
    const size_t minRest = minPairsToLocateOutlier(solveLens) - 1;
    const std::vector<double> distances = residualDistances(pairs, lens, pose);
    if (*std::max_element(distances.begin(), distances.end()) <= OUTLIER_MIN_PX ||
        pairs.size() - 1 < minRest)
        return false;

    double bestRestRms = std::numeric_limits<double>::infinity();
    size_t suspect = 0;
    Lens suspectLens = lens;
    Pose suspectPose = pose;
    for (size_t i = 0; i < pairs.size(); ++i) {
        std::vector<Pair> rest = pairs;
        rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));
        Lens l = lens;
        Pose p = pose;
        const double restRms = std::sqrt(refine(rest, l, p, solveLens) / static_cast<double>(rest.size()));
        if (restRms < bestRestRms) {
            bestRestRms = restRms;
            suspect = i;
            suspectLens = l;
            suspectPose = p;
        }
    }
    if (!std::isfinite(bestRestRms)) return false;
    const double suspectError = std::sqrt(reprojectionCost({pairs[suspect]}, suspectLens, suspectPose));
    if (!(suspectError > std::max(OUTLIER_MIN_PX, OUTLIER_FACTOR * bestRestRms))) return false;

    const double scale = std::max(CAUCHY_MIN_PX, CAUCHY_SCALE_FACTOR * bestRestRms);
    if (!std::isfinite(refine(pairs, suspectLens, suspectPose, solveLens, scale))) return false;
    lens = suspectLens;
    pose = suspectPose;
    return true;
}

bool finite(const Lens& lens, const Pose& pose) {
    bool ok = std::isfinite(lens.fx) && std::isfinite(lens.fy) && std::isfinite(lens.cx) &&
              std::isfinite(lens.cy);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) ok = ok && std::isfinite(pose.rotation[c][r]);
    for (int i = 0; i < 3; ++i) ok = ok && std::isfinite(pose.translation[i]);
    return ok;
}

bool finitePoint(const CalibrationPoint& p) {
    return std::isfinite(p.world.x) && std::isfinite(p.world.y) && std::isfinite(p.world.z) &&
           std::isfinite(p.pixel.x) && std::isfinite(p.pixel.y);
}

const char* const COPLANAR_MESSAGE =
    "The points lie on one plane: enter the lens throw ratio (pose-only) or add points at "
    "different depths";
const char* const DEGENERATE_MESSAGE =
    "The point pairs are degenerate and no projector fits them: check for duplicated or "
    "mismatched points";

}  // namespace

Result<CalibrationResult, Error> calibrateProjector(const std::vector<CalibrationPoint>& points,
                                                   const ProjectorIntrinsics& knownIntrinsics,
                                                   CalibrationMode mode) {
    std::vector<Pair> pairs;
    for (const CalibrationPoint& p : points) {
        if (!p.enabled) continue;
        if (!finitePoint(p))
            return Err<CalibrationResult>("A calibration point has an invalid (non-finite) coordinate");
        pairs.push_back({glm::dvec3(p.world), glm::dvec2(p.pixel)});
    }
    const std::string have = " (" + std::to_string(pairs.size()) + " enabled)";
    if (pairs.size() < 4) return Err<CalibrationResult>("Need at least 4 enabled points" + have);
    if (mode == CalibrationMode::Full && pairs.size() < 6)
        return Err<CalibrationResult>(
            "Solving the lens needs at least 6 enabled points" + have +
            ": add points, or enter the lens throw ratio and solve pose-only");

    const Spread spread = analyseSpread(pairs);
    if (!(spread.sigma[0] > 1e-9) || spread.sigma[1] < COLLINEAR_RATIO * spread.sigma[0])
        return Err<CalibrationResult>(
            "The 3D points lie on (or near) one line: spread them across the object");
    double pixelExtent = 0.0;
    if (pixelSpreadRatio(pairs, pixelExtent) < COLLINEAR_RATIO || !(pixelExtent > 1e-6))
        return Err<CalibrationResult>(
            "The projector pixels lie on (or near) one line: spread them across the image");

    const bool coplanar = spread.sigma[2] < COPLANAR_RATIO * spread.sigma[0];
    if (mode == CalibrationMode::Full && coplanar) return Err<CalibrationResult>(COPLANAR_MESSAGE);
    const bool solveLens =
        mode == CalibrationMode::Full || (mode == CalibrationMode::Auto && pairs.size() >= 6 && !coplanar);

    Lens lens{knownIntrinsics.fx, knownIntrinsics.fy, knownIntrinsics.cx, knownIntrinsics.cy};
    Pose pose;
    std::string method;
    double cost = std::numeric_limits<double>::infinity();

    glm::dmat3 m;
    glm::dvec3 p4;
    const bool haveDlt = pairs.size() >= 6 && !coplanar && solveDlt(pairs, m, p4);

    // A linear start that already has points behind the projector cannot be refined
    // (the cost is infinite); that almost always means mismatched pairs.
    bool startBehind = false;
    if (solveLens) {
        if (!haveDlt || !decomposeProjection(m, p4, lens, pose))
            return Err<CalibrationResult>(DEGENERATE_MESSAGE);
        cost = refine(pairs, lens, pose, true);
        startBehind = !std::isfinite(cost);
        method = "DLT + Levenberg-Marquardt (lens and pose)";
    } else {
        if (!(lens.fx > 0.0) || !(lens.fy > 0.0) || !std::isfinite(lens.fx) ||
            !std::isfinite(lens.fy) || !std::isfinite(lens.cx) || !std::isfinite(lens.cy))
            return Err<CalibrationResult>(
                "The known lens values are invalid: set a positive throw ratio");
        // Refine from every applicable starting pose and keep the best.
        const auto tryStart = [&](Pose start, const char* name) {
            const double c = refine(pairs, lens, start, false);
            if (!std::isfinite(c)) startBehind = true;
            if (c < cost) {
                cost = c;
                pose = start;
                method = name;
            }
        };
        Pose start;
        if (!coplanar && poseFromPosit(pairs, spread, lens, start))
            tryStart(start, "POSIT + Levenberg-Marquardt (known lens)");
        if (haveDlt && poseFromDlt(m, p4, lens, start))
            tryStart(start, "DLT pose + Levenberg-Marquardt (known lens)");
        for (const Pose& planeStart : posesFromPlane(pairs, spread, lens))
            tryStart(planeStart, coplanar ? "Plane homography + Levenberg-Marquardt (known lens)"
                                          : "Plane-fit homography + Levenberg-Marquardt (known lens)");
    }

    if (!std::isfinite(cost)) {
        if (startBehind)
            return Err<CalibrationResult>(
                "The solve puts points behind the projector: check that each 3D point is "
                "paired with the right pixel");
        return Err<CalibrationResult>(DEGENERATE_MESSAGE);
    }
    const bool reweighted = reweightSingleOutlier(pairs, lens, pose, solveLens);
    if (reweighted) method += ", Cauchy-weighted (one pair looks mismatched)";
    if (!finite(lens, pose) || !(lens.fx > 0.0) || !(lens.fy > 0.0))
        return Err<CalibrationResult>(DEGENERATE_MESSAGE);

    CalibrationResult result;
    result.camera.intrinsics = knownIntrinsics;
    result.camera.intrinsics.fx = static_cast<float>(lens.fx);
    result.camera.intrinsics.fy = static_cast<float>(lens.fy);
    result.camera.intrinsics.cx = static_cast<float>(lens.cx);
    result.camera.intrinsics.cy = static_cast<float>(lens.cy);
    result.camera.pose.rotation = glm::mat3(pose.rotation);
    result.camera.pose.translation = Vec3(pose.translation);
    result.camera.calibrated = true;
    result.solvedIntrinsics = solveLens;
    result.method = method;

    // Errors are measured with the float camera the renderer will actually use.
    double sumSq = 0.0;
    result.perPointError.reserve(points.size());
    for (const CalibrationPoint& p : points) {
        Vec2 pixel;
        float error = -1.0f;
        if (finitePoint(p) && result.camera.project(p.world, pixel)) {
            error = glm::length(pixel - p.pixel);
            if (!std::isfinite(error)) error = -1.0f;
        }
        if (p.enabled) {
            if (error < 0.0f) return Err<CalibrationResult>(DEGENERATE_MESSAGE);
            sumSq += static_cast<double>(error) * error;
        }
        result.perPointError.push_back(error);
    }
    result.rmsError = static_cast<float>(std::sqrt(sumSq / static_cast<double>(pairs.size())));

    float worst = 0.0f;
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].enabled) worst = std::max(worst, result.perPointError[i]);
    }
    if (!reweighted && worst > OUTLIER_MIN_PX) {
        char text[256];
        if (pairs.size() < minPairsToLocateOutlier(solveLens)) {
            std::snprintf(text, sizeof(text),
                          "Poor fit (worst pair %.1f px). With fewer than %zu points the wrong "
                          "pair cannot be singled out: re-check each pair, or add points.",
                          worst, minPairsToLocateOutlier(solveLens));
        } else {
            std::snprintf(text, sizeof(text),
                          "Poor fit (worst pair %.1f px) and no single pair explains it: "
                          "several pairs may be mismatched. Re-check them.",
                          worst);
        }
        result.warning = text;
    }
    return Ok(std::move(result));
}

}  // namespace quadmapper
