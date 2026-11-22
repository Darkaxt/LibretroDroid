/*
 *     Copyright (C) 2019  Filippo Scognamiglio
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

#include <GLES3/gl3.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <cstdlib>
#include <string>
#include <cmath>
#include <utility>
#include <sstream>
#include <algorithm>

#include "../../libretro-common/include/libretro.h"

#include "log.h"

#include "video.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es3/imagerendereres3.h"
#include "renderers/es2/imagerendereres2.h"

namespace libretrodroid {

static void printGLString(const char *name, GLenum s) {
    const char *v = (const char *) glGetString(s);
    LOGI("GL %s = %s\n", name, v);
}

GLuint loadShader(GLenum shaderType, const char* pSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, nullptr, buf);
                    LOGE("Could not compile shader %d:\n%s\n",
                         shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint createProgram(const char* pVertexSource, const char* pFragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
    if (!vertexShader) {
        return 0;
    }

    GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
    if (!pixelShader) {
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        glAttachShader(program, pixelShader);
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, nullptr, buf);
                    LOGE("Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

void Video::updateProgram() {
    if (loadedShaderType.has_value() && loadedShaderType.value() == requestedShaderConfig) {
        return;
    }

    loadedShaderType = requestedShaderConfig;

    auto shaders = ShaderManager::getShader(requestedShaderConfig);

    shadersChain = {};

    std::for_each(shaders.passes.begin(), shaders.passes.end(), [&](const auto& item){
        auto shader = ShaderChainEntry { };

        shader.gProgram = createProgram(item.vertex.data(), item.fragment.data());
        if (!shader.gProgram) {
            LOGE("Could not create gl program.");
            throw std::runtime_error("Cannot create gl program");
        }

        shader.gvPositionHandle = glGetAttribLocation(shader.gProgram, "vPosition");

        shader.gvCoordinateHandle = glGetAttribLocation(shader.gProgram, "vCoordinate");

        shader.gTextureHandle = glGetUniformLocation(shader.gProgram, "texture");

        shader.gPreviousPassTextureHandle = glGetUniformLocation(shader.gProgram, "previousPass");

        shader.gTextureSizeHandle = glGetUniformLocation(shader.gProgram, "textureSize");

        shader.gScreenDensityHandle = glGetUniformLocation(shader.gProgram, "screenDensity");

        shadersChain.push_back(shader);
    });

    renderer->setShaders(shaders);
}

void Video::renderFrame() {
    if (skipDuplicateFrames && !isDirty) return;
    isDirty = false;

    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (immersiveModeEnabled) {
        immersiveMode.renderBackground(
            videoLayout.getScreenWidth(),
            videoLayout.getScreenHeight(),
            videoLayout.getBackgroundVertices(),
            videoLayout.getRelativeForegroundBounds(),
            videoLayout.getFramebufferVertices().data(),
            renderer->getTexture()
        );
    }

    applyShaderPreset();
    if (preset.isLoaded()) {
        renderPreset();
        return;
    }

    updateProgram();
    for (int i = 0; i < shadersChain.size(); ++i) {
        auto shader = shadersChain[i];
        auto passData = renderer->getPassData(i);
        auto isLastPass = i == shadersChain.size() - 1;

        glBindFramebuffer(GL_FRAMEBUFFER, passData.framebuffer.value_or(0));

        glViewport(
            0,
            0,
            passData.width.value_or(videoLayout.getScreenWidth()),
            passData.height.value_or(videoLayout.getScreenHeight())
        );

        glUseProgram(shader.gProgram);

        auto vertices = isLastPass ? videoLayout.getForegroundVertices() : videoLayout.getFramebufferVertices();
        glVertexAttribPointer(shader.gvPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
        glEnableVertexAttribArray(shader.gvPositionHandle);

        auto coordinates = videoLayout.getTextureCoordinates();
        glVertexAttribPointer(shader.gvCoordinateHandle, 2, GL_FLOAT, GL_FALSE, 0, coordinates.data());
        glEnableVertexAttribArray(shader.gvCoordinateHandle);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->getTexture());
        glUniform1i(shader.gTextureHandle, 0);

        if (shader.gPreviousPassTextureHandle != -1 && passData.texture.has_value()) {
            glActiveTexture(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, passData.texture.value());
            glUniform1i(shader.gPreviousPassTextureHandle, 1);
        }

        glUniform2f(shader.gTextureSizeHandle, getTextureWidth(), getTextureHeight());

        glUniform1f(shader.gScreenDensityHandle, getScreenDensity());

        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDisableVertexAttribArray(shader.gvPositionHandle);
        glDisableVertexAttribArray(shader.gvCoordinateHandle);

        if (shader.gPreviousPassTextureHandle != -1 && passData.texture.has_value()) {
            glActiveTexture(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        glUseProgram(0);
    }
}

void Video::setShaderPreset(const std::string& path) {
    std::lock_guard<std::mutex> lock(presetLock);
    pendingPreset = path;
    isDirty = true;
}

void Video::applyShaderPreset() {
    std::optional<std::string> path;
    {
        std::lock_guard<std::mutex> lock(presetLock);
        path.swap(pendingPreset);
    }
    if (!path.has_value()) return;
    if (path->empty()) {
        preset.unload();
    } else if (!preset.load(*path)) {
        preset.unload();
        LOGE("Shader preset %s not loaded; using the built-in shaders", path->c_str());
    }
}

void Video::renderPreset() {
    unsigned sourceWidth = renderer->lastFrameSize.first;
    unsigned sourceHeight = renderer->lastFrameSize.second;
    if (sourceWidth == 0 || sourceHeight == 0) return;

    // Output resolution: the on-screen foreground size, extended over the cropped-away part of
    // the frame so the shown region maps 1:1 to screen pixels.
    auto& bounds = videoLayout.getRelativeForegroundBounds();
    auto& coordinates = videoLayout.getTextureCoordinates();
    float u0 = coordinates[0], u1 = coordinates[0], v0 = coordinates[1], v1 = coordinates[1];
    for (size_t i = 0; i < coordinates.size(); i += 2) {
        u0 = std::min(u0, coordinates[i]);
        u1 = std::max(u1, coordinates[i]);
        v0 = std::min(v0, coordinates[i + 1]);
        v1 = std::max(v1, coordinates[i + 1]);
    }
    float shownWidth = (bounds[2] - bounds[0]) * (float) videoLayout.getScreenWidth();
    float shownHeight = (bounds[3] - bounds[1]) * (float) videoLayout.getScreenHeight();
    auto outputWidth = (unsigned) std::lround(shownWidth / std::max(u1 - u0, 0.01F));
    auto outputHeight = (unsigned) std::lround(shownHeight / std::max(v1 - v0, 0.01F));
    outputWidth = std::min(std::max(outputWidth, sourceWidth), 4096u);
    outputHeight = std::min(std::max(outputHeight, sourceHeight), 4096u);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, videoLayout.getScreenWidth(), videoLayout.getScreenHeight());
    GLenum format = pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? GL_RGBA8 : GL_RGB565;
    preset.render((GLuint) renderer->getTexture(), sourceWidth, sourceHeight, format,
                  outputWidth, outputHeight,
                  videoLayout.getForegroundVertices().data(), coordinates.data());
}

float Video::getScreenDensity() {
    return std::min(videoLayout.getScreenWidth() / getTextureWidth(), videoLayout.getScreenHeight() / getTextureHeight());
}

float Video::getTextureWidth() {
    return renderer->lastFrameSize.first;
}

float Video::getTextureHeight() {
    return renderer->lastFrameSize.second;
}

void Video::onNewFrame(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (data != nullptr) {
        renderer->onNewFrame(data, width, height, pitch);
        isDirty = true;
    }
}

void Video::updateScreenSize(unsigned width, unsigned height) {
    videoLayout.updateScreenSize(width, height);
}

void Video::updateViewportSize(Rect viewportRect) {
    videoLayout.updateViewportSize(viewportRect);
}

void Video::updateViewportAlignment(unsigned int viewportAlignment) {
    videoLayout.updateViewportAlignment(viewportAlignment);
}

void Video::updateRendererSize(unsigned int width, unsigned int height) {
    LOGD("Updating renderer size: %d x %d", width, height);
    renderer->updateRenderedResolution(width, height);
}

void Video::updateRotation(float rotation) {
    videoLayout.updateRotation(rotation);
}

Video::Video(
    RenderingOptions renderingOptions,
    ShaderManager::Config shaderConfig,
    bool bottomLeftOrigin,
    float rotation,
    bool skipDuplicateFrames,
    bool immersiveModeEnabled,
    Rect viewportRect,
    ImmersiveMode::Config immersiveModeConfig,
    unsigned int viewportAlignment
) :
    requestedShaderConfig(std::move(shaderConfig)),
    skipDuplicateFrames(skipDuplicateFrames),
    immersiveModeEnabled(immersiveModeEnabled),
    immersiveMode(immersiveModeConfig),
    videoLayout(bottomLeftOrigin, rotation, viewportRect, viewportAlignment) {

    printGLString("Version", GL_VERSION);
    printGLString("Vendor", GL_VENDOR);
    printGLString("Renderer", GL_RENDERER);
    printGLString("Extensions", GL_EXTENSIONS);
    initializeGLESLogCallbackIfNeeded();

    LOGI("Initializing graphics");

    glViewport(0, 0, videoLayout.getScreenWidth(), videoLayout.getScreenHeight());

    glUseProgram(0);

    initializeRenderer(renderingOptions);
}

void Video::updateShaderType(ShaderManager::Config shaderConfig) {
    requestedShaderConfig = std::move(shaderConfig);
}

void Video::initializeRenderer(RenderingOptions renderingOptions) {
    auto shaders = ShaderManager::getShader(requestedShaderConfig);

    if (renderingOptions.hardwareAccelerated) {
        renderer = new FramebufferRenderer(
            renderingOptions.width,
            renderingOptions.height,
            renderingOptions.useDepth,
            renderingOptions.useStencil,
            std::move(shaders)
        );
    } else {
        if (renderingOptions.openglESVersion >= 3) {
            renderer = new ImageRendererES3();
        } else {
            renderer = new ImageRendererES2();
        }
    }

    pixelFormat = renderingOptions.pixelFormat;
    renderer->setPixelFormat(renderingOptions.pixelFormat);
    updateProgram();
}

void Video::updateAspectRatio(float aspectRatio) {
    videoLayout.updateAspectRatio(aspectRatio);
}

} //namespace libretrodroid
