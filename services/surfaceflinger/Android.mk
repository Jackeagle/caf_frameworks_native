LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    EventThread.cpp                         \
    Layer.cpp                               \
    LayerBase.cpp                           \
    LayerDim.cpp                            \
    LayerScreenshot.cpp                     \
    DisplayHardware/DisplayHardware.cpp     \
    DisplayHardware/DisplayHardwareBase.cpp \
    DisplayHardware/HWComposer.cpp          \
    DisplayHardware/PowerHAL.cpp            \
    GLExtensions.cpp                        \
    MessageQueue.cpp                        \
    SurfaceFlinger.cpp                      \
    SurfaceTextureLayer.cpp                 \
    Transform.cpp                           \

LOCAL_CFLAGS:= -DLOG_TAG=\"SurfaceFlinger\"
LOCAL_CFLAGS += -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES

ifeq ($(TARGET_BOARD_PLATFORM), omap3)
	LOCAL_CFLAGS += -DNO_RGBX_8888
endif
ifeq ($(TARGET_BOARD_PLATFORM), omap4)
	LOCAL_CFLAGS += -DHAS_CONTEXT_PRIORITY
endif
ifeq ($(TARGET_BOARD_PLATFORM), s5pc110)
	LOCAL_CFLAGS += -DHAS_CONTEXT_PRIORITY
	LOCAL_CFLAGS += -DNEVER_DEFAULT_TO_ASYNC_MODE
endif

ifeq ($(BOARD_USES_QCOM_HARDWARE), true)
    LOCAL_C_INCLUDES += hardware/qcom/display/libgralloc
    LOCAL_C_INCLUDES += hardware/qcom/display/libqdutils
    LOCAL_C_INCLUDES += hardware/qcom/display/libhwcomposer
    LOCAL_CFLAGS += -DQCOMHW
endif

ifeq ($(TARGET_DISABLE_TRIPLE_BUFFERING), true)
	LOCAL_CFLAGS += -DTARGET_DISABLE_TRIPLE_BUFFERING
endif

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libhardware \
	libutils \
	libEGL \
	libGLESv1_CM \
	libbinder \
	libui \
	libgui

ifeq ($(BOARD_USES_QCOM_HARDWARE), true)
    LOCAL_SHARED_LIBRARIES += libqdutils
endif

# this is only needed for DDMS debugging
ifneq ($(TARGET_BUILD_PDK), true)
	LOCAL_SHARED_LIBRARIES += libdvm libandroid_runtime
	LOCAL_CLFAGS += -DDDMS_DEBUGGING
	LOCAL_SRC_FILES += DdmConnection.cpp
endif

LOCAL_MODULE:= libsurfaceflinger

include $(BUILD_SHARED_LIBRARY)
