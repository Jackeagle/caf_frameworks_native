LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	animated_splash.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libhardware \
	libutils \

LOCAL_C_INCLUDES += $(call include-path-for, opengl-tests-includes)

LOCAL_C_INCLUDES  += $(TARGET_OUT_HEADERS)/qcom/display/

LOCAL_MODULE:= animated-splash

LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS := -DGL_GLEXT_PROTOTYPES

include $(BUILD_EXECUTABLE)
