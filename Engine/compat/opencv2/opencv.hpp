/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The handful of OpenCV calls krkr2's software render manager makes (box blur, resize,
// affine and perspective warps of 8-bit RGBA images), written for Shiori so the engine needs
// no OpenCV. Same call shapes; behaviour follows OpenCV's documentation (integer pixel
// positions, constant transparent border for warps, reflect-101 border for the box filter).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#define CV_8UC1 0
#define CV_8UC4 24

namespace cv {

enum InterpolationFlags { INTER_NEAREST = 0, INTER_LINEAR = 1, INTER_CUBIC = 2, INTER_AREA = 3 };

struct Size {
    int width = 0, height = 0;
    Size() = default;
    Size(int w, int h) : width(w), height(h) {}
};

struct Point2f {
    float x = 0, y = 0;
    Point2f() = default;
    Point2f(float x_, float y_) : x(x_), y(y_) {}
};

class Mat {
public:
    int rows = 0, cols = 0, kind = CV_8UC4;
    size_t step = 0;
    uint8_t *data = nullptr;
    std::shared_ptr<std::vector<uint8_t>> owned;
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // for transform matrices

    Mat() = default;
    Mat(int r, int c, int type, void *pixels, size_t stride)
        : rows(r), cols(c), kind(type), step(stride), data(static_cast<uint8_t *>(pixels)) {}

    int channels() const { return kind == CV_8UC4 ? 4 : 1; }
    int type() const { return kind; }
    bool empty() const { return data == nullptr; }
    void create(int r, int c, int type) {
        rows = r;
        cols = c;
        kind = type;
        step = size_t(c) * channels();
        owned = std::make_shared<std::vector<uint8_t>>(step * r);
        data = owned->data();
    }
    uint8_t *ptr(int row) { return data + step * row; }
    const uint8_t *ptr(int row) const { return data + step * row; }
    size_t step1(int = 0) const { return step; }
    template <typename T> T &at(int r, int c) { return reinterpret_cast<T *>(m)[r * 3 + c]; }
};

namespace detail {

inline void sample(const Mat &src, double x, double y, int interpolation, uint8_t *out) {
    const int ch = src.channels();
    auto pixel = [&](int px, int py, int c) -> double {
        if (px < 0 || py < 0 || px >= src.cols || py >= src.rows) return 0.0;
        return src.ptr(py)[px * ch + c];
    };
    if (interpolation == INTER_NEAREST) {
        const int px = int(std::floor(x + 0.5)), py = int(std::floor(y + 0.5));
        for (int c = 0; c < ch; ++c) out[c] = uint8_t(pixel(px, py, c));
        return;
    }
    if (interpolation == INTER_CUBIC) {
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        auto weight = [](double t) {
            const double a = -0.75;
            t = std::fabs(t);
            if (t <= 1) return ((a + 2) * t - (a + 3)) * t * t + 1;
            if (t < 2) return ((a * t - 5 * a) * t + 8 * a) * t - 4 * a;
            return 0.0;
        };
        double wx[4], wy[4];
        for (int i = 0; i < 4; ++i) {
            wx[i] = weight(fx - (i - 1));
            wy[i] = weight(fy - (i - 1));
        }
        for (int c = 0; c < ch; ++c) {
            double sum = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) sum += wx[i] * wy[j] * pixel(x0 + i - 1, y0 + j - 1, c);
            out[c] = uint8_t(std::clamp(std::lround(sum), 0L, 255L));
        }
        return;
    }
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    for (int c = 0; c < ch; ++c) {
        const double top = pixel(x0, y0, c) * (1 - fx) + pixel(x0 + 1, y0, c) * fx;
        const double bottom = pixel(x0, y0 + 1, c) * (1 - fx) + pixel(x0 + 1, y0 + 1, c) * fx;
        out[c] = uint8_t(std::clamp(std::lround(top * (1 - fy) + bottom * fy), 0L, 255L));
    }
}

// Solves the 8x8 system for the homography that maps src[i] to dst[i].
inline bool solve(double a[8][9]) {
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 8; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        if (std::fabs(a[pivot][col]) < 1e-12) return false;
        if (pivot != col)
            for (int k = 0; k < 9; ++k) std::swap(a[pivot][k], a[col][k]);
        for (int r = 0; r < 8; ++r) {
            if (r == col) continue;
            const double f = a[r][col] / a[col][col];
            for (int k = col; k < 9; ++k) a[r][k] -= f * a[col][k];
        }
    }
    return true;
}

inline bool invert3(const double *m, double *inv) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12) return false;
    const double d = 1.0 / det;
    inv[0] = (m[4] * m[8] - m[5] * m[7]) * d;
    inv[1] = (m[2] * m[7] - m[1] * m[8]) * d;
    inv[2] = (m[1] * m[5] - m[2] * m[4]) * d;
    inv[3] = (m[5] * m[6] - m[3] * m[8]) * d;
    inv[4] = (m[0] * m[8] - m[2] * m[6]) * d;
    inv[5] = (m[2] * m[3] - m[0] * m[5]) * d;
    inv[6] = (m[3] * m[7] - m[4] * m[6]) * d;
    inv[7] = (m[1] * m[6] - m[0] * m[7]) * d;
    inv[8] = (m[0] * m[4] - m[1] * m[3]) * d;
    return true;
}

inline void warp(const Mat &src, Mat &dst, const double *forward, Size size, int interpolation) {
    dst.create(size.height, size.width, src.type());
    std::memset(dst.data, 0, dst.step * dst.rows);
    double inv[9];
    if (!invert3(forward, inv)) return;
    const int ch = src.channels();
    for (int y = 0; y < size.height; ++y) {
        uint8_t *row = dst.ptr(y);
        for (int x = 0; x < size.width; ++x) {
            const double w = inv[6] * x + inv[7] * y + inv[8];
            if (std::fabs(w) < 1e-12) continue;
            const double sx = (inv[0] * x + inv[1] * y + inv[2]) / w;
            const double sy = (inv[3] * x + inv[4] * y + inv[5]) / w;
            if (sx < -1 || sy < -1 || sx > src.cols || sy > src.rows) continue;
            sample(src, sx, sy, interpolation == INTER_AREA ? INTER_LINEAR : interpolation, row + x * ch);
        }
    }
}

} // namespace detail

inline Mat getAffineTransform(const Point2f *src, const Point2f *dst) {
    // x' = a x + b y + c, y' = d x + e y + f from the first three points.
    double a[8][9] = {};
    double s[6][7] = {};
    for (int i = 0; i < 3; ++i) {
        double *r1 = s[i * 2], *r2 = s[i * 2 + 1];
        r1[0] = src[i].x, r1[1] = src[i].y, r1[2] = 1, r1[6] = dst[i].x;
        r2[3] = src[i].x, r2[4] = src[i].y, r2[5] = 1, r2[6] = dst[i].y;
    }
    // Gaussian elimination on 6x7.
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 6; ++r)
            if (std::fabs(s[r][col]) > std::fabs(s[pivot][col])) pivot = r;
        if (std::fabs(s[pivot][col]) < 1e-12) break;
        if (pivot != col)
            for (int k = 0; k < 7; ++k) std::swap(s[pivot][k], s[col][k]);
        for (int r = 0; r < 6; ++r) {
            if (r == col) continue;
            const double f = s[r][col] / s[col][col];
            for (int k = col; k < 7; ++k) s[r][k] -= f * s[col][k];
        }
    }
    (void)a;
    Mat m;
    for (int i = 0; i < 6; ++i) m.m[i] = std::fabs(s[i][i]) < 1e-12 ? 0 : s[i][6] / s[i][i];
    m.m[6] = 0, m.m[7] = 0, m.m[8] = 1;
    return m;
}

inline Mat getPerspectiveTransform(const Point2f *src, const Point2f *dst) {
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        double *r1 = a[i * 2], *r2 = a[i * 2 + 1];
        const double x = src[i].x, y = src[i].y, u = dst[i].x, v = dst[i].y;
        r1[0] = x, r1[1] = y, r1[2] = 1, r1[6] = -x * u, r1[7] = -y * u, r1[8] = u;
        r2[3] = x, r2[4] = y, r2[5] = 1, r2[6] = -x * v, r2[7] = -y * v, r2[8] = v;
    }
    Mat m;
    if (detail::solve(a))
        for (int i = 0; i < 8; ++i) m.m[i] = a[i][8] / a[i][i];
    m.m[8] = 1;
    return m;
}

inline void warpAffine(const Mat &src, Mat &dst, const Mat &matrix, Size size, int flags = INTER_LINEAR) {
    detail::warp(src, dst, matrix.m, size, flags & 7);
}

inline void warpPerspective(const Mat &src, Mat &dst, const Mat &matrix, Size size, int flags = INTER_LINEAR) {
    detail::warp(src, dst, matrix.m, size, flags & 7);
}

inline void resize(const Mat &src, Mat &dst, Size size, double = 0, double = 0, int interpolation = INTER_LINEAR) {
    if (dst.empty()) dst.create(size.height, size.width, src.type());
    const int ch = src.channels();
    const double sx = double(src.cols) / size.width, sy = double(src.rows) / size.height;
    if (interpolation == INTER_AREA && sx >= 1 && sy >= 1) {
        // Box average over each destination pixel's footprint.
        for (int y = 0; y < size.height; ++y) {
            const double y0 = y * sy, y1 = std::min<double>((y + 1) * sy, src.rows);
            uint8_t *row = dst.ptr(y);
            for (int x = 0; x < size.width; ++x) {
                const double x0 = x * sx, x1 = std::min<double>((x + 1) * sx, src.cols);
                double sum[4] = {}, area = 0;
                for (int py = int(y0); py < int(std::ceil(y1)); ++py) {
                    const double wy = std::min<double>(py + 1, y1) - std::max<double>(py, y0);
                    const uint8_t *srow = src.ptr(py);
                    for (int px = int(x0); px < int(std::ceil(x1)); ++px) {
                        const double w = wy * (std::min<double>(px + 1, x1) - std::max<double>(px, x0));
                        for (int c = 0; c < ch; ++c) sum[c] += w * srow[px * ch + c];
                        area += w;
                    }
                }
                for (int c = 0; c < ch; ++c) row[x * ch + c] = uint8_t(std::lround(area > 0 ? sum[c] / area : 0));
            }
        }
        return;
    }
    const int mode = interpolation == INTER_AREA ? INTER_LINEAR : interpolation;
    for (int y = 0; y < size.height; ++y) {
        uint8_t *row = dst.ptr(y);
        double fy = mode == INTER_NEAREST ? std::floor(y * sy) : (y + 0.5) * sy - 0.5;
        fy = std::clamp(fy, 0.0, double(src.rows - 1));
        for (int x = 0; x < size.width; ++x) {
            double fx = mode == INTER_NEAREST ? std::floor(x * sx) : (x + 0.5) * sx - 0.5;
            fx = std::clamp(fx, 0.0, double(src.cols - 1));
            detail::sample(src, fx, fy, mode, row + x * ch);
        }
    }
}

// Normalized box filter, anchor at the centre, reflect-101 border.
inline void boxFilter(const Mat &src, Mat &dst, int, Size ksize) {
    const int ch = src.channels(), w = src.cols, h = src.rows;
    if (w <= 0 || h <= 0) return;
    auto reflect = [](int i, int n) {
        if (n == 1) return 0;
        while (i < 0 || i >= n) i = i < 0 ? -i : 2 * n - 2 - i;
        return i;
    };
    const int ax = ksize.width / 2, ay = ksize.height / 2;
    std::vector<uint32_t> horizontal(size_t(w) * h * ch);
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src.ptr(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) {
                uint32_t sum = 0;
                for (int k = 0; k < ksize.width; ++k) sum += row[reflect(x - ax + k, w) * ch + c];
                horizontal[(size_t(y) * w + x) * ch + c] = sum;
            }
    }
    const double n = double(ksize.width) * ksize.height;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = dst.ptr(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) {
                uint32_t sum = 0;
                for (int k = 0; k < ksize.height; ++k) sum += horizontal[(size_t(reflect(y - ay + k, h)) * w + x) * ch + c];
                row[x * ch + c] = uint8_t(std::lround(sum / n));
            }
    }
}

} // namespace cv
