/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "VibratorService"

#include <android/hardware/vibrator/1.4/IVibrator.h>

#include "jni.h"
#include <nativehelper/JNIHelp.h>
#include "android_runtime/AndroidRuntime.h"
#include "core_jni_helpers.h"

#include <utils/misc.h>
#include <utils/Log.h>
#include <hardware/vibrator.h>

#include <inttypes.h>
#include <stdio.h>

using android::hardware::Return;
using android::hardware::Void;
using android::hardware::vibrator::V1_0::EffectStrength;
using android::hardware::vibrator::V1_0::Status;
using android::hardware::vibrator::V1_1::Effect_1_1;

namespace V1_0 = android::hardware::vibrator::V1_0;
namespace V1_1 = android::hardware::vibrator::V1_1;
namespace V1_2 = android::hardware::vibrator::V1_2;
namespace V1_3 = android::hardware::vibrator::V1_3;
namespace V1_4 = android::hardware::vibrator::V1_4;

namespace android {

static jmethodID sMethodIdOnComplete;

class VibratorCallback : public V1_4::IVibratorCallback {
    public:
        VibratorCallback(JNIEnv *env, jobject vibration) :
        mVibration(MakeGlobalRefOrDie(env, vibration)) {}

        ~VibratorCallback() {
            JNIEnv *env = AndroidRuntime::getJNIEnv();
            env->DeleteGlobalRef(mVibration);
        }

        Return<void> onComplete() override {
            auto env = AndroidRuntime::getJNIEnv();
            env->CallVoidMethod(mVibration, sMethodIdOnComplete);
            return Void();
        }

    private:
        jobject mVibration;
};

static constexpr int NUM_TRIES = 2;

// Creates a Return<R> with STATUS::EX_NULL_POINTER.
template<class R>
inline Return<R> NullptrStatus() {
    using ::android::hardware::Status;
    return Return<R>{Status::fromExceptionCode(Status::EX_NULL_POINTER)};
}

template <typename I>
class HalWrapper {
  public:
    static std::unique_ptr<HalWrapper> Create() {
        // Assume that if getService returns a nullptr, HAL is not available on the
        // device.
        auto hal = I::getService();
        return hal ? std::unique_ptr<HalWrapper>(new HalWrapper(std::move(hal))) : nullptr;
    }

    // Helper used to transparently deal with the vibrator HAL becoming unavailable.
    template<class R, class... Args0, class... Args1>
    Return<R> call(Return<R> (I::* fn)(Args0...), Args1&&... args1) {
        // Return<R> doesn't have a default constructor, so make a Return<R> with
        // STATUS::EX_NONE.
        using ::android::hardware::Status;
        Return<R> ret{Status::fromExceptionCode(Status::EX_NONE)};

        // Note that ret is guaranteed to be changed after this loop.
        for (int i = 0; i < NUM_TRIES; ++i) {
            ret = (mHal == nullptr) ? NullptrStatus<R>()
                    : (*mHal.*fn)(std::forward<Args1>(args1)...);

            if (ret.isOk()) {
                break;
            }

            ALOGE("Failed to issue command to vibrator HAL. Retrying.");
            // Restoring connection to the HAL.
            mHal = I::tryGetService();
        }
        return ret;
    }

  private:
    HalWrapper(sp<I> &&hal) : mHal(std::move(hal)) {}

  private:
    sp<I> mHal;
};

template <typename I>
static auto getHal() {
    static auto sHalWrapper = HalWrapper<I>::Create();
    return sHalWrapper.get();
}

template<class R, class I, class... Args0, class... Args1>
Return<R> halCall(Return<R> (I::* fn)(Args0...), Args1&&... args1) {
    auto hal = getHal<I>();
    return hal ? hal->call(fn, std::forward<Args1>(args1)...) : NullptrStatus<R>();
}

template<class R>
bool isValidEffect(jlong effect) {
    if (effect < 0) {
        return false;
    }
    R val = static_cast<R>(effect);
    auto iter = hardware::hidl_enum_range<R>();
    return val >= *iter.begin() && val <= *std::prev(iter.end());
}

static void vibratorInit(JNIEnv *env, jclass clazz)
{
    halCall(&V1_0::IVibrator::ping).isOk();
}

static jboolean vibratorExists(JNIEnv* /* env */, jclass /* clazz */)
{
    return halCall(&V1_0::IVibrator::ping).isOk() ? JNI_TRUE : JNI_FALSE;
}

static void vibratorOn(JNIEnv* /* env */, jclass /* clazz */, jlong timeout_ms)
{
    Status retStatus = halCall(&V1_0::IVibrator::on, timeout_ms).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOn command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static void vibratorOff(JNIEnv* /* env */, jclass /* clazz */)
{
    Status retStatus = halCall(&V1_0::IVibrator::off).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOff command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static jlong vibratorSupportsAmplitudeControl(JNIEnv*, jclass) {
    return halCall(&V1_0::IVibrator::supportsAmplitudeControl).withDefault(false);
}

static void vibratorSetAmplitude(JNIEnv*, jclass, jint amplitude) {
    Status status = halCall(&V1_0::IVibrator::setAmplitude, static_cast<uint32_t>(amplitude))
        .withDefault(Status::UNKNOWN_ERROR);
    if (status != Status::OK) {
      ALOGE("Failed to set vibrator amplitude (%" PRIu32 ").",
            static_cast<uint32_t>(status));
    }
}

static jboolean vibratorSupportsExternalControl(JNIEnv*, jclass) {
    return halCall(&V1_3::IVibrator::supportsExternalControl).withDefault(false);
}

static void vibratorSetExternalControl(JNIEnv*, jclass, jboolean enabled) {
    Status status = halCall(&V1_3::IVibrator::setExternalControl, static_cast<uint32_t>(enabled))
        .withDefault(Status::UNKNOWN_ERROR);
    if (status != Status::OK) {
      ALOGE("Failed to set vibrator external control (%" PRIu32 ").",
            static_cast<uint32_t>(status));
    }
}

static jlong vibratorPerformEffect(JNIEnv* env, jclass, jlong effect, jlong strength,
                                   jobject vibration) {
    Status status;
    uint32_t lengthMs;
    auto callback = [&status, &lengthMs](Status retStatus, uint32_t retLengthMs) {
        status = retStatus;
        lengthMs = retLengthMs;
    };
    EffectStrength effectStrength(static_cast<EffectStrength>(strength));

    Return<void> ret;
    if (auto hal = getHal<V1_4::IVibrator>(); hal && isValidEffect<V1_3::Effect>(effect)) {
        sp<VibratorCallback> effectCallback = new VibratorCallback(env, vibration);
        ret = hal->call(&V1_4::IVibrator::perform_1_4, static_cast<V1_3::Effect>(effect),
                effectStrength, effectCallback, callback);
    } else if (isValidEffect<V1_0::Effect>(effect)) {
        ret = halCall(&V1_0::IVibrator::perform, static_cast<V1_0::Effect>(effect),
                effectStrength, callback);
    } else if (isValidEffect<Effect_1_1>(effect)) {
        ret = halCall(&V1_1::IVibrator::perform_1_1, static_cast<Effect_1_1>(effect),
                           effectStrength, callback);
    } else if (isValidEffect<V1_2::Effect>(effect)) {
        ret = halCall(&V1_2::IVibrator::perform_1_2, static_cast<V1_2::Effect>(effect),
                           effectStrength, callback);
    } else if (isValidEffect<V1_3::Effect>(effect)) {
        ret = halCall(&V1_3::IVibrator::perform_1_3, static_cast<V1_3::Effect>(effect),
                           effectStrength, callback);
    } else {
        ALOGW("Unable to perform haptic effect, invalid effect ID (%" PRId32 ")",
                static_cast<int32_t>(effect));
        return -1;
    }

    if (!ret.isOk()) {
        ALOGW("Failed to perform effect (%" PRId32 ")", static_cast<int32_t>(effect));
        return -1;
    }

    if (status == Status::OK) {
        return lengthMs;
    } else if (status != Status::UNSUPPORTED_OPERATION) {
        // Don't warn on UNSUPPORTED_OPERATION, that's a normal event and just means the motor
        // doesn't have a pre-defined waveform to perform for it, so we should just give the
        // opportunity to fall back to the framework waveforms.
        ALOGE("Failed to perform haptic effect: effect=%" PRId64 ", strength=%" PRId32
                ", error=%" PRIu32 ").", static_cast<int64_t>(effect),
                static_cast<int32_t>(strength), static_cast<uint32_t>(status));
    }

    return -1;
}

static jlong vibratorGetCapabilities(JNIEnv*, jclass) {
    return halCall(&V1_4::IVibrator::getCapabilities).withDefault(0);
}

static const JNINativeMethod method_table[] = {
    { "vibratorExists", "()Z", (void*)vibratorExists },
    { "vibratorInit", "()V", (void*)vibratorInit },
    { "vibratorOn", "(J)V", (void*)vibratorOn },
    { "vibratorOff", "()V", (void*)vibratorOff },
    { "vibratorSupportsAmplitudeControl", "()Z", (void*)vibratorSupportsAmplitudeControl},
    { "vibratorSetAmplitude", "(I)V", (void*)vibratorSetAmplitude},
    { "vibratorPerformEffect", "(JJLcom/android/server/VibratorService$Vibration;)J",
        (void*)vibratorPerformEffect},
    { "vibratorSupportsExternalControl", "()Z", (void*)vibratorSupportsExternalControl},
    { "vibratorSetExternalControl", "(Z)V", (void*)vibratorSetExternalControl},
    { "vibratorGetCapabilities", "()J", (void*)vibratorGetCapabilities},
};

int register_android_server_VibratorService(JNIEnv *env)
{
    sMethodIdOnComplete = GetMethodIDOrDie(env,
            FindClassOrDie(env, "com/android/server/VibratorService$Vibration"),
            "onComplete", "()V");
    return jniRegisterNativeMethods(env, "com/android/server/VibratorService",
            method_table, NELEM(method_table));
}

};
