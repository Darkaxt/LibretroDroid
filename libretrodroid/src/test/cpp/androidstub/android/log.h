/*
 *     Copyright (C) 2026  Darkaxt
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Host stand-in for <android/log.h> so runtime sources build in host tests.
#pragma once
#include <cstdarg>
#include <cstdio>

enum { ANDROID_LOG_VERBOSE = 2, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL };

inline int __android_log_print(int, const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "%s: ", tag);
    int written = std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    return written;
}
