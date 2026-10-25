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
#ifndef DMATH_H
#define DMATH_H

/*
 * Deterministic math for the simulation.  libm's sinf/cosf may differ in the last
 * bit between platforms (Windows vs macOS vs Linux), which is enough to desync a
 * lockstep game.  These use only basic IEEE operations, so they give the same
 * result everywhere (the build must not contract a*b+c into fused multiply-adds).
 */

float dm_sinf(float x);
float dm_cosf(float x);

#endif /* DMATH_H */
