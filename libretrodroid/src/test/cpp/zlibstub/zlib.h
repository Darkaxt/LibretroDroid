// Pass-through zlib stand-in for host tests of rewind.cpp (the host toolchain has no zlib).
#pragma once
#include <cstring>
typedef unsigned long uLong;
typedef unsigned long uLongf;
typedef unsigned char Bytef;
#define Z_OK 0
#define Z_BUF_ERROR (-5)
inline uLong compressBound(uLong n) { return n + 16; }
inline int compress2(Bytef* d, uLongf* dn, const Bytef* s, uLong sn, int) {
    if (*dn < sn) return Z_BUF_ERROR; std::memcpy(d, s, sn); *dn = sn; return Z_OK; }
inline int uncompress(Bytef* d, uLongf* dn, const Bytef* s, uLong sn) {
    if (*dn < sn) return Z_BUF_ERROR; std::memcpy(d, s, sn); *dn = sn; return Z_OK; }
