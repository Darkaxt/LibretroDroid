#pragma once
#include <cstdio>
#define LOGE(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define LOGD(...) ((void) 0)
#define LOGI(...) ((void) 0)
