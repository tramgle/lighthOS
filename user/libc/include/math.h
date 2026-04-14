#ifndef _MATH_H
#define _MATH_H

/* Integer-mode Lua doesn't call most of these, but luaconf.h references
   HUGE_VAL, INFINITY, NAN in places. Stub what's needed. */

#define HUGE_VAL  (1e300 * 1e300)
#define INFINITY  HUGE_VAL
#define NAN       (0.0 / 0.0)

double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double a, double b);
double fmod(double a, double b);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);
double hypot(double a, double b);
double trunc(double x);

#endif
