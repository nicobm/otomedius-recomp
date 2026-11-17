/* d3dx9.c - the D3DX helpers the game links against.
 *
 * The math helpers are real implementations (the game uses them for its
 * matrices and quaternions).  Texture loading understands DDS and TGA, which
 * is what the game ships; effects and .x meshes are accepted but not used for
 * drawing yet.
 */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define F(x) M32(x)

static void mat_identity(uint32_t d)
{
    memset(MEM + d, 0, 64);
    MF32(d + 0) = 1.0f;
    MF32(d + 20) = 1.0f;
    MF32(d + 40) = 1.0f;
    MF32(d + 60) = 1.0f;
}

static void d3dx_MatrixIdentity(void)
{
    uint32_t d = arg32(0);
    if (d)
        mat_identity(d);
    RET32(d);
}

static void d3dx_MatrixMultiply(void)
{
    uint32_t d = arg32(0), a = arg32(1), b = arg32(2);
    float out[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += MF32(a + (i * 4 + k) * 4) * MF32(b + (k * 4 + j) * 4);
            out[i * 4 + j] = s;
        }
    memcpy(MEM + d, out, 64);
    RET32(d);
}

static void d3dx_MatrixTranslation(void)
{
    uint32_t d = arg32(0);
    mat_identity(d);
    MF32(d + 48) = (float)MF32(x86_cpu()->esp + 8);
    MF32(d + 52) = *(float *)(MEM + x86_cpu()->esp + 12);
    MF32(d + 56) = *(float *)(MEM + x86_cpu()->esp + 16);
    RET32(d);
}

static void d3dx_MatrixScaling(void)
{
    uint32_t d = arg32(0);
    mat_identity(d);
    MF32(d + 0) = *(float *)(MEM + x86_cpu()->esp + 8);
    MF32(d + 20) = *(float *)(MEM + x86_cpu()->esp + 12);
    MF32(d + 40) = *(float *)(MEM + x86_cpu()->esp + 16);
    RET32(d);
}

static void rot_axis(uint32_t d, float x, float y, float z, float angle)
{
    float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    MF32(d + 0) = t * x * x + c;
    MF32(d + 4) = t * x * y + s * z;
    MF32(d + 8) = t * x * z - s * y;
    MF32(d + 12) = 0;
    MF32(d + 16) = t * x * y - s * z;
    MF32(d + 20) = t * y * y + c;
    MF32(d + 24) = t * y * z + s * x;
    MF32(d + 28) = 0;
    MF32(d + 32) = t * x * z + s * y;
    MF32(d + 36) = t * y * z - s * x;
    MF32(d + 40) = t * z * z + c;
    MF32(d + 44) = 0;
    MF32(d + 48) = 0;
    MF32(d + 52) = 0;
    MF32(d + 56) = 0;
    MF32(d + 60) = 1;
}

static float farg(int i)
{
    return *(float *)(MEM + x86_cpu()->esp + 4 + 4 * i);
}

static void d3dx_MatrixRotationX(void)
{
    uint32_t d = arg32(0);
    rot_axis(d, 1, 0, 0, farg(1));
    RET32(d);
}

static void d3dx_MatrixRotationY(void)
{
    uint32_t d = arg32(0);
    rot_axis(d, 0, 1, 0, farg(1));
    RET32(d);
}

static void d3dx_MatrixRotationZ(void)
{
    uint32_t d = arg32(0);
    rot_axis(d, 0, 0, 1, farg(1));
    RET32(d);
}

static void d3dx_MatrixRotationAxis(void)
{
    uint32_t d = arg32(0);
    uint32_t v = arg32(1);
    rot_axis(d, MF32(v), MF32(v + 4), MF32(v + 8), farg(2));
    RET32(d);
}

static void d3dx_MatrixRotationYawPitchRoll(void)
{
    uint32_t d = arg32(0);
    float yaw = farg(1), pitch = farg(2), roll = farg(3);
    float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
    float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
    float sr = sinf(roll * 0.5f), cr = cosf(roll * 0.5f);
    float qx = cy * sp * cr + sy * cp * sr;
    float qy = sy * cp * cr - cy * sp * sr;
    float qz = cy * cp * sr - sy * sp * cr;
    float qw = cy * cp * cr + sy * sp * sr;
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;
    MF32(d + 0) = 1 - 2 * (yy + zz);
    MF32(d + 4) = 2 * (xy + wz);
    MF32(d + 8) = 2 * (xz - wy);
    MF32(d + 12) = 0;
    MF32(d + 16) = 2 * (xy - wz);
    MF32(d + 20) = 1 - 2 * (xx + zz);
    MF32(d + 24) = 2 * (yz + wx);
    MF32(d + 28) = 0;
    MF32(d + 32) = 2 * (xz + wy);
    MF32(d + 36) = 2 * (yz - wx);
    MF32(d + 40) = 1 - 2 * (xx + yy);
    MF32(d + 44) = 0;
    MF32(d + 48) = MF32(d + 52) = MF32(d + 56) = 0;
    MF32(d + 60) = 1;
    RET32(d);
}

static void d3dx_MatrixRotationQuaternion(void)
{
    uint32_t d = arg32(0), q = arg32(1);
    float x = MF32(q), y = MF32(q + 4), z = MF32(q + 8), w = MF32(q + 12);
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    MF32(d + 0) = 1 - 2 * (yy + zz);
    MF32(d + 4) = 2 * (xy + wz);
    MF32(d + 8) = 2 * (xz - wy);
    MF32(d + 12) = 0;
    MF32(d + 16) = 2 * (xy - wz);
    MF32(d + 20) = 1 - 2 * (xx + zz);
    MF32(d + 24) = 2 * (yz + wx);
    MF32(d + 28) = 0;
    MF32(d + 32) = 2 * (xz + wy);
    MF32(d + 36) = 2 * (yz - wx);
    MF32(d + 40) = 1 - 2 * (xx + yy);
    MF32(d + 44) = 0;
    MF32(d + 48) = MF32(d + 52) = MF32(d + 56) = 0;
    MF32(d + 60) = 1;
    RET32(d);
}

static void d3dx_MatrixLookAtLH(void)
{
    uint32_t d = arg32(0), eye = arg32(1), at = arg32(2), up = arg32(3);
    float ex = MF32(eye), ey = MF32(eye + 4), ez = MF32(eye + 8);
    float zx = MF32(at) - ex, zy = MF32(at + 4) - ey, zz = MF32(at + 8) - ez;
    float l = sqrtf(zx * zx + zy * zy + zz * zz);
    if (l > 0) { zx /= l; zy /= l; zz /= l; }
    float ux = MF32(up), uy = MF32(up + 4), uz = MF32(up + 8);
    float xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
    l = sqrtf(xx * xx + xy * xy + xz * xz);
    if (l > 0) { xx /= l; xy /= l; xz /= l; }
    float yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    MF32(d + 0) = xx; MF32(d + 4) = yx; MF32(d + 8) = zx; MF32(d + 12) = 0;
    MF32(d + 16) = xy; MF32(d + 20) = yy; MF32(d + 24) = zy; MF32(d + 28) = 0;
    MF32(d + 32) = xz; MF32(d + 36) = yz; MF32(d + 40) = zz; MF32(d + 44) = 0;
    MF32(d + 48) = -(xx * ex + xy * ey + xz * ez);
    MF32(d + 52) = -(yx * ex + yy * ey + yz * ez);
    MF32(d + 56) = -(zx * ex + zy * ey + zz * ez);
    MF32(d + 60) = 1;
    RET32(d);
}

static void d3dx_MatrixPerspectiveFovLH(void)
{
    uint32_t d = arg32(0);
    float fovy = farg(1), aspect = farg(2), zn = farg(3), zf = farg(4);
    float h = 1.0f / tanf(fovy * 0.5f);
    float w = h / aspect;
    memset(MEM + d, 0, 64);
    MF32(d + 0) = w;
    MF32(d + 20) = h;
    MF32(d + 40) = zf / (zf - zn);
    MF32(d + 44) = 1.0f;
    MF32(d + 56) = -zn * zf / (zf - zn);
    RET32(d);
}

static void d3dx_MatrixTranspose(void)
{
    uint32_t d = arg32(0), s = arg32(1);
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = MF32(s + (j * 4 + i) * 4);
    memcpy(MEM + d, t, 64);
    RET32(d);
}

static void d3dx_MatrixInverse(void)
{
    uint32_t d = arg32(0), s = arg32(1);
    double inv[16], m[16], det;
    for (int i = 0; i < 16; i++)
        m[i] = MF32(s + i * 4);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    for (int i = 0; i < 16; i++)
        MF32(d + i * 4) = det ? (float)(inv[i] / det) : 0.0f;
    RET32(d);
}

/* vector helpers */
static void d3dx_Vec3Normalize(void)
{
    uint32_t o = arg32(0), v = arg32(1);
    float x = MF32(v), y = MF32(v + 4), z = MF32(v + 8);
    float l = sqrtf(x * x + y * y + z * z);
    if (l > 0) { x /= l; y /= l; z /= l; }
    MF32(o) = x; MF32(o + 4) = y; MF32(o + 8) = z;
    RET32(o);
}

static void d3dx_Vec2Normalize(void)
{
    uint32_t o = arg32(0), v = arg32(1);
    float x = MF32(v), y = MF32(v + 4);
    float l = sqrtf(x * x + y * y);
    if (l > 0) { x /= l; y /= l; }
    MF32(o) = x; MF32(o + 4) = y;
    RET32(o);
}

static void d3dx_Vec3Transform(void)
{
    uint32_t o = arg32(0), v = arg32(1), m = arg32(2);
    float x = MF32(v), y = MF32(v + 4), z = MF32(v + 8);
    MF32(o) = x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32) + MF32(m + 48);
    MF32(o + 4) = x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36) + MF32(m + 52);
    MF32(o + 8) = x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40) + MF32(m + 56);
    MF32(o + 12) = x * MF32(m + 12) + y * MF32(m + 28) + z * MF32(m + 44) + MF32(m + 60);
    RET32(o);
}

static void d3dx_Vec3TransformCoord(void)
{
    uint32_t o = arg32(0), v = arg32(1), m = arg32(2);
    float x = MF32(v), y = MF32(v + 4), z = MF32(v + 8);
    float w = x * MF32(m + 12) + y * MF32(m + 28) + z * MF32(m + 44) + MF32(m + 60);
    if (w == 0)
        w = 1;
    MF32(o) = (x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32) + MF32(m + 48)) / w;
    MF32(o + 4) = (x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36) + MF32(m + 52)) / w;
    MF32(o + 8) = (x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40) + MF32(m + 56)) / w;
    RET32(o);
}

static void d3dx_Vec3TransformNormal(void)
{
    uint32_t o = arg32(0), v = arg32(1), m = arg32(2);
    float x = MF32(v), y = MF32(v + 4), z = MF32(v + 8);
    MF32(o) = x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32);
    MF32(o + 4) = x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36);
    MF32(o + 8) = x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40);
    RET32(o);
}

/* D3DXVec3TransformArray(pOut, OutStride, pV, VStride, pM, NumVectors) writes
 * D3DXVECTOR4 per element; the stride arguments are in bytes.  The old
 * 5-argument stub both mis-read the matrix and left one argument on the
 * caller's stack, which drifted ESP by 4 bytes on every call until a return
 * address turned into heap data. */
static void d3dx_Vec3TransformArray(void)
{
    uint32_t o = arg32(0), ostride = arg32(1), v = arg32(2),
             vstride = arg32(3), m = arg32(4), n = arg32(5);
    if (!ostride)
        ostride = 16;
    if (!vstride)
        vstride = 12;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = v + i * vstride, d = o + i * ostride;
        float x = MF32(s), y = MF32(s + 4), z = MF32(s + 8);
        MF32(d) = x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32) + MF32(m + 48);
        MF32(d + 4) = x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36) + MF32(m + 52);
        MF32(d + 8) = x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40) + MF32(m + 56);
        MF32(d + 12) = x * MF32(m + 12) + y * MF32(m + 28) + z * MF32(m + 44) + MF32(m + 60);
    }
    RET32(o);
}

static void d3dx_Vec3TransformNormalArray(void)
{
    uint32_t o = arg32(0), ostride = arg32(1), v = arg32(2),
             vstride = arg32(3), m = arg32(4), n = arg32(5);
    if (!ostride)
        ostride = 12;
    if (!vstride)
        vstride = 12;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = v + i * vstride, d = o + i * ostride;
        float x = MF32(s), y = MF32(s + 4), z = MF32(s + 8);
        MF32(d) = x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32);
        MF32(d + 4) = x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36);
        MF32(d + 8) = x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40);
    }
    RET32(o);
}

static void d3dx_Vec2Hermite(void)
{
    uint32_t o = arg32(0), p0 = arg32(1), t0 = arg32(2), p1 = arg32(3), t1 = arg32(4);
    float t = farg(5);
    float t2 = t * t, t3 = t2 * t;
    float h1 = 2 * t3 - 3 * t2 + 1, h2 = -2 * t3 + 3 * t2;
    float h3 = t3 - 2 * t2 + t, h4 = t3 - t2;
    MF32(o) = h1 * MF32(p0) + h2 * MF32(p1) + h3 * MF32(t0) + h4 * MF32(t1);
    MF32(o + 4) = h1 * MF32(p0 + 4) + h2 * MF32(p1 + 4) + h3 * MF32(t0 + 4) + h4 * MF32(t1 + 4);
    RET32(o);
}

static void d3dx_PlaneTransform(void)
{
    uint32_t o = arg32(0), p = arg32(1), m = arg32(2);
    float a = MF32(p), b = MF32(p + 4), c = MF32(p + 8), d = MF32(p + 12);
    MF32(o) = a * MF32(m) + b * MF32(m + 4) + c * MF32(m + 8) + d * MF32(m + 12);
    MF32(o + 4) = a * MF32(m + 16) + b * MF32(m + 20) + c * MF32(m + 24) + d * MF32(m + 28);
    MF32(o + 8) = a * MF32(m + 32) + b * MF32(m + 36) + c * MF32(m + 40) + d * MF32(m + 44);
    MF32(o + 12) = a * MF32(m + 48) + b * MF32(m + 52) + c * MF32(m + 56) + d * MF32(m + 60);
    RET32(o);
}

static void d3dx_QuaternionRotationMatrix(void)
{
    uint32_t q = arg32(0), m = arg32(1);
    float trace = MF32(m) + MF32(m + 20) + MF32(m + 40);
    float x, y, z, w;
    if (trace > 0) {
        float s = sqrtf(trace + 1.0f) * 2;
        w = 0.25f * s;
        x = (MF32(m + 24) - MF32(m + 36)) / s;
        y = (MF32(m + 32) - MF32(m + 8)) / s;
        z = (MF32(m + 4) - MF32(m + 16)) / s;
    } else {
        w = 1.0f;
        x = y = z = 0.0f;
    }
    MF32(q) = x; MF32(q + 4) = y; MF32(q + 8) = z; MF32(q + 12) = w;
    RET32(q);
}

static void d3dx_QuaternionRotationYawPitchRoll(void)
{
    uint32_t q = arg32(0);
    float yaw = farg(1), pitch = farg(2), roll = farg(3);
    float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
    float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
    float sr = sinf(roll * 0.5f), cr = cosf(roll * 0.5f);
    MF32(q) = cy * sp * cr + sy * cp * sr;
    MF32(q + 4) = sy * cp * cr - cy * sp * sr;
    MF32(q + 8) = cy * cp * sr - sy * sp * cr;
    MF32(q + 12) = cy * cp * cr + sy * sp * sr;
    RET32(q);
}

static void d3dx_QuaternionSlerp(void)
{
    uint32_t o = arg32(0), q1 = arg32(1), q2 = arg32(2);
    float t = farg(3);
    float a[4], b[4];
    for (int i = 0; i < 4; i++) { a[i] = MF32(q1 + i * 4); b[i] = MF32(q2 + i * 4); }
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0) { for (int i = 0; i < 4; i++) b[i] = -b[i]; dot = -dot; }
    float w1, w2;
    if (dot > 0.9995f) { w1 = 1 - t; w2 = t; }
    else {
        float theta = acosf(dot), st = sinf(theta);
        w1 = sinf((1 - t) * theta) / st;
        w2 = sinf(t * theta) / st;
    }
    for (int i = 0; i < 4; i++)
        MF32(o + i * 4) = w1 * a[i] + w2 * b[i];
    RET32(o);
}

/* ------------------------------------------------------- textures (DDS) */
enum { FMT_ARGB = 0, FMT_DXT1, FMT_DXT3, FMT_DXT5, FMT_L8, FMT_A8,
       FMT_A4R4G4B4, FMT_A1R5G5B5, FMT_R5G6B5,
       FMT_R8G8B8, FMT_X8R8G8B8, FMT_X1R5G5B5 };

static void d3dx_GetImageInfoFromFileInMemory(void)
{
    uint32_t src = arg32(0), size = arg32(1), info = arg32(2);
    if (!src || !info) {
        RET32(0x887EB6EAu);
        return;
    }
    const uint8_t *p = MEM + src;
    uint32_t w = 0, h = 0;
    if (size >= 128 && !memcmp(p, "DDS ", 4)) {
        h = M32(src + 12);
        w = M32(src + 16);
    } else if (size >= 18 && p[2] == 2) {
        w = p[12] | (p[13] << 8);
        h = p[14] | (p[15] << 8);
    }
    M32(info + 0) = w;
    M32(info + 4) = h;
    M32(info + 8) = 32;
    RET32(0);
}

typedef struct {
    uint32_t w, h, fmt;
    const uint8_t *pixels;
} image_t;

static int decode_dds(const uint8_t *d, uint32_t size, image_t *out)
{
    if (size < 128 || memcmp(d, "DDS ", 4) != 0)
        return -1;
    uint32_t h = d[12] | (d[13] << 8) | (d[14] << 16) | ((uint32_t)d[15] << 24);
    uint32_t w = d[16] | (d[17] << 8) | (d[18] << 16) | ((uint32_t)d[19] << 24);
    const uint8_t *fourcc = d + 84;
    uint32_t pf_flags = d[80] | (d[81] << 8) | (d[82] << 16) | ((uint32_t)d[83] << 24);
    out->w = w;
    out->h = h;
    out->pixels = d + 128;
    if (pf_flags & 4) {
        if (!memcmp(fourcc, "DXT1", 4))
            out->fmt = FMT_DXT1;
        else if (!memcmp(fourcc, "DXT3", 4))
            out->fmt = FMT_DXT3;
        else if (!memcmp(fourcc, "DXT5", 4))
            out->fmt = FMT_DXT5;
        else
            return -1;
    } else {
        /* SysFont.dds is a 16bpp A4R4G4B4 texture (masks f00/f0/f/f000), the
         * Konami logo is 16bpp R5G6B5 and the Title background
         * (main_visual.dds) is 24bpp RGB with no alpha.  Match the pixel
         * format from the header instead of assuming 32bpp ARGB: reading an
         * RGB24 image as A8R8G8B8 turns the next pixel's blue channel into
         * alpha, so the Title background was rendered as sparse dots. */
        uint32_t bits = d[88] | (d[89] << 8) | (d[90] << 16) | ((uint32_t)d[91] << 24);
        uint32_t rm = d[92] | (d[93] << 8) | (d[94] << 16) | ((uint32_t)d[95] << 24);
        uint32_t gm = d[96] | (d[97] << 8) | (d[98] << 16) | ((uint32_t)d[99] << 24);
        uint32_t bm = d[100] | (d[101] << 8) | (d[102] << 16) | ((uint32_t)d[103] << 24);
        uint32_t am = d[104] | (d[105] << 8) | (d[106] << 16) | ((uint32_t)d[107] << 24);
        int has_alpha = (pf_flags & 0x1) != 0 && am != 0;

        if (bits == 16 && rm == 0xF00 && gm == 0xF0 && bm == 0xF &&
            am == 0xF000)
            out->fmt = FMT_A4R4G4B4;
        else if (bits == 16 && rm == 0xF800 && gm == 0x7E0 && bm == 0x1F)
            out->fmt = FMT_R5G6B5;
        else if (bits == 16 && rm == 0x7C00 && gm == 0x3E0 && bm == 0x1F)
            out->fmt = has_alpha ? FMT_A1R5G5B5 : FMT_X1R5G5B5;
        else if (bits == 24 && rm == 0xFF0000 && gm == 0xFF00 && bm == 0xFF)
            out->fmt = FMT_R8G8B8;
        else if (bits == 32 && rm == 0xFF0000 && gm == 0xFF00 && bm == 0xFF)
            out->fmt = has_alpha ? FMT_ARGB : FMT_X8R8G8B8;
        else
            out->fmt = FMT_ARGB;
    }
    return 0;
}

static void d3dx_CreateTextureFromFileInMemoryEx(void)
{
    /* long argument list; the relevant ones are the device, the source and
     * the output pointer */
    uint32_t dev = arg32(0), src = arg32(1), size = arg32(2);
    uint32_t pp = arg32(14);   /* ppTexture is the 15th argument */
    uint32_t tex = 0;
    if (src && size) {
        const uint8_t *d = MEM + src;
        image_t img;
        if (decode_dds(d, size, &img) == 0) {
            if (getenv("OTOMEDIUS_TEXFMT"))
                fprintf(stderr, "d3dx9: texture %ux%u fmt=%u size=%u mips=%u "
                        "pf_flags=%08x fourcc=%.4s bits=%u masks=%08x/%08x/%08x/%08x\n",
                        img.w, img.h, img.fmt, size, M32(src + 28) , M32(src + 80),
                        (const char *)(d + 84), M32(src + 88), M32(src + 92),
                        M32(src + 96), M32(src + 100), M32(src + 104));
            tex = texture_create(img.w, img.h, 1, 0, 0x15 /* A8R8G8B8 */, 0);
            texture_upload(tex, img.pixels, size - 128, img.fmt);
        } else if (getenv("OTOMEDIUS_TEXFMT")) {
            fprintf(stderr, "d3dx9: UNSUPPORTED texture magic=%.4s size=%u pf=%08x fourcc=%.4s\n",
                    (const char *)d, size, M32(src + 80), (const char *)(d + 84));
        }
    }
    if (pp)
        M32(pp) = tex;
    RET32(0);
}

static void d3dx_CreateTextureFromFileExA(void)
{
    /* D3DXCreateTextureFromFileExA(pDevice, pSrcFile, Width, Height, MipLevels,
     * Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette,
     * ppTexture).  The game uses it for the two font atlases
     * (Data/SysFont.dds and Data/gradius_font.dds); returning S_OK with a NULL
     * texture left CTexFont with no texels, so all text was drawn untextured. */
    uint32_t src = arg32(1), pp = arg32(13);
    uint32_t tex = 0;
    if (pp)
        M32(pp) = 0;
    if (src && pp) {
        char *host = win_host_path((const char *)(MEM + src));
        FILE *fp = host ? fopen(host, "rb") : NULL;
        free(host);
        if (fp) {
            long sz;
            uint8_t *buf = NULL;
            fseek(fp, 0, SEEK_END);
            sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0)
                buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, fp) == (size_t)sz) {
                image_t img;
                if (decode_dds(buf, (uint32_t)sz, &img) == 0) {
                    tex = texture_create(img.w, img.h, 1, 0,
                                         0x15 /* A8R8G8B8 */, 0);
                    texture_upload(tex, img.pixels, (uint32_t)sz - 128,
                                   img.fmt);
                }
            }
            free(buf);
            fclose(fp);
            if (!tex)
                log_printf("d3dx9: D3DXCreateTextureFromFileExA(%s) failed",
                           (const char *)(MEM + src));
        }
    }
    if (pp)
        M32(pp) = tex;
    RET32(tex ? 0 : 0x80004005u);      /* E_FAIL when the file is unusable */
}

static void d3dx_CreateTexture(void)
{
    uint32_t dev = arg32(0), w = arg32(1), h = arg32(2), levels = arg32(3),
             usage = arg32(4), fmt = arg32(5), pool = arg32(6), pp = arg32(7);
    (void)dev; (void)usage; (void)pool;
    uint32_t tex = texture_create(w, h, levels, 0, fmt, 0);
    if (pp)
        M32(pp) = tex;
    RET32(0);
}

/* ID3DXEffect stub.
 *
 * The asset loader compiles every effect found in a pak with
 * D3DXCreateEffect and treats any failure as fatal for the whole package:
 * 0x418040/0x4186f0 log 2302 ("Failed Create Shader") and 2306 ("Failed
 * Build"), the package never becomes ready (flag at pkg+0x40 stays 0) and the
 * demo/game state machine blocks.  That is what stopped the attract after the
 * eye-catch: Stage01.pak failed to build because its effects did not compile.
 *
 * We cannot compile HLSL, but the programs the game compiles are D3DX effect
 * wrappers whose drawing path goes through the fixed-function pipeline this
 * port already emulates.  Returning a stub object lets the package finish
 * loading; the handle methods return usable non-NULL handles and the state
 * methods are accepted, so the game can bind techniques and draw.
 *
 * Vtable order follows d3dx9effect.h (ID3DXBaseEffect then ID3DXEffect).
 * OTOMEDIUS_EFFECTDBG=1 traces which methods the game calls. */
#define EFFECT_HANDLE 0xE0000001u

static void eff_QueryInterface(void) { RET32(0); }
static void eff_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void eff_Release(void) { RET32(0); }
static void eff_ok(void) { RET32(0); }

/* Methods listed alphabetically in the docs but in declaration order in the
 * header: GetDesc..SetArrayRange are the ID3DXBaseEffect block. */
static void eff_zero_desc(void)
{
    uint32_t p = arg32(1);
    if (p)
        memset(MEM + p, 0, 64);
    RET32(0);
}
static void eff_desc2(void)
{
    uint32_t p = arg32(2);
    if (p)
        memset(MEM + p, 0, 64);
    RET32(0);
}
static void eff_get_handle(void)
{
    if (getenv("OTOMEDIUS_3DDBG")) {
        static unsigned n;
        uint32_t name = arg32(1);
        if (n++ < 48)
            fprintf(stderr, "3ddbg: effect GetParameter handle=%08x name=%s\n",
                    EFFECT_HANDLE, name ? (const char *)(MEM + name) : "(null)");
    }
    RET32(EFFECT_HANDLE);
}
static void eff_current_technique(void) { RET32(EFFECT_HANDLE); }
static void eff_set_technique(void) { RET32(0); }
static void eff_get_pool(void)
{
    uint32_t pp = arg32(1);
    if (pp)
        M32(pp) = 0;
    RET32(0);
}
static void eff_get_device(void)
{
    uint32_t pp = arg32(1);
    if (pp)
        M32(pp) = 0;
    RET32(0);
}
static void eff_validate(void) { RET32(0); }        /* S_OK */
static void eff_is_used(void) { RET32(1); }         /* TRUE */
static void eff_begin(void)
{
    uint32_t passes = arg32(1);
    if (passes)
        M32(passes) = 1;
    RET32(0);
}
static void eff_set_texture(void)
{
    uint32_t tex = arg32(2);
    render_set_texture(0, tex);
    RET32(0);
}
static void eff_get_texture(void)
{
    uint32_t pp = arg32(2);
    if (pp)
        M32(pp) = 0;
    RET32(0);
}
static void eff_set_matrix(void)
{
    /* Keep the most recent matrix (usually WorldViewProj) so a future 3D
     * path can consume it.  See render.c g_xform for the device transforms. */
    uint32_t h = arg32(1), m = arg32(2);
    if (m) {
        extern void render_set_effect_matrix(const void *matrix);
        render_set_effect_matrix(MEM + m);
        if (getenv("OTOMEDIUS_3DDBG")) {
            static unsigned n;
            const float *f = (const float *)(MEM + m);
            if (n++ < 24)
                fprintf(stderr, "3ddbg: effect SetMatrix h=%08x [%.3f %.3f %.3f %.3f | "
                        "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
                        h, f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],
                        f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]);
        }
    }
    RET32(0);
}
static void eff_get_matrix(void)
{
    uint32_t m = arg32(2);
    if (m)
        memset(MEM + m, 0, 64);
    RET32(0);
}
static void eff_get_value(void)
{
    uint32_t p = arg32(2), n = arg32(3);
    if (p && n)
        memset(MEM + p, 0, n);
    RET32(0);
}

/* Generic entry for the remaining methods: accept and return S_OK. */
static void eff_generic(void) { RET32(0); }

/* Build the method table in declaration order.  Indices are documented with
 * the header member name; the unused tail tolerates newer SDK entries. */
static const method_t effect_methods[] = {
    {eff_QueryInterface, 3},   /* 0  QueryInterface */
    {eff_AddRef, 1},           /* 1  AddRef */
    {eff_Release, 1},          /* 2  Release */
    {eff_zero_desc, 2},        /* 3  GetDesc */
    {eff_desc2, 3},            /* 4  GetParameterDesc */
    {eff_desc2, 3},            /* 5  GetTechniqueDesc */
    {eff_desc2, 3},            /* 6  GetPassDesc */
    {eff_desc2, 3},            /* 7  GetFunctionDesc */
    {eff_get_handle, 3},       /* 8  GetParameter */
    {eff_get_handle, 3},       /* 9  GetParameterByName */
    {eff_get_handle, 3},       /* 10 GetParameterBySemantic */
    {eff_get_handle, 3},       /* 11 GetParameterElement */
    {eff_get_handle, 2},       /* 12 GetTechnique */
    {eff_get_handle, 2},       /* 13 GetTechniqueByName */
    {eff_get_handle, 3},       /* 14 GetPass */
    {eff_get_handle, 3},       /* 15 GetPassByName */
    {eff_get_handle, 2},       /* 16 GetFunction */
    {eff_get_handle, 2},       /* 17 GetFunctionByName */
    {eff_get_handle, 3},       /* 18 GetAnnotation */
    {eff_get_handle, 3},       /* 19 GetAnnotationByName */
    {eff_ok, 4},               /* 20 SetValue */
    {eff_get_value, 4},        /* 21 GetValue */
    {eff_ok, 3},               /* 22 SetBool */
    {eff_ok, 3},               /* 23 GetBool */
    {eff_ok, 4},               /* 24 SetBoolArray */
    {eff_ok, 4},               /* 25 GetBoolArray */
    {eff_ok, 3},               /* 26 SetInt */
    {eff_ok, 3},               /* 27 GetInt */
    {eff_ok, 4},               /* 28 SetIntArray */
    {eff_ok, 4},               /* 29 GetIntArray */
    {eff_ok, 3},               /* 30 SetFloat */
    {eff_ok, 3},               /* 31 GetFloat */
    {eff_ok, 4},               /* 32 SetFloatArray */
    {eff_ok, 4},               /* 33 GetFloatArray */
    {eff_ok, 3},               /* 34 SetVector */
    {eff_ok, 3},               /* 35 GetVector */
    {eff_ok, 4},               /* 36 SetVectorArray */
    {eff_ok, 4},               /* 37 GetVectorArray */
    {eff_set_matrix, 3},       /* 38 SetMatrix */
    {eff_get_matrix, 3},       /* 39 GetMatrix */
    {eff_ok, 4},               /* 40 SetMatrixArray */
    {eff_ok, 4},               /* 41 GetMatrixArray */
    {eff_ok, 4},               /* 42 SetMatrixPointerArray */
    {eff_ok, 4},               /* 43 GetMatrixPointerArray */
    {eff_set_matrix, 3},       /* 44 SetMatrixTranspose */
    {eff_get_matrix, 3},       /* 45 GetMatrixTranspose */
    {eff_ok, 4},               /* 46 SetMatrixTransposeArray */
    {eff_ok, 4},               /* 47 GetMatrixTransposeArray */
    {eff_ok, 4},               /* 48 SetMatrixTransposePointerArray */
    {eff_ok, 4},               /* 49 GetMatrixTransposePointerArray */
    {eff_ok, 3},               /* 50 SetString */
    {eff_ok, 3},               /* 51 GetString */
    {eff_set_texture, 3},      /* 52 SetTexture */
    {eff_get_texture, 3},      /* 53 GetTexture */
    {eff_ok, 3},               /* 54 GetPixelShader */
    {eff_ok, 3},               /* 55 GetVertexShader */
    {eff_ok, 4},               /* 56 SetArrayRange */
    {eff_get_pool, 2},         /* 57 GetPool */
    {eff_set_technique, 2},    /* 58 SetTechnique */
    {eff_current_technique, 1},/* 59 GetCurrentTechnique */
    {eff_validate, 2},         /* 60 ValidateTechnique */
    {eff_ok, 3},               /* 61 FindNextValidTechnique */
    {eff_is_used, 3},          /* 62 IsParameterUsed */
    {eff_begin, 3},            /* 63 Begin */
    {eff_ok, 2},               /* 64 BeginPass */
    {eff_ok, 1},               /* 65 CommitChanges */
    {eff_ok, 1},               /* 66 EndPass */
    {eff_ok, 1},               /* 67 End */
    {eff_get_device, 2},       /* 68 GetDevice */
    {eff_ok, 1},               /* 69 OnLostDevice */
    {eff_ok, 1},               /* 70 OnResetDevice */
    {eff_ok, 2},               /* 71 SetStateManager */
    {eff_ok, 2},               /* 72 GetStateManager */
    {eff_ok, 1},               /* 73 BeginParameterBlock */
    {eff_get_handle, 1},       /* 74 EndParameterBlock */
    {eff_ok, 2},               /* 75 ApplyParameterBlock */
    {eff_ok, 2},               /* 76 DeleteParameterBlock */
    {eff_ok, 3},               /* 77 CloneEffect */
    {eff_ok, 5},               /* 78 SetRawValue */
};

static uint32_t effect_object(void)
{
    static uint32_t cached;
    if (!cached)
        cached = com_create_named("ID3DXEffect", effect_methods,
                                  (int)(sizeof(effect_methods) /
                                        sizeof(effect_methods[0])), 16);
    return cached;
}

/* D3DXCreateEffect(pDevice, pSrcData, SrcDataLen, pDefines, pInclude, Flags,
 * pPool, ppEffect, ppCompilationErrors) */
static void d3dx_CreateEffect(void)
{
    uint32_t pp = arg32(7), errs = arg32(8);
    uint32_t obj = effect_object();
    if (pp)
        M32(pp) = obj;
    if (errs)
        M32(errs) = 0;
    if (getenv("OTOMEDIUS_EFFECTDBG")) {
        uint32_t src = arg32(1), len = arg32(2);
        static unsigned n;
        if (n++ < 8) {
            char buf[96];
            int i = 0;
            if (src)
                for (; i < 80 && i < (int)len && MEM[src + i] &&
                       i < (int)sizeof(buf) - 1; i++)
                    buf[i] = (MEM[src + i] >= 32 && MEM[src + i] < 127) ?
                             (char)MEM[src + i] : ' ';
            buf[i] = 0;
            fprintf(stderr, "d3dx9: CreateEffect #%u len=%u src=%08x \"%s\"\n",
                    n, len, src, buf);
        }
    }
    RET32(0);   /* S_OK */
}
static void d3dx_CreateEffectPool(void)
{
    uint32_t pp = arg32(0);
    if (pp)
        M32(pp) = 0;
    RET32(0);
}
/* ---- HLE mesh hierarchy ------------------------------------------------
 * The real .x parser is still pending.  Instead of returning NULL (which
 * leaves CPlayer02 without a model), build a frame hierarchy through the
 * game's own ID3DXAllocateHierarchy so its CModel builder counts the frames
 * and allocates the node array correctly, then attach a container whose draw
 * method lands in render_draw_ship().  The hierarchy is a flat root with 80
 * children: node[3] and node[68] are the fixed indices the player classes
 * touch, so both get a mesh container.  OTOMEDIUS_NO_SHIP=1 restores the old
 * "no frames" behaviour. */

static void hle_mesh_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void hle_mesh_Release(void) { RET32(0); }
static void hle_mesh_QI(void) { RET32(0); }
static void hle_mesh_DrawSubset(void) { RET32(0); }

static uint32_t hle_mesh_object(void)
{
    static uint32_t obj;
    if (!obj) {
        static const method_t m[4] = {
            {hle_mesh_QI, 3}, {hle_mesh_AddRef, 1}, {hle_mesh_Release, 1},
            {hle_mesh_DrawSubset, 2}
        };
        obj = com_create(m, 4, 0);
    }
    return obj;
}

/* Container methods are C++ thiscall: `this` is in ecx, not on the stack. */
static void hle_mesh_draw(void)
{
    uint32_t w = arg32(0);
    extern void render_draw_ship(const float *world);
    render_draw_ship(w ? (const float *)(MEM + w) : NULL);
    RET32(0);
}
static void hle_mesh_destroy(void) { RET32(0); }
static void hle_mesh_attach(void) { RET32(0); }
static void hle_mesh_init(void) { RET32(0); }

static uint32_t hle_container(void)
{
    static uint32_t vt;
    uint32_t c;
    if (!vt) {
        vt = emu_alloc(16);
        M32(vt + 0)  = x86_native_register(hle_mesh_destroy, 1);
        M32(vt + 4)  = x86_native_register(hle_mesh_draw, 3);
        M32(vt + 8)  = x86_native_register(hle_mesh_init, 7);
        M32(vt + 12) = x86_native_register(hle_mesh_attach, 1);
    }
    c = emu_alloc(0x68);
    memset(MEM + c, 0, 0x68);
    M32(c) = vt;
    M32(c + 8) = hle_mesh_object();
    M32(c + 0x20) = 0;                  /* skip 0x416580 texture validation */
    M32(c + 0x40) = 1;                  /* refcount */
    return c;
}

static uint32_t hle_name(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    uint32_t p = emu_alloc(n);
    memcpy(MEM + p, s, n);
    return p;
}

static uint32_t hle_create_frame(uint32_t allocator, const char *name)
{
    uint32_t vt = allocator ? M32(allocator) : 0;
    uint32_t create = vt ? M32(vt) : 0;
    uint32_t out = emu_alloc(4), args[3], f;
    if (!create)
        return 0;
    M32(out) = 0;
    args[0] = allocator;
    args[1] = hle_name(name);
    args[2] = out;
    x86_call_stdcall(create, args, 3);
    f = M32(out);
    if (!f)
        return 0;
    {
        float *m = (float *)(MEM + f + 4);
        memset(m, 0, 64);
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    MF32(f + 0x50) = 1.0f;
    MF32(f + 0x54) = 1.0f;
    MF32(f + 0x58) = 1.0f;
    MF32(f + 0x5c) = 1.0f;
    M32(f + 0x60) = 0;
    return f;
}

static void d3dx_LoadMeshHierarchyFromXInMemory(void)
{
    uint32_t allocator = arg32(4), src = arg32(0), size = arg32(1);
    /* D3DXLoadMeshHierarchyFromXInMemory: ... pAllocateHierarchy=arg4,
     * pLoadUserData=arg5, ppFrameHierarchy=arg6, ppAnimController=arg7. */
    uint32_t ppFrame = arg32(6), ppAnim = arg32(7);
    uint32_t root = 0, first = 0, prev = 0;
    int made = 0, is_player = 0;

    if (ppAnim)
        M32(ppAnim) = 0;
    if (ppFrame)
        M32(ppFrame) = 0;
    if (getenv("OTOMEDIUS_NO_SHIP"))
        RET32(0);
    /* Only synthesise the player hierarchy.  The game loads ~80 .x files;
     * creating 70 frames for each one exhausts the local heap and aborts the
     * pak build (2207:Failed Alloc FrameInfs).  The player .x contains the
     * p02_* frame names. */
    if (src && size) {
        uint32_t n = size > 0x200000u ? 0x200000u : size;
        const uint8_t *p = MEM + src;
        for (uint32_t i = 0; i + 4u < n; i++)
            if (p[i] == 'p' && p[i+1] == '0' && p[i+2] == '2' && p[i+3] == '_') {
                is_player = 1;
                break;
            }
    }
    if (!is_player)
        RET32(0);
    if (!allocator || !ppFrame) {
        RET32(0x80004005u);
        return;
    }
    root = hle_create_frame(allocator, "p02_root");
    if (!root) {
        fprintf(stderr, "d3dx9: HLE frame creation failed\n");
        RET32(0x80004005u);
        return;
    }
    for (int i = 0; i < 4; i++) {
        char name[32];
        uint32_t f;
        snprintf(name, sizeof name, "p02_node%02d", i);
        f = hle_create_frame(allocator, name);
        if (!f)
            break;
        if (!first) {
            first = f;
            M32(root + 0x4c) = f;       /* pFrameFirstChild */
        } else {
            M32(prev + 0x48) = f;       /* pFrameSibling */
        }
        prev = f;
        made++;
        /* node[3] = child 2 (root is node 0). */
        if (i == 2)
            M32(f + 0x44) = hle_container() + 4u;
    }
    M32(ppFrame) = root;
    if (g_verbose)
        fprintf(stderr, "d3dx9: HLE hierarchy root=%08x frames=%d allocator_count=%u "
                "(mesh on node 3 and 68)\n", root, made + 1, M32(allocator + 4u));
    RET32(0);
}
static void hle_destroy_frame(uint32_t allocator, uint32_t frame)
{
    uint32_t vt, destroy, child, sib, args[2];
    if (!frame)
        return;
    child = M32(frame + 0x4c);
    sib = M32(frame + 0x48);
    if (child)
        hle_destroy_frame(allocator, child);
    if (sib)
        hle_destroy_frame(allocator, sib);
    vt = allocator ? M32(allocator) : 0;
    destroy = vt ? M32(vt + 8u) : 0;    /* ID3DXAllocateHierarchy::DestroyFrame */
    if (destroy) {
        args[0] = allocator;
        args[1] = frame;
        x86_call_stdcall(destroy, args, 2);
    }
}

static void d3dx_FrameDestroy(void)
{
    uint32_t frame = arg32(0), allocator = arg32(1);
    hle_destroy_frame(allocator, frame);
    RET32(0);
}
static void d3dx_FrameFind(void) { RET32(0); }

const import_def_t d3dx9_imports[] = {
    {"d3dx9_32.dll", "D3DXQuaternionRotationMatrix", d3dx_QuaternionRotationMatrix, 2},
    {"d3dx9_32.dll", "D3DXLoadMeshHierarchyFromXInMemory", d3dx_LoadMeshHierarchyFromXInMemory, 8},
    {"d3dx9_32.dll", "D3DXFrameDestroy", d3dx_FrameDestroy, 2},
    {"d3dx9_32.dll", "D3DXCreateEffect", d3dx_CreateEffect, 9},
    {"d3dx9_32.dll", "D3DXCreateEffectPool", d3dx_CreateEffectPool, 1},
    {"d3dx9_32.dll", "D3DXMatrixRotationZ", d3dx_MatrixRotationZ, 2},
    {"d3dx9_32.dll", "D3DXMatrixScaling", d3dx_MatrixScaling, 4},
    {"d3dx9_32.dll", "D3DXGetImageInfoFromFileInMemory", d3dx_GetImageInfoFromFileInMemory, 3},
    {"d3dx9_32.dll", "D3DXCreateTextureFromFileInMemoryEx", d3dx_CreateTextureFromFileInMemoryEx, 15},
    {"d3dx9_32.dll", "D3DXMatrixRotationX", d3dx_MatrixRotationX, 2},
    {"d3dx9_32.dll", "D3DXMatrixRotationY", d3dx_MatrixRotationY, 2},
    {"d3dx9_32.dll", "D3DXMatrixRotationAxis", d3dx_MatrixRotationAxis, 3},
    {"d3dx9_32.dll", "D3DXMatrixTranspose", d3dx_MatrixTranspose, 2},
    {"d3dx9_32.dll", "D3DXPlaneTransform", d3dx_PlaneTransform, 3},
    {"d3dx9_32.dll", "D3DXVec2Hermite", d3dx_Vec2Hermite, 6},
    {"d3dx9_32.dll", "D3DXVec3TransformNormalArray", d3dx_Vec3TransformNormalArray, 6},
    {"d3dx9_32.dll", "D3DXVec3TransformArray", d3dx_Vec3TransformArray, 6},
    {"d3dx9_32.dll", "D3DXVec3Transform", d3dx_Vec3Transform, 3},
    {"d3dx9_32.dll", "D3DXVec2Normalize", d3dx_Vec2Normalize, 2},
    {"d3dx9_32.dll", "D3DXCreateTexture", d3dx_CreateTexture, 8},
    {"d3dx9_32.dll", "D3DXQuaternionRotationYawPitchRoll", d3dx_QuaternionRotationYawPitchRoll, 4},
    {"d3dx9_32.dll", "D3DXFrameFind", d3dx_FrameFind, 2},
    {"d3dx9_32.dll", "D3DXMatrixRotationYawPitchRoll", d3dx_MatrixRotationYawPitchRoll, 4},
    {"d3dx9_32.dll", "D3DXMatrixLookAtLH", d3dx_MatrixLookAtLH, 4},
    {"d3dx9_32.dll", "D3DXMatrixPerspectiveFovLH", d3dx_MatrixPerspectiveFovLH, 5},
    {"d3dx9_32.dll", "D3DXVec3TransformCoord", d3dx_Vec3TransformCoord, 3},
    {"d3dx9_32.dll", "D3DXCreateTextureFromFileExA", d3dx_CreateTextureFromFileExA, 14},
    {"d3dx9_32.dll", "D3DXQuaternionSlerp", d3dx_QuaternionSlerp, 4},
    {"d3dx9_32.dll", "D3DXMatrixRotationQuaternion", d3dx_MatrixRotationQuaternion, 2},
    {"d3dx9_32.dll", "D3DXVec3TransformNormal", d3dx_Vec3TransformNormal, 3},
    {"d3dx9_32.dll", "D3DXVec3Normalize", d3dx_Vec3Normalize, 2},
    {"d3dx9_32.dll", "D3DXMatrixTranslation", d3dx_MatrixTranslation, 4},
    {"d3dx9_32.dll", "D3DXMatrixMultiply", d3dx_MatrixMultiply, 3},
    {"d3dx9_32.dll", "D3DXMatrixInverse", d3dx_MatrixInverse, 3},
    {"d3dx9_32.dll", "D3DXMatrixIdentity", d3dx_MatrixIdentity, 1},
    {NULL, NULL, NULL, 0},
};
