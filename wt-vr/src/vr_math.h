// Matrix helpers for WT:VR.
//
// GHWT conventions (verified from captured shader constants):
//   - row-vector matrices: out = v.x*row0 + v.y*row1 + v.z*row2 + v.w*row3
//     so World * ViewProj = WorldViewProj
//   - right-handed, Y up, camera looks down -Z (same as OpenXR)
//   - D3D depth range 0..1, near 0.1, far ~505, units look like metres
#pragma once

#include <cmath>
#include <cstring>
#include <utility>

struct Vector3 {
    float x, y, z;
};

struct Quaternion {
    float x, y, z, w;
};

struct Matrix4 {
    float values[4][4];

    static Matrix4 Identity() {
        Matrix4 matrix = {};
        for (int index = 0; index < 4; ++index) matrix.values[index][index] = 1.0f;
        return matrix;
    }

    static Matrix4 FromRegisters(const float registers[][4]) {
        Matrix4 matrix;
        memcpy(matrix.values, registers, sizeof(matrix.values));
        return matrix;
    }
};

inline Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
    Matrix4 product;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int inner = 0; inner < 4; ++inner) sum += left.values[row][inner] * right.values[inner][column];
            product.values[row][column] = sum;
        }
    return product;
}

// Gauss-Jordan with partial pivoting. Returns false if singular.
inline bool Invert(const Matrix4& source, Matrix4& inverse) {
    double work[4][8];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 8; ++column)
            work[row][column] = column < 4 ? source.values[row][column] : (column - 4 == row ? 1.0 : 0.0);

    for (int pivotColumn = 0; pivotColumn < 4; ++pivotColumn) {
        int pivotRow = pivotColumn;
        for (int row = pivotColumn + 1; row < 4; ++row)
            if (fabs(work[row][pivotColumn]) > fabs(work[pivotRow][pivotColumn])) pivotRow = row;
        if (fabs(work[pivotRow][pivotColumn]) < 1e-12) return false;
        if (pivotRow != pivotColumn)
            for (int column = 0; column < 8; ++column) std::swap(work[pivotRow][column], work[pivotColumn][column]);

        double pivot = work[pivotColumn][pivotColumn];
        for (int column = 0; column < 8; ++column) work[pivotColumn][column] /= pivot;
        for (int row = 0; row < 4; ++row) {
            if (row == pivotColumn) continue;
            double factor = work[row][pivotColumn];
            for (int column = 0; column < 8; ++column) work[row][column] -= factor * work[pivotColumn][column];
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) inverse.values[row][column] = float(work[row][column + 4]);
    return true;
}

// Rigid transform (rotation rows + translation row) for a pose, row-vector convention:
// rows 0..2 are the rotated X, Y, Z axes, row 3 is the position.
inline Matrix4 PoseToMatrix(const Quaternion& orientation, const Vector3& position) {
    const float x = orientation.x, y = orientation.y, z = orientation.z, w = orientation.w;
    Matrix4 matrix = Matrix4::Identity();
    matrix.values[0][0] = 1 - 2 * (y * y + z * z);
    matrix.values[0][1] = 2 * (x * y + z * w);
    matrix.values[0][2] = 2 * (x * z - y * w);
    matrix.values[1][0] = 2 * (x * y - z * w);
    matrix.values[1][1] = 1 - 2 * (x * x + z * z);
    matrix.values[1][2] = 2 * (y * z + x * w);
    matrix.values[2][0] = 2 * (x * z + y * w);
    matrix.values[2][1] = 2 * (y * z - x * w);
    matrix.values[2][2] = 1 - 2 * (x * x + y * y);
    matrix.values[3][0] = position.x;
    matrix.values[3][1] = position.y;
    matrix.values[3][2] = position.z;
    return matrix;
}

// Rotation about +Y by yaw radians, then translation (row-vector convention).
inline Matrix4 YawTranslation(float yaw, const Vector3& position) {
    Matrix4 matrix = Matrix4::Identity();
    const float cosine = cosf(yaw), sine = sinf(yaw);
    matrix.values[0][0] = cosine;  matrix.values[0][2] = -sine;
    matrix.values[2][0] = sine;    matrix.values[2][2] = cosine;
    matrix.values[3][0] = position.x;
    matrix.values[3][1] = position.y;
    matrix.values[3][2] = position.z;
    return matrix;
}

// Right-handed off-centre perspective with D3D 0..1 depth, matching the game's own projection layout:
//   x_clip = 2x/(R-L) + z(R+L)/(R-L),  y likewise,  z_clip = z*f/(n-f) + n*f/(n-f),  w = -z
// tangents are tan() of the OpenXR fov angles (left and down are negative).
inline Matrix4 OffCentreProjection(float tangentLeft, float tangentRight, float tangentDown, float tangentUp,
                                   float nearPlane, float farPlane) {
    Matrix4 matrix = {};
    const float width = tangentRight - tangentLeft;
    const float height = tangentUp - tangentDown;
    matrix.values[0][0] = 2.0f / width;
    matrix.values[1][1] = 2.0f / height;
    matrix.values[2][0] = (tangentRight + tangentLeft) / width;
    matrix.values[2][1] = (tangentUp + tangentDown) / height;
    matrix.values[2][2] = farPlane / (nearPlane - farPlane);
    matrix.values[2][3] = -1.0f;
    matrix.values[3][2] = nearPlane * farPlane / (nearPlane - farPlane);
    return matrix;
}

// Camera position from a ViewProj: clip x, y and w are all zero at the eye.
inline bool CameraPositionFromViewProj(const Matrix4& viewProj, Vector3& position) {
    const int columns[3] = { 0, 1, 3 };
    double system[3][4];
    for (int equation = 0; equation < 3; ++equation) {
        const int column = columns[equation];
        system[equation][0] = viewProj.values[0][column];
        system[equation][1] = viewProj.values[1][column];
        system[equation][2] = viewProj.values[2][column];
        system[equation][3] = -viewProj.values[3][column];
    }
    for (int pivotColumn = 0; pivotColumn < 3; ++pivotColumn) {
        int pivotRow = pivotColumn;
        for (int row = pivotColumn + 1; row < 3; ++row)
            if (fabs(system[row][pivotColumn]) > fabs(system[pivotRow][pivotColumn])) pivotRow = row;
        if (fabs(system[pivotRow][pivotColumn]) < 1e-12) return false;
        for (int column = 0; column < 4; ++column) std::swap(system[pivotRow][column], system[pivotColumn][column]);
        for (int row = 0; row < 3; ++row) {
            if (row == pivotColumn) continue;
            double factor = system[row][pivotColumn] / system[pivotColumn][pivotColumn];
            for (int column = 0; column < 4; ++column) system[row][column] -= factor * system[pivotColumn][column];
        }
    }
    position.x = float(system[0][3] / system[0][0]);
    position.y = float(system[1][3] / system[1][1]);
    position.z = float(system[2][3] / system[2][2]);
    return true;
}

// Yaw of a camera from its ViewProj. Column 3 (clip w) is the view direction, so the
// camera's "back" axis is its negation; yaw puts the anchor's +Z along that back axis.
inline float CameraYawFromViewProj(const Matrix4& viewProj) {
    const float backX = -viewProj.values[0][3];
    const float backZ = -viewProj.values[2][3];
    return atan2f(backX, backZ);
}
