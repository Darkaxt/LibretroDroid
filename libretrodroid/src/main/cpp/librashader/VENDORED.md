# librashader C headers

`librashader.h` and `librashader_ld.h` are copied unmodified from
https://github.com/SnowflakePowered/librashader at commit `9d0116f86aadb866b0275b19a9fe48ee29b165e7`
(librashader 0.12.1, ABI 2, API version 5). The headers are MIT-licensed (SPDX notice in each file); the
librashader implementation (`librashader.so`) is MPL-2.0, and its source is the commit above.

The runtime loads `librashader.so` (the `librashader-capi` cdylib, OpenGL runtime only) with `dlopen` through
`librashader_ld.h`, so shader presets are optional: without the library the built-in shaders are used.
The app ships the library built from the same commit for arm64-v8a.
