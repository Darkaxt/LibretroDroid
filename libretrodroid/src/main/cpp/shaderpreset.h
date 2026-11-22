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


#ifndef LIBRETRODROID_SHADERPRESET_H
#define LIBRETRODROID_SHADERPRESET_H

#include <GLES3/gl3.h>
#include <cstdint>
#include <string>

namespace libretrodroid {

/**
 * A RetroArch slang preset run through librashader (loaded at runtime from librashader.so).
 * GL thread only. The chain renders into an owned output texture, which is then drawn to the
 * currently bound framebuffer.
 */
class ShaderPreset {
public:
    ShaderPreset() = default;
    ~ShaderPreset();
    ShaderPreset(const ShaderPreset&) = delete;
    ShaderPreset& operator=(const ShaderPreset&) = delete;

    /** Loads the preset at path; on failure the previous chain is kept and false is returned. */
    bool load(const std::string& path);
    void unload();
    bool isLoaded() const { return chain != nullptr; }

    /**
     * Runs the chain on the source texture into an outputWidth x outputHeight texture, then draws
     * that texture with the given clip-space vertices and texture coordinates (6 vertices each).
     */
    void render(GLuint source, unsigned sourceWidth, unsigned sourceHeight, GLenum sourceFormat,
                unsigned outputWidth, unsigned outputHeight,
                const float* vertices, const float* textureCoordinates);

    uint64_t getLastFrameMicros() const { return lastFrameMicros; }

private:
    bool ensureBlitProgram();
    /** One-shot: GL state left by the chain and output texture pixels, to logcat. */
    void logDiagnostics();
    static constexpr uint64_t DIAGNOSTIC_FRAME = 120;
    void releaseOutput();

    void* chain = nullptr; // libra_gl_filter_chain_t
    uint64_t frameCount = 0;
    uint64_t lastFrameMicros = 0;

    GLuint output = 0;
    unsigned outputWidth = 0;
    unsigned outputHeight = 0;

    GLuint blitProgram = 0;
    GLint blitPosition = -1;
    GLint blitCoordinate = -1;
    GLint blitTexture = -1;
};

}

#endif
