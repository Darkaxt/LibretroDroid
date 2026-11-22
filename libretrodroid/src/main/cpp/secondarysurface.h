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

#ifndef LIBRETRODROID_SECONDARYSURFACE_H
#define LIBRETRODROID_SECONDARYSURFACE_H

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "shaderpreset.h"

namespace libretrodroid {

/**
 * Presents a crop of the core frame on a second Android surface (for example a Presentation on
 * another display), drawn from the GL thread on the same EGL context right after the main frame.
 *
 * setWindow() may be called from any thread; render() and onContextLost() run on the GL thread.
 * The mutex guarantees the EGL surface is never current while it is destroyed, so the caller may
 * release its Surface as soon as setWindow(nullptr) returns.
 */
class SecondarySurface {
public:
    struct Crop {
        float x0 = 0.0F;
        float y0 = 0.0F;
        float x1 = 1.0F;
        float y1 = 1.0F;
    };

    ~SecondarySurface();

    /** Takes a reference to window (nullptr detaches). */
    void setWindow(ANativeWindow* window);

    /** Frame region shown on the secondary surface, in image space (y grows downwards). */
    void setCrop(Crop crop);

    bool isActive();

    /** Slang preset for this surface ("" for none); loaded on the GL thread at the next render. */
    void setShaderPreset(const std::string& path);

    /** Last preset chain cost in microseconds, 0 without a preset. */
    uint64_t getShaderPresetMicros();

    /**
     * Draws the crop of texture (sourceWidth x sourceHeight, sourceFormat); frameAspect is the
     * display aspect ratio of the whole frame.
     */
    void render(GLuint texture, unsigned sourceWidth, unsigned sourceHeight, GLenum sourceFormat,
                bool bottomLeftOrigin, float frameAspect);

    /** The GL context was recreated: forget GL objects and the EGL surface (rebuilt lazily). */
    void onContextLost();

    /**
     * Maps a touch at (x, y), normalized to the secondary surface, to a position in the whole
     * frame (0..1, y downwards), or (-10, -10) when it falls outside the shown crop.
     */
    std::pair<float, float> toFramePosition(float x, float y);

private:
    void destroySurfaceLocked();
    bool ensureSurfaceLocked();
    bool ensureProgramLocked();

    std::mutex mutex;
    ANativeWindow* window = nullptr;
    bool windowChanged = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint program = 0;
    GLint positionHandle = -1;
    GLint coordinateHandle = -1;
    GLint textureHandle = -1;

    ShaderPreset preset;
    std::string presetPath;
    std::optional<std::string> pendingPreset;

    Crop crop;
    // Letterboxed content bounds on the secondary surface, normalized 0..1 (y downwards).
    float contentX0 = 0.0F;
    float contentY0 = 0.0F;
    float contentX1 = 1.0F;
    float contentY1 = 1.0F;
};

}

#endif
