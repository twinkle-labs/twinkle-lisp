/*    
 * Copyright (C) 2020, Twinkle Labs, LLC.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <stddef.h>

#ifndef _WIN32
#include <sys/time.h>
#else

#include <windows.h>
#include <winsock2.h>

// Windows Specific
struct timezone {
    int tz_minuteswest; /* minutes west of Greenwich */
    int tz_dsttime;     /* type of dst correction */
};
#if 0
// already defined in winsock.h
// long on windows is always 32bit
struct timeval {
	long tv_sec;         /* seconds */
	long tv_usec;        /* microseconds */
};
#endif

static int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned __int64 t = 0;
        t |= ft.dwHighDateTime;
        t <<= 32;
        t |= ft.dwLowDateTime;

        // Convert to microseconds
        t /= 10;

        // Difference between 1/1/1970 and 1/1/1601
        t -= 11644473600000000ULL;

        // Convert microseconds to seconds
        tv->tv_sec = (long)(t / 1000000UL);
        tv->tv_usec = (long)(t % 1000000UL);
    }
    // We don't support tz
    return 0;
}

#endif // end windows code

/** Seconds (microseconds precision) since Jan 1, 1970 (unix time) */
double microtime(void)
{
    double ret;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    ret = (double)tv.tv_sec;
    ret += (1.0E-6 * (double)tv.tv_usec);

    return ret;
}

