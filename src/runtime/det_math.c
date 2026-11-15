/*
 * det_math.c - the guest's elementary maths, the same on every host.
 *
 * The lifted code (fpatan -> atan2, fsin/fcos/fptan/fsincos) and the libm
 * imports the HLE answers (hle_libc.c: sin, cos, acos, pow, expf, ...) come
 * through lg_m_*. Normally that is the host's libm, as before. In
 * deterministic mode (netplay.h) it is the bundled fdlibm
 * (third_party/fdlibm), so a Mac and a Quest - Apple's libm and Bionic's,
 * which may round differently in the last bit - compute the same numbers.
 * sqrt, fabs, floor/ceil/trunc/nearbyint, fmod, frexp, modf and ldexp are
 * exact in IEEE arithmetic on any host and are left to it.
 *
 * Functions fdlibm's subset does not have are built from ones it does
 * (asin/acos from atan2 and sqrt, the hyperbolics from exp): a few ulp off
 * the host's, but the same everywhere, which is what matters here. The float
 * versions are the double ones rounded once.
 */
#include <math.h>

int g_lg_det_math;      /* set by netplay.c when deterministic mode is on */

double lg_fd_sqrt(double x) { return sqrt(x); }   /* fdlibm's pow wants it: IEEE sqrt is exact anywhere */

double lg_fd_sin(double), lg_fd_cos(double), lg_fd_tan(double), lg_fd_atan(double);
double lg_fd_atan2(double, double), lg_fd_exp(double), lg_fd_log(double), lg_fd_log10(double);
double lg_fd_pow(double, double);

static double fd_asin(double x)
{
    if (!(x >= -1.0 && x <= 1.0)) return (x - x) / (x - x);   /* NaN, as libm */
    return lg_fd_atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}
static double fd_acos(double x)
{
    if (!(x >= -1.0 && x <= 1.0)) return (x - x) / (x - x);
    return lg_fd_atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}
static double fd_sinh(double x)
{
    double a = fabs(x);
    if (a < 3.7252902984e-09) return x;                       /* 2^-28 */
    double t = lg_fd_exp(a), r = 0.5 * (t - 1.0 / t);
    return x < 0 ? -r : r;
}
static double fd_cosh(double x) { double t = lg_fd_exp(fabs(x)); return 0.5 * (t + 1.0 / t); }
static double fd_tanh(double x)
{
    double a = fabs(x);
    if (a > 22.0) return x < 0 ? -1.0 : 1.0;
    if (a < 3.7252902984e-09) return x;
    double t = lg_fd_exp(2.0 * a), r = (t - 1.0) / (t + 1.0);
    return x < 0 ? -r : r;
}

#define D1(name, fd) double lg_m_##name(double x) { return g_lg_det_math ? fd(x) : name(x); } \
                     float lg_m_##name##f(float x) { return g_lg_det_math ? (float)fd((double)x) : name##f(x); }
D1(sin, lg_fd_sin)   D1(cos, lg_fd_cos)   D1(tan, lg_fd_tan)   D1(atan, lg_fd_atan)
D1(asin, fd_asin)    D1(acos, fd_acos)
D1(exp, lg_fd_exp)   D1(log, lg_fd_log)   D1(log10, lg_fd_log10)
D1(sinh, fd_sinh)    D1(cosh, fd_cosh)    D1(tanh, fd_tanh)
double lg_m_atan2(double y, double x) { return g_lg_det_math ? lg_fd_atan2(y, x) : atan2(y, x); }
float  lg_m_atan2f(float y, float x)  { return g_lg_det_math ? (float)lg_fd_atan2(y, x) : atan2f(y, x); }
double lg_m_pow(double x, double y)   { return g_lg_det_math ? lg_fd_pow(x, y) : pow(x, y); }
float  lg_m_powf(float x, float y)    { return g_lg_det_math ? (float)lg_fd_pow(x, y) : powf(x, y); }
