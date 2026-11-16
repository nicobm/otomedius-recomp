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

static void d3dx_Vec3TransformArray(void)
{
    uint32_t o = arg32(0), v = arg32(1), m = arg32(2), n = arg32(3), stride = arg32(4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = v + i * stride, d = o + i * 12;
        float x = MF32(s), y = MF32(s + 4), z = MF32(s + 8);
        MF32(d) = x * MF32(m) + y * MF32(m + 16) + z * MF32(m + 32) + MF32(m + 48);
        MF32(d + 4) = x * MF32(m + 4) + y * MF32(m + 20) + z * MF32(m + 36) + MF32(m + 52);
        MF32(d + 8) = x * MF32(m + 8) + y * MF32(m + 24) + z * MF32(m + 40) + MF32(m + 56);
    }
    RET32(o);
}

static void d3dx_Vec3TransformNormalArray(void)
{
    uint32_t o = arg32(0), v = arg32(1), m = arg32(2), n = arg32(3), stride = arg32(4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = v + i * stride, d = o + i * 12;
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
enum { FMT_ARGB = 0, FMT_DXT1, FMT_DXT3, FMT_DXT5, FMT_L8, FMT_A8 };

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
            tex = texture_create(img.w, img.h, 1, 0, 0x15 /* A8R8G8B8 */, 0);
            texture_upload(tex, img.pixels, size - 128, img.fmt);
        }
    }
    if (pp)
        M32(pp) = tex;
    RET32(0);
}

static void d3dx_CreateTextureFromFileExA(void)
{
    uint32_t pp = arg32(13);
    if (pp)
        M32(pp) = 0;
    RET32(0);
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

/* D3DXCreateEffect(pDevice, pSrcData, SrcDataLen, pDefines, pInclude,
 * pStateBlock, pPool, pStateBlock?, ppEffect, pHRESULT) -> ppEffect is the
 * 9th argument; it must be written or the caller uses stack garbage. */
static void d3dx_CreateEffect(void)
{
    uint32_t pp = arg32(8);
    if (pp)
        M32(pp) = 0;
    RET32(0x80004005u);   /* E_FAIL */
}
static void d3dx_CreateEffectPool(void)
{
    uint32_t pp = arg32(0);
    if (pp)
        M32(pp) = 0;
    RET32(0);
}
/* ID3DXAnimationController** and ID3DXFRAME** are the last two arguments.
 * Leaving them untouched made the loader treat whatever was on the stack as an
 * ID3DXMesh and call `[vtbl+0x28]` on it (crash at 0x10A97). The .x parser is
 * not implemented, so report failure with NULL outputs and let the game skip
 * the mesh. */
static void d3dx_LoadMeshHierarchyFromXInMemory(void)
{
    uint32_t ppAnim = arg32(6), ppFrame = arg32(7);
    if (ppAnim)
        M32(ppAnim) = 0;
    if (ppFrame)
        M32(ppFrame) = 0;
    if (g_verbose > 1)
        fprintf(stderr, "d3dx9: LoadMeshHierarchy(%u bytes) -> S_OK, no frames "
                "(mesh loader not implemented)\n", arg32(1));
    RET32(0);   /* S_OK: the caller checks the frame pointer and skips it */
}
static void d3dx_FrameDestroy(void) { RET32(0); }
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
    {"d3dx9_32.dll", "D3DXVec3TransformNormalArray", d3dx_Vec3TransformNormalArray, 5},
    {"d3dx9_32.dll", "D3DXVec3TransformArray", d3dx_Vec3TransformArray, 5},
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
