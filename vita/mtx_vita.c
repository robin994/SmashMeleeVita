/* Scalar SDK implementations from extern/dolphin/src/dolphin/mtx.
 * PS entry points use the original C math on ARM instead of PowerPC assembly. */
#include <dolphin.h>
#include <dolphin/mtx.h>
#include <math.h>

void PSVECSubtract(Vec* a, Vec* b, Vec* c)
{
    ASSERTMSGLINE(0x9C, a, "VECSubtract():  NULL VecPtr 'a' ");
    ASSERTMSGLINE(0x9D, b, "VECSubtract():  NULL VecPtr 'b' ");
    ASSERTMSGLINE(0x9E, c, "VECSubtract():  NULL VecPtr 'a_b' ");
    c->x = a->x - b->x;
    c->y = a->y - b->y;
    c->z = a->z - b->z;
}

void PSVECNormalize(Vec* src, Vec* unit)
{
    f32 mag;

    ASSERTMSGLINE(0x127, src, "VECNormalize():  NULL VecPtr 'src' ");
    ASSERTMSGLINE(0x128, unit, "VECNormalize():  NULL VecPtr 'unit' ");
    mag = (src->z * src->z) + ((src->x * src->x) + (src->y * src->y));
    ASSERTMSGLINE(0x12D, 0.0f != mag,
                  "VECNormalize():  zero magnitude vector ");
    mag = 1.0f / sqrtf(mag);
    unit->x = src->x * mag;
    unit->y = src->y * mag;
    unit->z = src->z * mag;
}

f32 PSVECDotProduct(Vec* a, Vec* b)
{
    f32 dot;

    ASSERTMSGLINE(0x1D1, a, "VECDotProduct():  NULL VecPtr 'a' ");
    ASSERTMSGLINE(0x1D2, b, "VECDotProduct():  NULL VecPtr 'b' ");
    dot = (a->z * b->z) + ((a->x * b->x) + (a->y * b->y));
    return dot;
}

void PSVECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
    Vec vTmp;

    ASSERTMSGLINE(0x20F, a, "VECCrossProduct():  NULL VecPtr 'a' ");
    ASSERTMSGLINE(0x210, b, "VECCrossProduct():  NULL VecPtr 'b' ");
    ASSERTMSGLINE(0x211, axb, "VECCrossProduct():  NULL VecPtr 'axb' ");

    vTmp.x = (a->y * b->z) - (a->z * b->y);
    vTmp.y = (a->z * b->x) - (a->x * b->z);
    vTmp.z = (a->x * b->y) - (a->y * b->x);
    axb->x = vTmp.x;
    axb->y = vTmp.y;
    axb->z = vTmp.z;
}

void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    Vec vN;
    f32 s;
    f32 c;
    f32 t;
    f32 x;
    f32 y;
    f32 z;
    f32 xSq;
    f32 ySq;
    f32 zSq;

    ASSERTMSGLINE(0x50B, m, "MTXRotAxisRad():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x50C, axis, "MTXRotAxisRad():  NULL VecPtr 'axis' ");

    s = sinf(rad);
    c = cosf(rad);
    t = 1 - c;
    VECNormalize(axis, &vN);
    x = vN.x;
    y = vN.y;
    z = vN.z;
    xSq = (x * x);
    ySq = (y * y);
    zSq = (z * z);
    m[0][0] = (c + (t * xSq));
    m[0][1] = (y * (t * x)) - (s * z);
    m[0][2] = (z * (t * x)) + (s * y);
    m[0][3] = 0;
    m[1][0] = ((y * (t * x)) + (s * z));
    m[1][1] = (c + (t * ySq));
    m[1][2] = ((z * (t * y)) - (s * x));
    m[1][3] = 0;
    m[2][0] = ((z * (t * x)) - (s * y));
    m[2][1] = ((z * (t * y)) + (s * x));
    m[2][2] = (c + (t * zSq));
    m[2][3] = 0;
}

void C_MTXLookAt(Mtx m, Vec* camPos, Vec* camUp, Vec* target)
{
    Vec vLook;
    Vec vRight;
    Vec vUp;

    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    VECNormalize(&vLook, &vLook);

    VECCrossProduct(camUp, &vLook, &vRight);
    VECNormalize(&vRight, &vRight);
    VECCrossProduct(&vLook, &vRight, &vUp);

    m[0][0] = vRight.x;
    m[0][1] = vRight.y;
    m[0][2] = vRight.z;
    m[0][3] = -((camPos->z * vRight.z) + ((camPos->x * vRight.x) + (camPos->y * vRight.y)));

    m[1][0] = vUp.x;
    m[1][1] = vUp.y;
    m[1][2] = vUp.z;
    m[1][3] = -((camPos->z * vUp.z) + ((camPos->x * vUp.x) + (camPos->y * vUp.y)));

    m[2][0] = vLook.x;
    m[2][1] = vLook.y;
    m[2][2] = vLook.z;
    m[2][3] = -((camPos->z * vLook.z) + ((camPos->x * vLook.x) + (camPos->y * vLook.y)));
}

void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    Vec vTmp;

    ASSERTMSGLINE(0x13A, m, "PSMTXMultVecSR():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x13B, src, "PSMTXMultVecSR():  NULL MtxPtr 'src' ");
    ASSERTMSGLINE(0x13C, dst, "PSMTXMultVecSR():  NULL MtxPtr 'dst' ");
    vTmp.x = (m[0][2] * src->z) + ((m[0][0] * src->x) + (m[0][1] * src->y));
    vTmp.y = (m[1][2] * src->z) + ((m[1][0] * src->x) + (m[1][1] * src->y));
    vTmp.z = (m[2][2] * src->z) + ((m[2][0] * src->x) + (m[2][1] * src->y));
    dst->x = vTmp.x;
    dst->y = vTmp.y;
    dst->z = vTmp.z;
}

void PSVECAdd(Vec* a, Vec* b, Vec* c)
{
    ASSERTMSGLINE(0x57, a, "VECAdd():  NULL VecPtr 'a' ");
    ASSERTMSGLINE(0x58, b, "VECAdd():  NULL VecPtr 'b' ");
    ASSERTMSGLINE(0x59, c, "VECAdd():  NULL VecPtr 'ab' ");
    c->x = a->x + b->x;
    c->y = a->y + b->y;
    c->z = a->z + b->z;
}

void PSVECScale(Vec* src, Vec* dst, f32 scale)
{
    ASSERTMSGLINE(0xE2, src, "VECScale():  NULL VecPtr 'src' ");
    ASSERTMSGLINE(0xE3, dst, "VECScale():  NULL VecPtr 'dst' ");
    dst->x = (src->x * scale);
    dst->y = (src->y * scale);
    dst->z = (src->z * scale);
}

f32 PSVECMag(Vec* v)
{
    return sqrtf(VECSquareMag(v));
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    ASSERTMSGLINE(0xDE, src, "MTXCopy():  NULL MtxPtr 'src' ");
    ASSERTMSGLINE(0xDF, dst, "MTXCopy():  NULL MtxPtr 'dst' ");
    if (src != dst) {
        dst[0][0] = src[0][0];
        dst[0][1] = src[0][1];
        dst[0][2] = src[0][2];
        dst[0][3] = src[0][3];
        dst[1][0] = src[1][0];
        dst[1][1] = src[1][1];
        dst[1][2] = src[1][2];
        dst[1][3] = src[1][3];
        dst[2][0] = src[2][0];
        dst[2][1] = src[2][1];
        dst[2][2] = src[2][2];
        dst[2][3] = src[2][3];
    }
}

void PSMTXConcat(Mtx a, Mtx b, Mtx ab)
{
    Mtx mTmp;
    f32(*m)[4];

    ASSERTMSGLINE(0x128, a, "MTXConcat():  NULL MtxPtr 'a'  ");
    ASSERTMSGLINE(0x129, b, "MTXConcat():  NULL MtxPtr 'b'  ");
    ASSERTMSGLINE(0x12A, ab, "MTXConcat():  NULL MtxPtr 'ab' ");

    if (ab == a || ab == b) {
        m = mTmp;
    } else {
        m = ab;
    }

    m[0][0] =
        0 + a[0][2] * b[2][0] + ((a[0][0] * b[0][0]) + (a[0][1] * b[1][0]));
    m[0][1] =
        0 + a[0][2] * b[2][1] + ((a[0][0] * b[0][1]) + (a[0][1] * b[1][1]));
    m[0][2] =
        0 + a[0][2] * b[2][2] + ((a[0][0] * b[0][2]) + (a[0][1] * b[1][2]));
    m[0][3] = a[0][3] +
              (a[0][2] * b[2][3] + (a[0][0] * b[0][3] + (a[0][1] * b[1][3])));

    m[1][0] =
        0 + a[1][2] * b[2][0] + ((a[1][0] * b[0][0]) + (a[1][1] * b[1][0]));
    m[1][1] =
        0 + a[1][2] * b[2][1] + ((a[1][0] * b[0][1]) + (a[1][1] * b[1][1]));
    m[1][2] =
        0 + a[1][2] * b[2][2] + ((a[1][0] * b[0][2]) + (a[1][1] * b[1][2]));
    m[1][3] = a[1][3] +
              (a[1][2] * b[2][3] + (a[1][0] * b[0][3] + (a[1][1] * b[1][3])));

    m[2][0] =
        0 + a[2][2] * b[2][0] + ((a[2][0] * b[0][0]) + (a[2][1] * b[1][0]));
    m[2][1] =
        0 + a[2][2] * b[2][1] + ((a[2][0] * b[0][1]) + (a[2][1] * b[1][1]));
    m[2][2] =
        0 + a[2][2] * b[2][2] + ((a[2][0] * b[0][2]) + (a[2][1] * b[1][2]));
    m[2][3] = a[2][3] +
              (a[2][2] * b[2][3] + (a[2][0] * b[0][3] + (a[2][1] * b[1][3])));

    if (m == mTmp) {
        PSMTXCopy(mTmp, ab);
    }
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    ASSERTMSGLINE(0x58A, m, "MTXScale():  NULL MtxPtr 'm' ");
    m[0][0] = xS;
    m[0][1] = 0;
    m[0][2] = 0;
    m[0][3] = 0;
    m[1][0] = 0;
    m[1][1] = yS;
    m[1][2] = 0;
    m[1][3] = 0;
    m[2][0] = 0;
    m[2][1] = 0;
    m[2][2] = zS;
    m[2][3] = 0;
}

void PSMTXQuat(Mtx m, QuaternionPtr q)
{
    f32 s;
    f32 xs;
    f32 ys;
    f32 zs;
    f32 wx;
    f32 wy;
    f32 wz;
    f32 xx;
    f32 xy;
    f32 xz;
    f32 yy;
    f32 yz;
    f32 zz;

    ASSERTMSGLINE(0x5C9, m, "MTXQuat():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x5CA, q, "MTXQuat():  NULL QuaternionPtr 'q' ");
    ASSERTMSGLINE(0x5CB, q->x || q->y || q->z || q->w,
                  "MTXQuat():  zero-value quaternion ");
    s = 2 /
        ((q->w * q->w) + ((q->z * q->z) + ((q->x * q->x) + (q->y * q->y))));
    xs = q->x * s;
    ys = q->y * s;
    zs = q->z * s;
    wx = q->w * xs;
    wy = q->w * ys;
    wz = q->w * zs;
    xx = q->x * xs;
    xy = q->x * ys;
    xz = q->x * zs;
    yy = q->y * ys;
    yz = q->y * zs;
    zz = q->z * zs;
    m[0][0] = (1 - (yy + zz));
    m[0][1] = (xy - wz);
    m[0][2] = (xz + wy);
    m[0][3] = 0;
    m[1][0] = (xy + wz);
    m[1][1] = (1 - (xx + zz));
    m[1][2] = (yz - wx);
    m[1][3] = 0;
    m[2][0] = (xz - wy);
    m[2][1] = (yz + wx);
    m[2][2] = (1 - (xx + yy));
    m[2][3] = 0;
}

void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    Vec vTmp;

    ASSERTMSGLINE(0x39, m, "MTXMultVec():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x3A, src, "MTXMultVec():  NULL MtxPtr 'src' ");
    ASSERTMSGLINE(0x3B, dst, "MTXMultVec():  NULL MtxPtr 'dst' ");

    vTmp.x = m[0][3] +
             ((m[0][2] * src->z) + ((m[0][0] * src->x) + (m[0][1] * src->y)));
    vTmp.y = m[1][3] +
             ((m[1][2] * src->z) + ((m[1][0] * src->x) + (m[1][1] * src->y)));
    vTmp.z = m[2][3] +
             ((m[2][2] * src->z) + ((m[2][0] * src->x) + (m[2][1] * src->y)));
    dst->x = vTmp.x;
    dst->y = vTmp.y;
    dst->z = vTmp.z;
}

void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z)
{ PSMTXIdentity(m); m[0][3]=x; m[1][3]=y; m[2][3]=z; }

f32 PSVECSquareMag(Vec* v)
{ return v->x*v->x + v->y*v->y + v->z*v->z; }


u32 PSMTXInverse(Mtx src, Mtx inv)
{
    Mtx tmp;
    f32 (*m)[4] = src == inv ? tmp : inv;
    f32 det = ((((src[2][1] * (src[0][2] * src[1][0])) +
                 ((src[2][2] * (src[0][0] * src[1][1])) +
                  (src[2][0] * (src[0][1] * src[1][2])))) -
                (src[0][2] * (src[2][0] * src[1][1]))) -
               (src[2][2] * (src[1][0] * src[0][1]))) -
              (src[1][2] * (src[0][0] * src[2][1]));
    if (det == 0.0f) return 0;
    det = 1.0f / det;
    m[0][0] = det * ((src[1][1] * src[2][2]) - (src[2][1] * src[1][2]));
    m[0][1] = det * -((src[0][1] * src[2][2]) - (src[2][1] * src[0][2]));
    m[0][2] = det * ((src[0][1] * src[1][2]) - (src[1][1] * src[0][2]));
    m[1][0] = det * -((src[1][0] * src[2][2]) - (src[2][0] * src[1][2]));
    m[1][1] = det * ((src[0][0] * src[2][2]) - (src[2][0] * src[0][2]));
    m[1][2] = det * -((src[0][0] * src[1][2]) - (src[1][0] * src[0][2]));
    m[2][0] = det * ((src[1][0] * src[2][1]) - (src[2][0] * src[1][1]));
    m[2][1] = det * -((src[0][0] * src[2][1]) - (src[2][0] * src[0][1]));
    m[2][2] = det * ((src[0][0] * src[1][1]) - (src[1][0] * src[0][1]));
    m[0][3] = (-m[0][0] * src[0][3]) - (m[0][1] * src[1][3]) - (m[0][2] * src[2][3]);
    m[1][3] = (-m[1][0] * src[0][3]) - (m[1][1] * src[1][3]) - (m[1][2] * src[2][3]);
    m[2][3] = (-m[2][0] * src[0][3]) - (m[2][1] * src[1][3]) - (m[2][2] * src[2][3]);
    if (m == tmp) PSMTXCopy(tmp, inv);
    return 1;
}

void PSMTXTranspose(Mtx src, Mtx dst)
{
    Mtx tmp;
    f32 (*out)[4] = src == dst ? tmp : dst;
    out[0][0]=src[0][0]; out[0][1]=src[1][0]; out[0][2]=src[2][0]; out[0][3]=0.0f;
    out[1][0]=src[0][1]; out[1][1]=src[1][1]; out[1][2]=src[2][1]; out[1][3]=0.0f;
    out[2][0]=src[0][2]; out[2][1]=src[1][2]; out[2][2]=src[2][2]; out[2][3]=0.0f;
    if (out == tmp) PSMTXCopy(tmp, dst);
}

void MTXRotRad(Mtx m, char axis, f32 rad)
{
    f32 s = sinf(rad), c = cosf(rad);
    PSMTXIdentity(m);
    switch (axis | 0x20) {
    case 'x': m[1][1]=c; m[1][2]=-s; m[2][1]=s; m[2][2]=c; break;
    case 'y': m[0][0]=c; m[0][2]=s; m[2][0]=-s; m[2][2]=c; break;
    case 'z': m[0][0]=c; m[0][1]=-s; m[1][0]=s; m[1][1]=c; break;
    default: break;
    }
}

void MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n,
                     f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 inv = 1.0f / (r - l);
    m[0][0]=scaleS*(2.0f*n*inv); m[0][1]=0.0f; m[0][2]=scaleS*(inv*(r+l))-transS; m[0][3]=0.0f;
    inv = 1.0f / (t - b);
    m[1][0]=0.0f; m[1][1]=scaleT*(2.0f*n*inv); m[1][2]=scaleT*(inv*(t+b))-transT; m[1][3]=0.0f;
    m[2][0]=0.0f; m[2][1]=0.0f; m[2][2]=-1.0f; m[2][3]=0.0f;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS,
                         f32 scaleT, f32 transS, f32 transT)
{
    f32 cot = 1.0f / tanf(0.5f * fovY * 0.017453293f);
    m[0][0]=scaleS*(cot/aspect); m[0][1]=0.0f; m[0][2]=-transS; m[0][3]=0.0f;
    m[1][0]=0.0f; m[1][1]=cot*scaleT; m[1][2]=-transT; m[1][3]=0.0f;
    m[2][0]=0.0f; m[2][1]=0.0f; m[2][2]=-1.0f; m[2][3]=0.0f;
}

void MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS,
                   f32 scaleT, f32 transS, f32 transT)
{
    f32 inv = 1.0f / (r - l);
    m[0][0]=2.0f*inv*scaleS; m[0][1]=0.0f; m[0][2]=0.0f; m[0][3]=transS + scaleS*(inv*-(r+l));
    inv = 1.0f / (t - b);
    m[1][0]=0.0f; m[1][1]=2.0f*inv*scaleT; m[1][2]=0.0f; m[1][3]=transT + scaleT*(inv*-(t+b));
    m[2][0]=0.0f; m[2][1]=0.0f; m[2][2]=0.0f; m[2][3]=1.0f;
}
