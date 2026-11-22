/*
 *     Copyright (C) 2021  Filippo Scognamiglio
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

#include <jni.h>

#include <EGL/egl.h>
#include <android/native_window_jni.h>

#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <optional>

#include "libretrodroid.h"
#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "errorcodes.h"
#include "environment.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/jnistring.h"

namespace libretrodroid {

extern "C" {
#include "utils/utils.h"
#include "../../libretro-common/include/libretro.h"
#include "utils/libretrodroidexception.h"
}

extern "C" {

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_availableDisks(
    JNIEnv* env,
    jclass obj
) {
    return LibretroDroid::getInstance().availableDisks();
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_currentDisk(
    JNIEnv* env,
    jclass obj
) {
    return LibretroDroid::getInstance().currentDisk();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_changeDisk(
    JNIEnv* env,
    jclass obj,
    jint index
) {
    return LibretroDroid::getInstance().changeDisk(index);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_updateVariable(
    JNIEnv* env,
    jclass obj,
    jobject variable
) {
    Variable v = JavaUtils::variableFromJava(env, variable);
    Environment::getInstance().updateVariable(v.key, v.value);
}

JNIEXPORT jobjectArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getVariables(
    JNIEnv* env,
    jclass obj
) {
    jclass variableClass = env->FindClass("com/swordfish/libretrodroid/Variable");
    jmethodID variableMethodID = env->GetMethodID(variableClass, "<init>", "()V");

    auto variables = Environment::getInstance().getVariables();
    jobjectArray result = env->NewObjectArray(variables.size(), variableClass, nullptr);

    for (int i = 0; i < variables.size(); i++) {
        jobject jVariable = env->NewObject(variableClass, variableMethodID);

        jfieldID jKeyField = env->GetFieldID(variableClass, "key", "Ljava/lang/String;");
        jfieldID jValueField = env->GetFieldID(variableClass, "value", "Ljava/lang/String;");
        jfieldID jDescriptionField = env->GetFieldID(
            variableClass,
            "description",
            "Ljava/lang/String;"
        );

        env->SetObjectField(jVariable, jKeyField, env->NewStringUTF(variables[i].key.data()));
        env->SetObjectField(jVariable, jValueField, env->NewStringUTF(variables[i].value.data()));
        env->SetObjectField(
            jVariable,
            jDescriptionField,
            env->NewStringUTF(variables[i].description.data()));

        env->SetObjectArrayElement(result, i, jVariable);
    }
    return result;
}

JNIEXPORT jobjectArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getControllers(
    JNIEnv* env,
    jclass obj
) {
    jclass variableClass = env->FindClass("[Lcom/swordfish/libretrodroid/Controller;");

    auto controllers = Environment::getInstance().getControllers();
    jobjectArray result = env->NewObjectArray(controllers.size(), variableClass, nullptr);

    for (int i = 0; i < controllers.size(); i++) {
        jclass variableClass2 = env->FindClass("com/swordfish/libretrodroid/Controller");
        jobjectArray controllerArray = env->NewObjectArray(
            controllers[i].size(),
            variableClass2,
            nullptr
        );
        jmethodID variableMethodID = env->GetMethodID(variableClass2, "<init>", "()V");

        for (int j = 0; j < controllers[i].size(); j++) {
            jobject jController = env->NewObject(variableClass2, variableMethodID);

            jfieldID jIdField = env->GetFieldID(variableClass2, "id", "I");
            jfieldID jDescriptionField = env->GetFieldID(
                variableClass2,
                "description",
                "Ljava/lang/String;"
            );

            env->SetIntField(jController, jIdField, (int) controllers[i][j].id);
            env->SetObjectField(
                jController,
                jDescriptionField,
                env->NewStringUTF(controllers[i][j].description.data()));

            env->SetObjectArrayElement(controllerArray, j, jController);
        }

        env->SetObjectArrayElement(result, i, controllerArray);
    }
    return result;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setControllerType(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint type
) {
    LibretroDroid::getInstance().setControllerType(port, type);
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeState(
    JNIEnv* env,
    jclass obj,
    jbyteArray state
) {
    try {
        jboolean isCopy = JNI_FALSE;
        jbyte* data = env->GetByteArrayElements(state, &isCopy);
        jsize size = env->GetArrayLength(state);

        bool result = LibretroDroid::getInstance().unserializeState(data, size);
        env->ReleaseByteArrayElements(state, data, JNI_ABORT);

        return result ? JNI_TRUE : JNI_FALSE;

    } catch (std::exception &exception) {
        LOGE("Error in unserializeState: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return JNI_FALSE;
    }
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeState(
    JNIEnv* env,
    jclass obj
) {
    try {
        auto [data, size] = LibretroDroid::getInstance().serializeState();

        jbyteArray result = env->NewByteArray(size);
        env->SetByteArrayRegion(result, 0, size, data);

        return result;

    } catch (std::exception &exception) {
        LOGE("Error in serializeState: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return nullptr;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setCheat(
    JNIEnv* env,
    jclass obj,
    jint index,
    jboolean enabled,
    jstring code
) {
    try {
        auto codeString = JniString(env, code);
        LibretroDroid::getInstance().setCheat(index, enabled, codeString.stdString());
    } catch (std::exception &exception) {
        LOGE("Error in setCheat: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_CHEAT);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resetCheat(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().resetCheat();
    } catch (std::exception &exception) {
        LOGE("Error in resetCheat: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_CHEAT);
    }
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeSRAM(
    JNIEnv* env,
    jclass obj,
    jbyteArray sram
) {
    try {
        jboolean isCopy = JNI_FALSE;
        jbyte* data = env->GetByteArrayElements(sram, &isCopy);
        jsize size = env->GetArrayLength(sram);

        LibretroDroid::getInstance().unserializeSRAM(data, size);

        env->ReleaseByteArrayElements(sram, data, JNI_ABORT);

    } catch (std::exception &exception) {
        LOGE("Error in unserializeSRAM: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeSRAM(
    JNIEnv* env,
    jclass obj
) {
    try {
        auto [data, size] = LibretroDroid::getInstance().serializeSRAM();

        jbyteArray result = env->NewByteArray(size);
        env->SetByteArrayRegion(result, 0, size, (jbyte *) data);

        return result;

    } catch (std::exception &exception) {
        LOGE("Error in serializeSRAM: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return nullptr;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_reset(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().reset();
    } catch (std::exception &exception) {
        LOGE("Error in clear: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onSurfaceChanged(
    JNIEnv* env,
    jclass obj,
    jint width,
    jint height
) {
    LibretroDroid::getInstance().onSurfaceChanged(width, height);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onSurfaceCreated(
    JNIEnv* env,
    jclass obj
) {
    LibretroDroid::getInstance().onSurfaceCreated();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onMotionEvent(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint source,
    jfloat xAxis,
    jfloat yAxis
) {
    LibretroDroid::getInstance().onMotionEvent(port, source, xAxis, yAxis);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onTouchEvent(
    JNIEnv* env,
    jclass obj,
    jfloat xAxis,
    jfloat yAxis
) {
    LibretroDroid::getInstance().onTouchEvent(xAxis, yAxis);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setFrameCrop(
    JNIEnv* env,
    jclass obj,
    jfloat x0,
    jfloat y0,
    jfloat x1,
    jfloat y1
) {
    LibretroDroid::getInstance().setFrameCrop(x0, y0, x1, y1);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setSecondarySurface(
    JNIEnv* env,
    jclass obj,
    jobject surface
) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    LibretroDroid::getInstance().setSecondaryWindow(window);
    if (window != nullptr) ANativeWindow_release(window); // SecondarySurface holds its own reference
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setSecondaryCrop(
    JNIEnv* env,
    jclass obj,
    jfloat x0,
    jfloat y0,
    jfloat x1,
    jfloat y1
) {
    LibretroDroid::getInstance().setSecondaryCrop(x0, y0, x1, y1);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onSecondaryTouchEvent(
    JNIEnv* env,
    jclass obj,
    jfloat x,
    jfloat y
) {
    LibretroDroid::getInstance().onSecondaryTouchEvent(x, y);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onKeyEvent(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint action,
    jint keyCode
) {
    LibretroDroid::getInstance().onKeyEvent(port, action, keyCode);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_create(
    JNIEnv* env,
    jclass obj,
    jint GLESVersion,
    jstring soFilePath,
    jstring systemDir,
    jstring savesDir,
    jobjectArray jVariables,
    jobject shaderConfig,
    jfloat refreshRate,
    jboolean preferLowLatencyAudio,
    jboolean enableVirtualFileSystem,
    jboolean enableMicrophone,
    jboolean skipDuplicateFrames,
    jobject immersiveMode,
    jstring language
) {
    try {
        auto corePath = JniString(env, soFilePath);
        auto deviceLanguage = JniString(env, language);
        auto systemDirectory = JniString(env, systemDir);
        auto savesDirectory = JniString(env, savesDir);

        std::vector<Variable> variables;
        int size = env->GetArrayLength(jVariables);
        for (int i = 0; i < size; i++) {
            auto jVariable = (jobject) env->GetObjectArrayElement(jVariables, i);
            auto variable = JavaUtils::variableFromJava(env, jVariable);
            variables.push_back(variable);
        }

        std::optional<ImmersiveMode::Config> parsedConfig = std::nullopt;
        if (immersiveMode != nullptr) {
            jclass configClass = env->GetObjectClass(immersiveMode);
            jfieldID downscaledWidthField = env->GetFieldID(configClass, "downscaledWidth", "I");
            jfieldID downscaledHeightField = env->GetFieldID(configClass, "downscaledHeight", "I");
            jfieldID blurMaskSizeField = env->GetFieldID(configClass, "blurMaskSize", "I");
            jfieldID blurBrightnessField = env->GetFieldID(configClass, "blurBrightness", "F");
            jfieldID blurSkipUpdateField = env->GetFieldID(configClass, "blurSkipUpdate", "I");
            jfieldID blendFactorField = env->GetFieldID(configClass, "blendFactor", "F");

            ImmersiveMode::Config config {};
            config.downscaledWidth = env->GetIntField(immersiveMode, downscaledWidthField);
            config.downscaledHeight = env->GetIntField(immersiveMode, downscaledHeightField);
            config.blurMaskSize = env->GetIntField(immersiveMode, blurMaskSizeField);
            config.blurBrightness = env->GetFloatField(immersiveMode, blurBrightnessField);
            config.blurSkipUpdate = env->GetIntField(immersiveMode, blurSkipUpdateField);
            config.blendFactor = env->GetFloatField(immersiveMode, blendFactorField);
            parsedConfig = config;
        }

        LibretroDroid::getInstance().create(
            GLESVersion,
            corePath.stdString(),
            systemDirectory.stdString(),
            savesDirectory.stdString(),
            variables,
            JavaUtils::shaderFromJava(env, shaderConfig),
            refreshRate,
            preferLowLatencyAudio,
            enableVirtualFileSystem,
            enableMicrophone,
            skipDuplicateFrames,
            parsedConfig,
            deviceLanguage.stdString()
        );

    } catch (libretrodroid::LibretroDroidError& exception) {
        LOGE("Error in create: %s", exception.what());
        JavaUtils::throwRetroException(env, exception.getErrorCode());
    } catch (std::exception &exception) {
        LOGE("Error in create: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_LIBRARY);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromPath(
    JNIEnv* env,
    jclass obj,
    jstring gameFilePath
) {
    auto gamePath = JniString(env, gameFilePath);

    try {
        LibretroDroid::getInstance().loadGameFromPath(gamePath.stdString());
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromPath: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromBytes(
    JNIEnv* env,
    jclass obj,
    jbyteArray gameFileBytes
) {
    try {
        size_t size = env->GetArrayLength(gameFileBytes);
        auto* data = new int8_t[size];
        env->GetByteArrayRegion(
            gameFileBytes,
            0,
            size,
            reinterpret_cast<int8_t*>(data)
        );
        LibretroDroid::getInstance().loadGameFromBytes(data, size);
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromBytes: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromVirtualFiles(
        JNIEnv* env,
        jclass obj,
        jobject virtualFileList
) {

    try {
        jmethodID getVirtualFileMethodID = env->GetMethodID(
                env->FindClass("com/swordfish/libretrodroid/DetachedVirtualFile"),
                "getVirtualPath",
                "()Ljava/lang/String;"
        );
        jmethodID getFileDescriptorMethodID = env->GetMethodID(
                env->FindClass("com/swordfish/libretrodroid/DetachedVirtualFile"),
                "getFileDescriptor",
                "()I"
        );

        std::vector<VFSFile> virtualFiles;

        JavaUtils::forEachOnJavaIterable(env, virtualFileList, [&](jobject item) {
            JniString virtualFileName(env,(jstring) env->CallObjectMethod(
                item,
                getVirtualFileMethodID
            ));

            int fileDescriptor = env->CallIntMethod(item, getFileDescriptorMethodID);
            virtualFiles.emplace_back(VFSFile(virtualFileName.stdString(), fileDescriptor));
        });

        LibretroDroid::getInstance().loadGameFromVirtualFiles(std::move(virtualFiles));
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromDescriptors: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_destroy(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().destroy();
    } catch (std::exception &exception) {
        LOGE("Error in destroy: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resume(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().resume();
    } catch (std::exception &exception) {
        LOGE("Error in resume: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_pause(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().pause();
    } catch (std::exception &exception) {
        LOGE("Error in pause: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_step(
    JNIEnv* env,
    jclass obj,
    jobject glRetroView
) {
    LibretroDroid::getInstance().step();

    if (LibretroDroid::getInstance().requiresVideoRefresh()) {
        LibretroDroid::getInstance().clearRequiresVideoRefresh();
        jclass cls = env->GetObjectClass(glRetroView);
        jmethodID requestAspectRatioUpdate = env->GetMethodID(cls, "refreshAspectRatio", "()V");
        env->CallVoidMethod(glRetroView, requestAspectRatioUpdate);
    }

    if (LibretroDroid::getInstance().isRumbleEnabled()) {
        LibretroDroid::getInstance().handleRumbleUpdates([&](int port, float weak, float strong) {
            jclass cls = env->GetObjectClass(glRetroView);
            jmethodID sendRumbleStrengthMethodID = env->GetMethodID(cls, "sendRumbleEvent", "(IFF)V");
            env->CallVoidMethod(glRetroView, sendRumbleStrengthMethodID, port, weak, strong);
        });
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRumbleEnabled(
    JNIEnv* env,
    jclass obj,
    jboolean enabled
) {
    LibretroDroid::getInstance().setRumbleEnabled(enabled);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setFrameSpeed(
    JNIEnv* env,
    jclass obj,
    jint speed
) {
    LibretroDroid::getInstance().setFrameSpeed(speed);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setAudioEnabled(
    JNIEnv* env,
    jclass obj,
    jboolean enabled
) {
    LibretroDroid::getInstance().setAudioEnabled(enabled);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setShaderConfig(
    JNIEnv* env,
    jclass obj,
    jobject shaderConfig
) {
    LibretroDroid::getInstance().setShaderConfig(JavaUtils::shaderFromJava(env, shaderConfig));
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setViewport(
    JNIEnv* env,
    jclass obj,
    jfloat x,
    jfloat y,
    jfloat width,
    jfloat height
) {
    LibretroDroid::getInstance().setViewport(Rect(x, y, width, height));
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setViewportAlignment(
        JNIEnv* env,
        jclass obj,
        jint viewportAlignment
) {
    LibretroDroid::getInstance().setViewportAlignment(viewportAlignment);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_refreshAspectRatio(
    JNIEnv* env,
    jclass obj
) {
    LibretroDroid::getInstance().refreshAspectRatio();
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_readMemory(
    JNIEnv* env,
    jclass obj,
    jlong address,
    jbyteArray dest,
    jint offset,
    jint length
) {
    jsize capacity = env->GetArrayLength(dest);
    if (address < 0 || address > 0xFFFFFFFFLL || offset < 0 || length < 0 || offset > capacity || length > capacity - offset) {
        return -1;
    }
    if (length == 0) return 0;

    std::vector<uint8_t> buffer(length);
    size_t copied = LibretroDroid::getInstance().readMemory((uint32_t) address, buffer.data(), length);
    if (copied > 0) {
        env->SetByteArrayRegion(dest, offset, (jsize) copied, (const jbyte*) buffer.data());
    }
    return (jint) copied;
}

JNIEXPORT jlongArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getMemoryRegions(
    JNIEnv* env,
    jclass obj
) {
    auto regions = LibretroDroid::getInstance().getMemoryRegions();
    std::vector<jlong> values;
    values.reserve(regions.size() * 6);
    for (const auto& region : regions) {
        values.push_back((jlong) region.start);
        values.push_back((jlong) region.select);
        values.push_back((jlong) region.disconnect);
        values.push_back((jlong) region.len);
        values.push_back((jlong) region.flags);
        values.push_back(region.ptr != nullptr ? 1 : 0);
    }
    jlongArray result = env->NewLongArray((jsize) values.size());
    env->SetLongArrayRegion(result, 0, (jsize) values.size(), values.data());
    return result;
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getFrameCount(
    JNIEnv* env,
    jclass obj
) {
    return (jlong) LibretroDroid::getInstance().getFrameCount();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRewind(
    JNIEnv* env,
    jclass obj,
    jlong budgetBytes,
    jint interval
) {
    LibretroDroid::getInstance().setRewind(
        budgetBytes > 0 ? (size_t) budgetBytes : 0,
        interval > 0 ? (unsigned) interval : 1
    );
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRewinding(
    JNIEnv* env,
    jclass obj,
    jboolean rewinding
) {
    LibretroDroid::getInstance().setRewinding(rewinding);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRunAhead(
    JNIEnv* env,
    jclass obj,
    jint frames
) {
    LibretroDroid::getInstance().setRunAhead(frames > 0 ? (unsigned) frames : 0);
}

namespace {

struct AchievementsBridge {
    JavaVM* vm = nullptr;
    jobject listener = nullptr;
    jmethodID onServerCall = nullptr;
    jmethodID onEvent = nullptr;
};

AchievementsBridge achievementsBridge;

JNIEnv* currentEnv() {
    JNIEnv* env = nullptr;
    if (achievementsBridge.vm == nullptr) return nullptr;
    if (achievementsBridge.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    // Callbacks run on the emulation (GLSurfaceView) thread, which is always a Java thread.
    return nullptr;
}

jbyteArray toBytes(JNIEnv* env, const std::string& text) {
    jbyteArray array = env->NewByteArray((jsize) text.size());
    if (array != nullptr) {
        env->SetByteArrayRegion(array, 0, (jsize) text.size(), reinterpret_cast<const jbyte*>(text.data()));
    }
    return array;
}

std::string fromBytes(JNIEnv* env, jbyteArray array) {
    if (array == nullptr) return {};
    jsize length = env->GetArrayLength(array);
    std::string text((size_t) length, char(0));
    env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(text.data()));
    return text;
}

}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_achievementsSetListener(
    JNIEnv* env,
    jclass obj,
    jobject listener
) {
    env->GetJavaVM(&achievementsBridge.vm);
    if (achievementsBridge.listener != nullptr) env->DeleteGlobalRef(achievementsBridge.listener);
    achievementsBridge.listener = listener != nullptr ? env->NewGlobalRef(listener) : nullptr;
    if (listener != nullptr) {
        jclass type = env->GetObjectClass(listener);
        achievementsBridge.onServerCall = env->GetMethodID(type, "onServerCall", "(J[B[B[B)V");
        achievementsBridge.onEvent = env->GetMethodID(type, "onEvent", "(IJ[B)V");
        env->DeleteLocalRef(type);
    }

    libretrodroid::Achievements::getInstance().setCallbacks(
        [](int64_t id, const std::string& url, const std::string& postData, const std::string& contentType) {
            JNIEnv* e = currentEnv();
            if (e == nullptr || achievementsBridge.listener == nullptr) return;
            jbyteArray u = toBytes(e, url);
            jbyteArray p = toBytes(e, postData);
            jbyteArray c = toBytes(e, contentType);
            e->CallVoidMethod(achievementsBridge.listener, achievementsBridge.onServerCall, (jlong) id, u, p, c);
            if (e->ExceptionCheck()) e->ExceptionClear();
            e->DeleteLocalRef(u);
            e->DeleteLocalRef(p);
            e->DeleteLocalRef(c);
        },
        [](int event, int64_t value, const std::string& text) {
            JNIEnv* e = currentEnv();
            if (e == nullptr || achievementsBridge.listener == nullptr) return;
            jbyteArray t = toBytes(e, text);
            e->CallVoidMethod(achievementsBridge.listener, achievementsBridge.onEvent, (jint) event, (jlong) value, t);
            if (e->ExceptionCheck()) e->ExceptionClear();
            e->DeleteLocalRef(t);
        }
    );
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_achievementsIdentify(
    JNIEnv* env,
    jclass obj,
    jint consoleId,
    jstring romPath
) {
    auto path = JniString(env, romPath);
    libretrodroid::Achievements::getInstance().identify((unsigned) consoleId, path.stdString());
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_achievementsLogin(
    JNIEnv* env,
    jclass obj,
    jbyteArray user,
    jbyteArray password,
    jbyteArray token
) {
    libretrodroid::Achievements::getInstance().login(
        fromBytes(env, user), fromBytes(env, password), fromBytes(env, token));
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_achievementsServerResponse(
    JNIEnv* env,
    jclass obj,
    jlong id,
    jbyteArray body,
    jint httpStatus
) {
    libretrodroid::Achievements::getInstance().onServerResponse((int64_t) id, fromBytes(env, body), httpStatus);
}

JNIEXPORT jstring JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_achievementsUserAgentClause(
    JNIEnv* env,
    jclass obj
) {
    return env->NewStringUTF(libretrodroid::Achievements::getInstance().userAgentClause().c_str());
}

JNIEXPORT jlongArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getRuntimeStats(
    JNIEnv* env,
    jclass obj
) {
    std::vector<int64_t> stats = LibretroDroid::getInstance().getRuntimeStats();
    jlongArray result = env->NewLongArray((jsize) stats.size());
    if (result == nullptr) return nullptr;
    std::vector<jlong> values(stats.begin(), stats.end());
    env->SetLongArrayRegion(result, 0, (jsize) values.size(), values.data());
    return result;
}

}

}
