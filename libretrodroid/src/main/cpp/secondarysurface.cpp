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

#include "secondarysurface.h"

#include "log.h"

namespace libretrodroid {

namespace {

const char* VERTEX_SHADER =
    "attribute vec2 vPosition;\n"
    "attribute vec2 vCoordinate;\n"
    "varying vec2 coordinate;\n"
    "void main() {\n"
    "  coordinate = vCoordinate;\n"
    "  gl_Position = vec4(vPosition, 0.0, 1.0);\n"
    "}\n";

const char* FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D texture;\n"
    "varying vec2 coordinate;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(texture2D(texture, coordinate).rgb, 1.0);\n"
    "}\n";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        LOGE("SecondarySurface: shader compilation failed");
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}

SecondarySurface::~SecondarySurface() {
    std::lock_guard<std::mutex> lock(mutex);
    destroySurfaceLocked();
    if (window != nullptr) {
        ANativeWindow_release(window);
        window = nullptr;
    }
}

void SecondarySurface::setWindow(ANativeWindow* newWindow) {
    std::lock_guard<std::mutex> lock(mutex);
    destroySurfaceLocked();
    if (window != nullptr) ANativeWindow_release(window);
    window = newWindow;
    if (window != nullptr) ANativeWindow_acquire(window);
    windowChanged = true;
}

void SecondarySurface::setCrop(Crop newCrop) {
    std::lock_guard<std::mutex> lock(mutex);
    crop = newCrop;
}

bool SecondarySurface::isActive() {
    std::lock_guard<std::mutex> lock(mutex);
    return window != nullptr;
}

void SecondarySurface::destroySurfaceLocked() {
    if (surface != EGL_NO_SURFACE) {
        // Never current here: render() restores the main surface before releasing the mutex.
        eglDestroySurface(display, surface);
        surface = EGL_NO_SURFACE;
    }
}

bool SecondarySurface::ensureSurfaceLocked() {
    if (window == nullptr) return false;
    if (surface != EGL_NO_SURFACE && !windowChanged) return true;
    windowChanged = false;
    destroySurfaceLocked();

    display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLint configId = 0;
    eglQueryContext(display, context, EGL_CONFIG_ID, &configId);
    EGLint attributes[] = { EGL_CONFIG_ID, configId, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) {
        LOGE("SecondarySurface: no EGL config %d", configId);
        return false;
    }
    EGLint visual = 0;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &visual);
    ANativeWindow_setBuffersGeometry(window, 0, 0, visual);

    surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        LOGE("SecondarySurface: eglCreateWindowSurface failed 0x%x", eglGetError());
        return false;
    }
    LOGI("SecondarySurface: created EGL surface with config %d", configId);
    return true;
}

bool SecondarySurface::ensureProgramLocked() {
    if (program != 0) return true;
    GLuint vertex = compile(GL_VERTEX_SHADER, VERTEX_SHADER);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0) return false;
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        LOGE("SecondarySurface: program link failed");
        glDeleteProgram(program);
        program = 0;
        return false;
    }
    positionHandle = glGetAttribLocation(program, "vPosition");
    coordinateHandle = glGetAttribLocation(program, "vCoordinate");
    textureHandle = glGetUniformLocation(program, "texture");
    return true;
}

void SecondarySurface::render(GLuint texture, bool bottomLeftOrigin, float frameAspect) {
    std::lock_guard<std::mutex> lock(mutex);
    if (window == nullptr || texture == 0) return;

    EGLContext context = eglGetCurrentContext();
    EGLSurface mainDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface mainRead = eglGetCurrentSurface(EGL_READ);
    if (!ensureSurfaceLocked()) return;

    if (!eglMakeCurrent(display, surface, surface, context)) {
        LOGE("SecondarySurface: eglMakeCurrent failed 0x%x", eglGetError());
        eglMakeCurrent(display, mainDraw, mainRead, context);
        destroySurfaceLocked();
        windowChanged = true;
        return;
    }
    // A 60 Hz secondary display must never throttle the main display's frame loop.
    eglSwapInterval(display, 0);

    EGLint width = 0;
    EGLint height = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);

    if (ensureProgramLocked() && width > 0 && height > 0) {
        float cropWidth = crop.x1 - crop.x0;
        float cropHeight = crop.y1 - crop.y0;
        float contentAspect = frameAspect * cropWidth / cropHeight;
        float surfaceAspect = (float) width / (float) height;
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        if (contentAspect > surfaceAspect) {
            scaleY = surfaceAspect / contentAspect;
        } else {
            scaleX = contentAspect / surfaceAspect;
        }
        contentX0 = 0.5F - scaleX / 2.0F;
        contentX1 = 0.5F + scaleX / 2.0F;
        contentY0 = 0.5F - scaleY / 2.0F;
        contentY1 = 0.5F + scaleY / 2.0F;

        // Texture v follows image rows for software frames; hardware frames are bottom-up.
        float vTop = bottomLeftOrigin ? 1.0F - crop.y0 : crop.y0;
        float vBottom = bottomLeftOrigin ? 1.0F - crop.y1 : crop.y1;

        const GLfloat vertices[] = {
            -scaleX, -scaleY,
            -scaleX, +scaleY,
            +scaleX, -scaleY,
            +scaleX, -scaleY,
            -scaleX, +scaleY,
            +scaleX, +scaleY,
        };
        const GLfloat coordinates[] = {
            crop.x0, vBottom,
            crop.x0, vTop,
            crop.x1, vBottom,
            crop.x1, vBottom,
            crop.x0, vTop,
            crop.x1, vTop,
        };

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glVertexAttribPointer(positionHandle, 2, GL_FLOAT, GL_FALSE, 0, vertices);
        glEnableVertexAttribArray(positionHandle);
        glVertexAttribPointer(coordinateHandle, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
        glEnableVertexAttribArray(coordinateHandle);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(textureHandle, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(positionHandle);
        glDisableVertexAttribArray(coordinateHandle);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);

        eglSwapBuffers(display, surface);
    }

    eglMakeCurrent(display, mainDraw, mainRead, context);
}

void SecondarySurface::onContextLost() {
    std::lock_guard<std::mutex> lock(mutex);
    program = 0;
    destroySurfaceLocked();
    windowChanged = true;
}

std::pair<float, float> SecondarySurface::toFramePosition(float x, float y) {
    std::lock_guard<std::mutex> lock(mutex);
    if (x < contentX0 || x > contentX1 || y < contentY0 || y > contentY1) return { -10.0F, -10.0F };
    float relativeX = (x - contentX0) / (contentX1 - contentX0);
    float relativeY = (y - contentY0) / (contentY1 - contentY0);
    return {
        crop.x0 + relativeX * (crop.x1 - crop.x0),
        crop.y0 + relativeY * (crop.y1 - crop.y0)
    };
}

}
