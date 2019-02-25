/*
 * Copyright (c) 2002-2018 Balabit
 * Copyright (c) 1998-2018 Balázs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "timeutils/wallclocktime.h"
#include "timeutils/strptime-tz.h"

void
wall_clock_time_unset(WallClockTime *self)
{
  WallClockTime val = WALL_CLOCK_TIME_INIT;
  *self = val;
}

gchar *
wall_clock_time_strptime(WallClockTime *wct, const gchar *format, const gchar *input)
{
  return strptime_with_tz(input, format, &wct->tm, &wct->wct_gmtoff, &wct->wct_zone);
}
