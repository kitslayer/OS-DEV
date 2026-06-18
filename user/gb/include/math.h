/* math.h — DOOM libc shim (subset actually referenced: sin, tan, atan, fabs). */
#ifndef _OSDEV_MATH_H
#define _OSDEV_MATH_H

double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double fabs(double x);
double sqrt(double x);
double pow(double base, double exp);
double floor(double x);
double ceil(double x);
double exp(double x);
double log(double x);
double fmod(double x, double y);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif /* _OSDEV_MATH_H */
