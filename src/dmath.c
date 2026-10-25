/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "dmath.h"

// Polynomial coefficients for sin and cos on [-pi/4, pi/4] (from musl's __sindf/__cosdf).
static const float S1 = -1.6666666641626524e-01f,
                   S2 =  8.3333293858894632e-03f,
                   S3 = -1.9839334836096632e-04f,
                   S4 =  2.7183114939898219e-06f;
static const float C1 = -4.9999999725103100e-01f,
                   C2 =  4.1666623323739063e-02f,
                   C3 = -1.3886763774609929e-03f,
                   C4 =  2.4390448796277409e-05f;

static float sin_kernel(float r)
{
	const float r2 = r * r;
	return r + r * r2 * (S1 + r2 * (S2 + r2 * (S3 + r2 * S4)));
}

static float cos_kernel(float r)
{
	const float r2 = r * r;
	return 1.0f + r2 * (C1 + r2 * (C2 + r2 * (C3 + r2 * C4)));
}

/**
 * Reduces x to r in [-pi/4, pi/4] with x = r + k * pi/2, returning k mod 4.
 * pi/2 is split in two parts so that k * pi/2 is subtracted without losing precision.
 */
static int reduce(float x, float *r)
{
	const float pio2_hi = 1.5707963705062866e+00f,
	            pio2_lo = -4.3711388286737929e-08f,
	            two_over_pi = 6.3661977236758134e-01f;

	const float t = x * two_over_pi;
	const int k = (int)(t >= 0 ? t + 0.5f : t - 0.5f);

	*r = (x - k * pio2_hi) - k * pio2_lo;
	return k & 3;
}

float dm_sinf(float x)
{
	float r;
	switch (reduce(x, &r))
	{
	case 0:  return sin_kernel(r);
	case 1:  return cos_kernel(r);
	case 2:  return -sin_kernel(r);
	default: return -cos_kernel(r);
	}
}

float dm_cosf(float x)
{
	float r;
	switch (reduce(x, &r))
	{
	case 0:  return cos_kernel(r);
	case 1:  return -sin_kernel(r);
	case 2:  return -cos_kernel(r);
	default: return sin_kernel(r);
	}
}
