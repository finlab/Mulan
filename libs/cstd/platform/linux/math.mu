// cstd.math — <math.h> for Linux (glibc).
//
// Raw binding (layer L1): C99 `double` and `float` functions. The classification macros
// (`isnan` / `isinf` / `isfinite` / `signbit` / `fpclassify`) are macros in C, not symbols; `isnan` is
// provided as a Mu function, the others are left to `std.math` (which will use bit inspection).
// `long double` is the x87 80-bit extended type here, so the `l` variants are not bound.

public unit cstd.math;

use std.ctypes;

// ---------------------------------------------------------------- absolute / remainder
extern double fabs(double x);
extern double fmod(double x, double y);
extern double remainder(double x, double y);
extern double remquo(double x, double y, int* quotient);
extern double fma(double x, double y, double z);
extern double fmax(double x, double y);
extern double fmin(double x, double y);
extern double fdim(double x, double y);

extern float fabsf(float x);
extern float fmodf(float x, float y);
extern float fmaxf(float x, float y);
extern float fminf(float x, float y);

// ---------------------------------------------------------------- exponential / logarithmic
extern double exp(double x);
extern double exp2(double x);
extern double expm1(double x);
extern double log(double x);
extern double log10(double x);
extern double log1p(double x);
extern double log2(double x);
extern double pow(double base, double exponent);
extern double sqrt(double x);
extern double cbrt(double x);
extern double hypot(double x, double y);

extern float expf(float x);
extern float logf(float x);
extern float powf(float base, float exponent);
extern float sqrtf(float x);
extern float cbrtf(float x);
extern float hypotf(float x, float y);

// ---------------------------------------------------------------- trigonometric
extern double sin(double x);
extern double cos(double x);
extern double tan(double x);
extern double asin(double x);
extern double acos(double x);
extern double atan(double x);
extern double atan2(double y, double x);
extern double sinh(double x);
extern double cosh(double x);
extern double tanh(double x);
extern double asinh(double x);
extern double acosh(double x);
extern double atanh(double x);

extern float sinf(float x);
extern float cosf(float x);
extern float tanf(float x);
extern float atan2f(float y, float x);

// ---------------------------------------------------------------- rounding
extern double ceil(double x);
extern double floor(double x);
extern double trunc(double x);
extern double round(double x);
extern c_long lround(double x);
extern long llround(double x);
extern double nearbyint(double x);
extern double rint(double x);

extern float ceilf(float x);
extern float floorf(float x);
extern float truncf(float x);
extern float roundf(float x);

// ---------------------------------------------------------------- decomposition
extern double frexp(double x, int* exponent);
extern double ldexp(double x, int exponent);
extern double modf(double x, double* integer_part);
extern double scalbn(double x, int exponent);
extern double copysign(double magnitude, double sign);
extern double nextafter(double from, double to);

extern float frexpf(float x, int* exponent);
extern float ldexpf(float x, int exponent);
extern float modff(float x, float* integer_part);

// ---------------------------------------------------------------- error / gamma
extern double erf(double x);
extern double erfc(double x);
extern double tgamma(double x);
extern double lgamma(double x);

// Not a symbol in C (it is a macro): `x != x` is the portable test, and it is exactly what the macro
// amounts to for IEEE-754 doubles.
public bool isnan(double x) { return x != x; }
