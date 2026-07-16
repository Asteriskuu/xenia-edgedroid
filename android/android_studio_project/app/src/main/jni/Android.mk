LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xenia-jni
LOCAL_SRC_FILES := xenia_android.cpp

LOCAL_C_INCLUDES := \
    $(XENIA_ROOT)/src \
    $(XENIA_ROOT)

LOCAL_LDLIBS := -llog -landroid

LOCAL_STATIC_LIBRARIES := xenia-app

include $(BUILD_SHARED_LIBRARY)
