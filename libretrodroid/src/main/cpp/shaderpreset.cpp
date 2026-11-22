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


#include "shaderpreset.h"

#include <EGL/egl.h>
#include <chrono>
#include <dlfcn.h>

#include "log.h"

#define LIBRA_RUNTIME_OPENGL
#include "librashader/librashader_ld.h"

namespace libretrodroid {

namespace {

uint64_t nowMicros() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

libra_instance_t& library() {
    static libra_instance_t instance = librashader_load_instance();
    return instance;
}

const void* loadGlFunction(const char* name) {
    static void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    void* address = gles != nullptr ? dlsym(gles, name) : nullptr;
    if (address == nullptr) address = (void*) eglGetProcAddress(name);
    return address;
}

void logError(const char* what, libra_error_t error) {
    char* message = nullptr;
    if (library().error_write(error, &message) == 0 && message != nullptr) {
        LOGE("librashader %s: %s", what, message);
        library().error_free_string(&message);
    } else {
        LOGE("librashader %s failed", what);
    }
    library().error_free(&error);
}

const char* BLIT_VERTEX =
    "#version 300 es\n"
    "in vec4 vPosition;\n"
    "in vec2 vCoordinate;\n"
    "out vec2 coordinate;\n"
    "void main() {\n"
    "    coordinate = vCoordinate;\n"
    "    gl_Position = vPosition;\n"
    "}\n";

const char* BLIT_FRAGMENT =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D source;\n"
    "in vec2 coordinate;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    color = vec4(texture(source, coordinate).rgb, 1.0);\n"
    "}\n";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("ShaderPreset: blit shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}

ShaderPreset::~ShaderPreset() {
    unload();
    if (blitProgram != 0) glDeleteProgram(blitProgram);
}

bool ShaderPreset::load(const std::string& path) {
    libra_instance_t& lib = library();
    if (!lib.instance_loaded) {
        LOGE("ShaderPreset: librashader.so is not available");
        return false;
    }

    uint64_t started = nowMicros();
    libra_shader_preset_t preset = nullptr;
    libra_error_t error = lib.preset_create(path.c_str(), &preset);
    if (error != nullptr) {
        logError("preset", error);
        return false;
    }

    filter_chain_gl_opt_t options {};
    options.version = LIBRASHADER_CURRENT_VERSION;
    options.glsl_version = 0; // detect from the context (GLSL ES 3.20 on a GLES 3.2 device)
    options.use_dsa = false;
    options.force_no_mipmaps = false;
    options.disable_cache = true;

    libra_gl_filter_chain_t created = nullptr;
    error = lib.gl_filter_chain_create(&preset, loadGlFunction, &options, &created);
    if (preset != nullptr) lib.preset_free(&preset);
    // librashader may leave its own GL objects bound; restore what the renderer expects.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    if (error != nullptr) {
        logError("filter chain", error);
        return false;
    }

    unload();
    chain = created;
    frameCount = 0;
    LOGI("ShaderPreset: loaded %s in %llu ms", path.c_str(),
         (unsigned long long) ((nowMicros() - started) / 1000));
    return true;
}

void ShaderPreset::unload() {
    if (chain != nullptr) {
        auto handle = (libra_gl_filter_chain_t) chain;
        library().gl_filter_chain_free(&handle);
        chain = nullptr;
    }
    releaseOutput();
}

void ShaderPreset::abandon() {
    chain = nullptr;
    output = 0;
    outputWidth = 0;
    outputHeight = 0;
    blitProgram = 0;
}

void ShaderPreset::releaseOutput() {
    if (output != 0) {
        glDeleteTextures(1, &output);
        output = 0;
    }
    outputWidth = 0;
    outputHeight = 0;
}

bool ShaderPreset::ensureBlitProgram() {
    if (blitProgram != 0) return true;
    GLuint vertex = compile(GL_VERTEX_SHADER, BLIT_VERTEX);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, BLIT_FRAGMENT);
    if (vertex == 0 || fragment == 0) return false;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOGE("ShaderPreset: blit program does not link");
        glDeleteProgram(program);
        return false;
    }
    blitProgram = program;
    blitPosition = glGetAttribLocation(program, "vPosition");
    blitCoordinate = glGetAttribLocation(program, "vCoordinate");
    blitTexture = glGetUniformLocation(program, "source");
    return true;
}

void ShaderPreset::logDiagnostics() {
    GLint cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint stencil = glIsEnabled(GL_STENCIL_TEST), depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND);
    GLboolean mask[4] = {};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLint sampler = 0, unit = 0, vao = 0, program = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    GLenum glError = glGetError();

    GLuint probe = 0;
    glGenFramebuffers(1, &probe);
    glBindFramebuffer(GL_FRAMEBUFFER, probe);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    uint8_t center[4] = {}, quarter[4] = {};
    glReadPixels((GLint) outputWidth / 2, (GLint) outputHeight / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels((GLint) outputWidth / 4, (GLint) outputHeight / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, quarter);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &probe);
    LOGI("ShaderPreset diag: out=%ux%u fbo=0x%x glError=0x%x cull=%d scissor=%d stencil=%d depth=%d blend=%d "
         "mask=%d%d%d%d activeUnit=0x%x sampler0=%d vao=%d program=%d center=%u,%u,%u,%u quarter=%u,%u,%u,%u",
         outputWidth, outputHeight, status, glError, cull, scissor, stencil, depth, blend,
         mask[0], mask[1], mask[2], mask[3], unit, sampler, vao, program,
         center[0], center[1], center[2], center[3], quarter[0], quarter[1], quarter[2], quarter[3]);
}

void ShaderPreset::render(GLuint source, unsigned sourceWidth, unsigned sourceHeight, GLenum sourceFormat,
                          unsigned width, unsigned height,
                          const float* vertices, const float* textureCoordinates) {
    if (chain == nullptr || width == 0 || height == 0 || sourceWidth == 0 || sourceHeight == 0) return;
    if (!ensureBlitProgram()) return;
    if (output == 0 || width != outputWidth || height != outputHeight) {
        // Immutable storage: a new size needs a new texture.
        releaseOutput();
        glGenTextures(1, &output);
        glBindTexture(GL_TEXTURE_2D, output);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei) width, (GLsizei) height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        outputWidth = width;
        outputHeight = height;
    }

    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);

    uint64_t started = nowMicros();
    libra_image_gl_t input { source, sourceFormat, sourceWidth, sourceHeight };
    libra_image_gl_t target { output, GL_RGBA8, width, height };
    auto handle = (libra_gl_filter_chain_t) chain;
    libra_error_t error = library().gl_filter_chain_frame(&handle, (size_t) frameCount++, input, target,
                                                          nullptr, nullptr, nullptr);
    lastFrameMicros = nowMicros() - started;

    if (frameCount == DIAGNOSTIC_FRAME) logDiagnostics();

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) framebuffer);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (error != nullptr) {
        logError("frame", error);
        unload();
        return;
    }

    // Reset the state librashader's passes may leave behind before drawing to the screen.
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindSampler(0, 0);
    glUseProgram(blitProgram);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glVertexAttribPointer(blitPosition, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(blitPosition);
    glVertexAttribPointer(blitCoordinate, 2, GL_FLOAT, GL_FALSE, 0, textureCoordinates);
    glEnableVertexAttribArray(blitCoordinate);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, output);
    glUniform1i(blitTexture, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(blitPosition);
    glDisableVertexAttribArray(blitCoordinate);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

}
