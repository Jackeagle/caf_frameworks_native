/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above
*      copyright notice, this list of conditions and the following
*      disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its
*      contributors may be used to endorse or promote products derived
*      from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <unistd.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <ui/ANativeObjectBase.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <cutils/log.h>
#include <cutils/native_handle.h>
#include <sys/cdefs.h>
#include <stdint.h>
#include <pthread.h>
#include <linux/fb.h>
#include <copybit.h>
#include <sys/ioctl.h>
#include <linux/msm_mdp.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/queue.h>
#include <linux/netlink.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <cutils/properties.h>

#define TEST_FB GRALLOC_HARDWARE_FB_PRIMARY
//#define TEST_FB GRALLOC_HARDWARE_FB_SECONDARY
//#define TEST_FB GRALLOC_HARDWARE_FB_TERTIARY

#define CLIENT_NAME "ASPLASH"
#define MDP_ARB_EVENT_NAME "switch-reverse"
#define MDP_ARB_PRIORITY 1
#define MDP_ARB_UEVENT_PATH "change@/devices/virtual/mdp_arb/mdp_arb"
#define MDP_ARB_UEVENT_OPTIMIZE_PREFIX "optimize="
#define MDP_ARB_UEVENT_DOWN_PREFIX "down="
#define MDP_ARB_UEVENT_UP_PREFIX "up="
#define MDP_ARB_UEVENT_FB_IDX_PREFIX "fb_idx="
#define MDP_ARB_UEVENT_STATE_PREFIX "state="

#define UEVENT_THREAD_NAME "ASPLASH_UEVENT"
#define UEVENT_STRING_LEN_MAX 128

#define MIN_NUM_FRAME_BUFFERS  2
#define MAX_NUM_FRAME_BUFFERS  3

#define LOG_TAG "ASPLASH"
#define MAX_FILENAME_LENGTH 20
#define NUM_OF_BMPS 16
#define BMP_HEADER_IN_BYTES 54
#define P_SPLASH_IMAGE_WIDTH     1280
#define P_SPLASH_IMAGE_HEIGHT    768
#define P_SPLASH_2S_HOLD 26
#define BMP_PIXEL_FORMAT HAL_PIXEL_FORMAT_BGR_888
#define BMP_BPP 3
#define SLEEP_EACH_FRAME_IN_US   50000 //50ms
#define SLEEP_LAST_FRAME_IN_US   2000000 //2s

static char *inBuf = NULL;
static copybit_device_t *copybitDev = NULL;
static alloc_device_t *grDev = NULL;
static framebuffer_device_t *fbDev = NULL;
static int arb_fd = -1;
static int32_t mNumBuffers = 0;
static ANativeWindowBuffer nativeWindowBuf[MAX_NUM_FRAME_BUFFERS];
static bool gRegArb = false;
static bool gAck = false;
static mdp_arb_notification_event gEvent = MDP_ARB_NOTIFICATION_DOWN;
static bool gRunning = false;

struct copybit_iterator : public copybit_region_t {
    copybit_iterator(const copybit_rect_t& rect) {
        mRect = rect;
        mCount = 1;
        this->next = iterate;
    }
private:
    static int iterate(copybit_region_t const * self, copybit_rect_t* rect) {
        if (!self || !rect) {
            return 0;
        }

        copybit_iterator const* me = static_cast<copybit_iterator const*>(self);
        if (me->mCount) {
            rect->l = me->mRect.l;
            rect->t = me->mRect.t;
            rect->r = me->mRect.r;
            rect->b = me->mRect.b;
            me->mCount--;
            return 1;
        }
        return 0;
    }
    copybit_rect_t mRect;
    mutable int mCount;
};

class Locker {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    public:
    class Autolock {
        Locker& locker;
        public:
        inline Autolock(Locker& locker) : locker(locker) {  locker.lock(); }
        inline ~Autolock() { locker.unlock(); }
    };
    inline Locker() {
        pthread_mutex_init(&mutex, 0);
        pthread_cond_init(&cond, 0);
    }
    inline ~Locker() {
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&cond);
    }
    inline void lock()     { pthread_mutex_lock(&mutex); }
    inline void unlock()   { pthread_mutex_unlock(&mutex); }
    inline void wait()     { pthread_cond_wait(&cond, &mutex); }
    inline void signal()   { pthread_cond_signal(&cond); }
};

static Locker mEventLock;
static Locker mWaitForFinishLock;
static Locker mDrawLock;

LIST_HEAD(uevent_handler_head, uevent_handler) uevent_handler_list;
pthread_mutex_t uevent_handler_list_lock = PTHREAD_MUTEX_INITIALIZER;

struct uevent_handler {
    void (*handler)(void *data, const char *msg, int msg_len);
    void *handler_data;
    LIST_ENTRY(uevent_handler) list;
};

static int uevent_fd = -1;

static void place_marker(const char* str)
{
    FILE *fp;

    fp = fopen( "/proc/bootkpi/marker_entry" , "w" );
    if (fp) {
        fwrite(str , 1 , strlen(str) , fp );
        fclose(fp);
    }
}

/* Returns 0 on failure, 1 on success */
static int uevent_init()
{
    struct sockaddr_nl addr;
    int sz = 64*1024;
    int s;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 0xffffffff;

    s = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if(s < 0)
        return 0;

    setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz));

    if(bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(s);
        return 0;
    }

    uevent_fd = s;
    return (uevent_fd > 0);
}

static int uevent_next_event(char* buffer, int buffer_length)
{
    while (1) {
        struct pollfd fds;
        int nr;

        fds.fd = uevent_fd;
        fds.events = POLLIN;
        fds.revents = 0;
        nr = poll(&fds, 1, -1);

        if(nr > 0 && (fds.revents & POLLIN)) {
            int count = recv(uevent_fd, buffer, buffer_length, 0);
            if (count > 0) {
                struct uevent_handler *h;
                pthread_mutex_lock(&uevent_handler_list_lock);
                LIST_FOREACH(h, &uevent_handler_list, list)
                    h->handler(h->handler_data, buffer, buffer_length);
                pthread_mutex_unlock(&uevent_handler_list_lock);

                return count;
            }
        }
    }
    // won't get here
    return 0;
}

static void deregisterMdpArbitrator(int& fd)
{
    if (fd >= 0) {
        int ret = 0;
        ret = ioctl(fd, MSMFB_ARB_DEREGISTER, NULL);
        if (ret)
            ALOGE("MDP_ARB_DEREGISTER fails=%d", ret);
        close(fd);
        fd = -1;
    }
    return;
}

static int registerMdpArbitrator(const char *name, int fb_idx)
{
    int ret = 0;
    int fd = -1;
    mdp_arb_register arbReg;
    mdp_arb_event event;
    int upState = 0;
    int downState = 1;
    fd = open("/dev/mdp_arb", O_RDWR);
    if (fd < 0) {
        ALOGI("%s, MDP arbitrator is disabled! client=%s, fb_idx=%d",
              name, fb_idx);
        return -1;
    }
    memset(&arbReg, 0x00, sizeof(arbReg));
    memset(&event, 0x00, sizeof(event));
    strlcpy(arbReg.name, name, MDP_ARB_NAME_LEN);
    arbReg.fb_index = fb_idx;
    arbReg.num_of_events = 1;
    strlcpy(event.name, MDP_ARB_EVENT_NAME, MDP_ARB_NAME_LEN);
    event.event.register_state.num_of_down_state_value = 1;
    event.event.register_state.down_state_value = &downState;
    event.event.register_state.num_of_up_state_value = 1;
    event.event.register_state.up_state_value = &upState;
    arbReg.event = &event;
    arbReg.priority = MDP_ARB_PRIORITY;
    arbReg.notification_support_mask = (MDP_ARB_NOTIFICATION_DOWN |
        MDP_ARB_NOTIFICATION_UP);
    ret = ioctl(fd, MSMFB_ARB_REGISTER, &arbReg);
    if (ret) {
        ALOGE("MDP_ARB_REGISTER fails=%d, client=%s, fb_idx=%d",
              ret, name, fb_idx);
        deregisterMdpArbitrator(fd);
        return -1;
    }
    return fd;
}

static void cleanUpResources(bool deregArb, bool ack,
                             mdp_arb_notification_event event)
{
    int ret = 0;

    mDrawLock.lock();
    /* close devices and free module */
    if (inBuf) {
        free(inBuf);
        inBuf = NULL;
    }

    if (copybitDev) {
        copybit_close(copybitDev);
        copybitDev = NULL;
    }

    if (grDev) {
        for(int i = 0; i < mNumBuffers; i++) {
            if (nativeWindowBuf[i].handle != NULL)
                grDev->free(grDev, nativeWindowBuf[i].handle);
        }
        gralloc_close(grDev);
        grDev = NULL;
    }

    if (fbDev) {
        framebuffer_close(fbDev);
        fbDev= NULL;
    }

    if (ack) {
        // Acknowledge the MDP arb
        ret = ioctl(arb_fd, MSMFB_ARB_ACKNOWLEDGE, &event);
        if (ret) {
            ALOGE("%s mdp arb ack fails=%d", __FUNCTION__, ret);
        }
    }

    if (deregArb) {
        deregisterMdpArbitrator(arb_fd);
    }

    mDrawLock.unlock();
}

static int drawSplash(bool regArb, bool ack, mdp_arb_notification_event event,
                      bool& looping)
{
    hw_module_t const* module;
    hw_module_t const* copybit_module;
    char * inbuf;
    int err = 0;
    int stride = 0;
    int i = 0;
    int32_t mNumFreeBuffers = 0;
    int32_t mBufferHead = 0;
    copybit_image_t src, dst;
    copybit_iterator* clip = NULL;
    copybit_rect_t rect;
    copybit_rect_t src_rect;
    copybit_rect_t dst_rect;
    int native_buf_index = 0;
    char gHeader[BMP_HEADER_IN_BYTES];
    bool sendAck = false;
    char value[PROPERTY_VALUE_MAX];
    bool keepRunning = false;
    bool stop = false;

    char file_list[][MAX_FILENAME_LENGTH] =
    {
        "/data/pan_1.bmp",  "/data/pan_2.bmp",  "/data/pan_3.bmp",
        "/data/pan_4.bmp",  "/data/pan_5.bmp",  "/data/pan_6.bmp",
        "/data/pan_7.bmp",  "/data/pan_8.bmp",  "/data/pan_9.bmp",
        "/data/pan_10.bmp", "/data/pan_11.bmp", "/data/pan_12.bmp",
        "/data/pan_13.bmp", "/data/pan_14.bmp", "/data/pan_15.bmp",
        "/data/pan_16.bmp"
    };

    mDrawLock.lock();
    gRunning = true;

    if(property_get("sys.asplash.keep_running", value, "false")
            && !strcmp(value, "true")) {
        keepRunning = true;
    } else {
        keepRunning = false;
    }

    if (hw_get_module(COPYBIT_HARDWARE_MODULE_ID, &copybit_module) != 0) {
        ALOGE("Can't open copybit module");
        err = -1;
        goto err_exit;
    } else {
        err = copybit_open(copybit_module, &copybitDev);
        if((err < 0) || (copybitDev == NULL)) {
            ALOGE_IF(err, "couldn't open copybit HAL (%s)",
                        strerror(-err));
            err = -1;
            goto err_exit;
        }
    }


    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        gralloc_module_t const* grModule =
        reinterpret_cast<gralloc_module_t const*>(module);
        int fb_idx = 0;
        if (grModule->getDisplayFbIdx && grModule->framebufferOpenEx) {
            err = grModule->getDisplayFbIdx(TEST_FB, &fb_idx);
            if (err) {
                ALOGE("couldn't get FB idx, fb=%s", TEST_FB);
                goto err_exit;
            } else {
                if (regArb) {
                    /* Register to MDP ARB */
                    arb_fd = registerMdpArbitrator(CLIENT_NAME, fb_idx);
                    ALOGW_IF((arb_fd < 0), "can't register to MDP ARB"\
                        " client=%s, fb_idx=%d", CLIENT_NAME, fb_idx);
                }
                err = grModule->framebufferOpenEx(module, CLIENT_NAME,
                        fb_idx, &fbDev);
                if (err) {
                    ALOGE("couldn't open framebuffer HAL Ex "\
                          "client=%s, fb_idx=%d", CLIENT_NAME, fb_idx);
                    goto err_exit;
                }
            }
        } else {
            ALOGI("gralloc doesn't support getDisplayFbIdx and "\
                  "framebufferOpenEx, fallback to legacy framebuffer_open");
            err = framebuffer_open(module, TEST_FB, &fbDev);
            if(err){
                ALOGE_IF(err, "couldn't open framebuffer HAL (%s)",
                            strerror(-err));
            }
        }

        err = gralloc_open(module, &grDev);
        if(err){
            ALOGE_IF(err, "couldn't open gralloc HAL (%s)",
                        strerror(-err));
        }

        if (!fbDev || !grDev) {
            err = -1;
            goto err_exit;
        }

        if(fbDev->numFramebuffers >= MIN_NUM_FRAME_BUFFERS &&
            fbDev->numFramebuffers <= MAX_NUM_FRAME_BUFFERS){
            mNumBuffers = fbDev->numFramebuffers;
        } else {
            mNumBuffers = MIN_NUM_FRAME_BUFFERS;
        }

        /* setup buffers*/
        for (i = 0; i < mNumBuffers; i++) {
            nativeWindowBuf[i].width = (fbDev->width > P_SPLASH_IMAGE_WIDTH) ?
                                        fbDev->width : P_SPLASH_IMAGE_WIDTH;
            nativeWindowBuf[i].height =
                                     (fbDev->height > P_SPLASH_IMAGE_HEIGHT) ?
                                      fbDev->height : P_SPLASH_IMAGE_HEIGHT;
            nativeWindowBuf[i].format = BMP_PIXEL_FORMAT;
            nativeWindowBuf[i].usage =
                    GRALLOC_USAGE_SW_WRITE_RARELY|GRALLOC_USAGE_SW_READ_RARELY;
        }

        /* allocate buffers*/
        for (i = 0; i < mNumBuffers; i++) {
            err = grDev->alloc(grDev,
            nativeWindowBuf[i].width, nativeWindowBuf[i].height,
            nativeWindowBuf[i].format, nativeWindowBuf[i].usage,
            &nativeWindowBuf[i].handle, &nativeWindowBuf[i].stride);

            ALOGE_IF(err, "fb buffer %d allocation failed w=%d, " \
                     "h=%d, err=%s",
                     i, fbDev->width, fbDev->height, strerror(-err));

            if (err)
            {
                mNumBuffers = i;
                break;
            }
        }
        mDrawLock.unlock();

        /* loop to go through animation */
        int aCount;
        for (aCount = 0; aCount < NUM_OF_BMPS; aCount++) {
            FILE * pFile;
            long fSize;

            mDrawLock.lock();
            if(property_get("sys.asplash.stop", value, "false")
                    && !strcmp(value, "true")) {
                stop = true;
            } else {
                stop = false;
            }

            if (!fbDev || !grDev || !copybitDev) {
                looping = true;
                mDrawLock.unlock();
                break;
            }

            /* read from bmp file */
            pFile = fopen (file_list[aCount],"r");
            if (pFile!=NULL)
            {
                fseek(pFile,0,SEEK_END);
                fSize = ftell(pFile);
                rewind(pFile);

                inBuf = (char*)malloc(sizeof(char)*fSize);
                fread(gHeader,1,BMP_HEADER_IN_BYTES,pFile);
                fread(inBuf,1,fSize-BMP_HEADER_IN_BYTES,pFile);
                fclose (pFile);
            } else {
                ALOGE("Can't open file=%s", file_list[aCount]);
                goto err_exit;
            }

            /* Use copybit to do flip */
            //src
            src.w = P_SPLASH_IMAGE_WIDTH;
            src.h = P_SPLASH_IMAGE_HEIGHT;
            src.format = COPYBIT_FORMAT_BGR_888;
            src.base = inBuf;
            src.handle = NULL;
            src.horiz_padding = 0;
            src.vert_padding = 0;
            //dst
            dst.w = nativeWindowBuf[native_buf_index].stride;
            dst.h = nativeWindowBuf[native_buf_index].height;
            dst.format = COPYBIT_FORMAT_BGR_888;
            dst.base = NULL;
            dst.handle = const_cast<native_handle_t*>
                            (nativeWindowBuf[native_buf_index].handle);
            dst.horiz_padding = 0;
            dst.vert_padding = 0;
            //src rect
            src_rect.l = 0;
            src_rect.t = 0;
            src_rect.r = src.w;
            src_rect.b = src.h;
            //dst rect
            dst_rect.l = (nativeWindowBuf[native_buf_index].width>src.w)?
                         ((nativeWindowBuf[native_buf_index].width-src.w)/2):
                         (0);
            dst_rect.t = (nativeWindowBuf[native_buf_index].height>src.h)?
                         ((nativeWindowBuf[native_buf_index].height-src.h)/2):
                         (0);
            dst_rect.r = dst_rect.l+src.w;
            dst_rect.b = dst_rect.t+src.h;
            //region
            rect.l = dst_rect.l;
            rect.t = dst_rect.t;
            rect.r = dst_rect.r;
            rect.b = dst_rect.b;
            clip = new copybit_iterator(rect);
            if (clip == NULL) {
                ALOGE("can't alloc clip");
                goto err_exit;
            } else {
                if (!fbDev || !grDev || !copybitDev) {
                    looping = true;
                    mDrawLock.unlock();
                    break;
                }
                err = copybitDev->set_parameter(copybitDev, COPYBIT_TRANSFORM,
                            COPYBIT_TRANSFORM_FLIP_V);
                if (err != 0) {
                    ALOGE_IF(err, "set_parameter FLIP_V error=%d (%s)",
                                err, strerror(-err));
                    goto err_exit;
                }
                if (!fbDev || !grDev || !copybitDev) {
                    looping = true;
                    mDrawLock.unlock();
                    break;
                }
                err = copybitDev->set_parameter(copybitDev,
                                COPYBIT_BACKGROUND_COLOR, 0xFFFFFFFF);
                if (err != 0) {
                    ALOGE_IF(err, "set_parameter BG error=%d (%s)",
                             err, strerror(-err));
                    goto err_exit;
                }
                if (!fbDev || !grDev || !copybitDev) {
                    looping = true;
                    mDrawLock.unlock();
                    break;
                }
                err = copybitDev->sw_blit(copybitDev, &dst, &src, &dst_rect,
                                    &src_rect, (struct copybit_region_t *)clip);
                if (err != 0) {
                    ALOGE_IF(err, "sw_blit error=%d (%s)",
                             err, strerror(-err));
                    goto err_exit;
                } else {
                    ALOGD("sw_blit succeed!");
                }
            }

            void *vaddr = NULL;
            if (!fbDev || !grDev || !copybitDev) {
                looping = true;
                mDrawLock.unlock();
                break;
            }
            err = grModule->lock(grModule,
                    nativeWindowBuf[native_buf_index].handle,
                    (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK),
                    0,0,fbDev->width, fbDev->height, &vaddr);

            if(err != 0) {
                ALOGE("gralloc lock fail\n");
                goto err_exit;
            }

            if (!fbDev || !grDev || !copybitDev) {
                looping = true;
                mDrawLock.unlock();
                break;
            }
            /* post to display */
            framebuffer_device_t* fb = fbDev;
            fb->post(fb, nativeWindowBuf[native_buf_index].handle);
            if (ack && !sendAck) {
                // Acknowledge the MDP arb
                err = ioctl(arb_fd, MSMFB_ARB_ACKNOWLEDGE, &event);
                if (err) {
                    ALOGE("%s mdp arb ack fails=%d", __FUNCTION__, err);
                    err = 0;
                }
                sendAck = true;
            }
            if (!fbDev || !grDev || !copybitDev) {
                looping = true;
                mDrawLock.unlock();
                break;
            }
            mDrawLock.unlock();
            usleep(SLEEP_EACH_FRAME_IN_US);
            mDrawLock.lock();

            /*hold last image for 2s */
            if (aCount == (NUM_OF_BMPS - 1)){
                mDrawLock.unlock();
                usleep(SLEEP_LAST_FRAME_IN_US);
                mDrawLock.lock();
            }
            if (!fbDev || !grDev || !copybitDev) {
                looping = true;
                mDrawLock.unlock();
                break;
            }

            err = grModule->unlock(grModule,
                    nativeWindowBuf[native_buf_index].handle);
            native_buf_index++;
            if (native_buf_index >= mNumBuffers)
                native_buf_index = 0;
            free(inBuf);
            inBuf = NULL;

            if (keepRunning && !stop) {
                looping = true;
                if (aCount == NUM_OF_BMPS - 1) {
                    aCount = -1;
                }
            } else {
                looping = false;
            }
            mDrawLock.unlock();
        }
        ALOGD("Finished!\n");
    } else {
        ALOGE("couldn't get gralloc module\n");
        err = -1;
        goto err_exit;
    }

    return err;

err_exit:
    mDrawLock.unlock();
    /* close devices and free module */
    cleanUpResources(true, ack, event);
    return err;
}

static void sigHandler(int sig)
{
    ALOGI("%s, sig=%d", __FUNCTION__, sig);
    cleanUpResources(true, false, MDP_ARB_NOTIFICATION_DOWN);
    signal(sig, SIG_DFL);
    kill(getpid(), sig);
}

static void handleMdpArbEvent(const mdp_arb_notification_event event,
                              int *fbIdx,
                              int numFbIdx,
                              int eventState)
{
    int ret = 0;
    int i = 0;

    for (i = 0; i < numFbIdx; i++) {
        if (fbIdx[i] < 0) {
            continue;
        }
        switch(event) {
            case MDP_ARB_NOTIFICATION_UP:
                ALOGD("Received Up Event=%d state=%d", event, eventState);
                gRegArb = false;
                gAck = true;
                gEvent = event;
                if (gRunning) {
                    mWaitForFinishLock.wait();
                }
                mEventLock.signal();
                break;
            case MDP_ARB_NOTIFICATION_DOWN:
                ALOGD("Received Down Event=%d state=%d", event, eventState);
                cleanUpResources(false, true, event);
                break;
            case MDP_ARB_NOTIFICATION_OPTIMIZE:
            default:
                ALOGE("%s: Invalid reverse camera state %d", __FUNCTION__,
                    event);
                break;
        }
    }
}

/* Parse uevent data for action requested for the display */
static bool getMdpArbNotification(const char* strUdata,
                                  int len,
                                  mdp_arb_notification_event& event,
                                  int *fbIdx,
                                  int numFbIdx,
                                  int& eventState)
{
    const char* iter_str = strUdata;
    char iter_temp[UEVENT_STRING_LEN_MAX];
    char *p = NULL;
    bool found = false;
    char *token = NULL, *last = NULL;
    const char *delimit = ", ";
    int idx = 0, l = 0, c = 0;
    char* pstr = NULL;
    int i = 0, j = 0;

    if (strcasestr(MDP_ARB_UEVENT_PATH, strUdata)) {
        while(((iter_str - strUdata) <= len)) {
            memset(iter_temp, 0x00, sizeof(iter_temp));
            strlcpy(iter_temp, iter_str, UEVENT_STRING_LEN_MAX);
            if ((strstr(iter_temp, CLIENT_NAME)) != NULL) {
                if ((pstr = strstr(iter_temp, MDP_ARB_UEVENT_OPTIMIZE_PREFIX))
                        != NULL) {
                    event = MDP_ARB_NOTIFICATION_OPTIMIZE;
                    p = pstr + strlen(MDP_ARB_UEVENT_OPTIMIZE_PREFIX);
                } else if ((pstr = strstr(iter_temp,
                        MDP_ARB_UEVENT_DOWN_PREFIX)) != NULL) {
                    event = MDP_ARB_NOTIFICATION_DOWN;
                    p = pstr + strlen(MDP_ARB_UEVENT_DOWN_PREFIX);
                } else if ((pstr = strstr(iter_temp, MDP_ARB_UEVENT_UP_PREFIX))
                        != NULL) {
                    event = MDP_ARB_NOTIFICATION_UP;
                    p = pstr + strlen(MDP_ARB_UEVENT_UP_PREFIX);
                } else {
                    ALOGE("%s can't find notification string, u=%s",
                        __FUNCTION__, iter_temp);
                    continue;
                }
                l = strlen(p);
                token = strtok_r(p, delimit, &last);
                i = 0;
                c =  strlen(token);
                while((NULL != token) && (c <= l)) {
                    if (!strncmp(token, CLIENT_NAME, strlen(CLIENT_NAME))) {
                        fbIdx[idx] = i;
                        idx++;
                        if (idx > numFbIdx) {
                            ALOGI("%s idx=%d is bigger than numFbIdx=%d",
                                __FUNCTION__, idx, numFbIdx);
                            break;
                        }
                    }
                    c += strlen(token);
                    token = strtok_r(NULL, delimit, &last);
                    i++;
                }
                found = true;
            }
            if ((pstr = strstr(iter_temp, MDP_ARB_UEVENT_FB_IDX_PREFIX))
                != NULL) {
                p = pstr + strlen(MDP_ARB_UEVENT_FB_IDX_PREFIX);
                l = strlen(p);
                token = strtok_r(p, delimit, &last);
                i = 0;
                j = 0;
                c = strlen(token);
                while((token) && (j < idx) && (c <= l)) {
                    if (i == fbIdx[j]) {
                        if (token) {
                            fbIdx[j] = atoi(token);
                        } else {
                            ALOGI("%s token is NULL, iter_str=%s,i=%d,j=%d,"\
                                "idx=%d", __FUNCTION__, iter_str, i, j, idx);
                        }
                        j++;
                    }
                    c += strlen(token);
                    token = strtok_r(NULL, delimit, &last);
                    i++;
                }
            }
            if ((pstr = strstr(iter_temp, MDP_ARB_UEVENT_STATE_PREFIX))
                != NULL) {
                p = pstr + strlen(MDP_ARB_UEVENT_STATE_PREFIX);
                eventState = atoi(p);
            }
            iter_str += strlen(iter_str)+1;
        }
    }
    return found;
}


static void handle_uevent(const char* udata, int len)
{
    mdp_arb_notification_event event;
    int fb_idx[1] = {-1};
    int state = -1;
    bool update = getMdpArbNotification(udata, len, event, fb_idx,
                    sizeof(fb_idx)/sizeof(int), state);
    if (update)
        handleMdpArbEvent(event, fb_idx, sizeof(fb_idx)/sizeof(int), state);
}

static void *uevent_loop(void *param)
{
    int len = 0;
    static char udata[PAGE_SIZE];
    char thread_name[64] = UEVENT_THREAD_NAME;
    prctl(PR_SET_NAME, (unsigned long) &thread_name, 0, 0, 0);
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
    if(!uevent_init()) {
        ALOGE("%s: failed to init uevent ",__FUNCTION__);
        return NULL;
    }

    while(1) {
        len = uevent_next_event(udata, sizeof(udata) - 2);
        handle_uevent(udata, len);
    }

    return NULL;
}

static void init_uevent_thread(void)
{
    pthread_t uevent_thread;
    int ret;

    ALOGI("Initializing UEVENT Thread");
    ret = pthread_create(&uevent_thread, NULL, uevent_loop, NULL);
    if (ret) {
        ALOGE("failed to create uevent thread %s", strerror(ret));
    }
}

int main(void)
{
    place_marker("ASPLASH: start");
    int err = 0;
    bool looping = false;

    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGSEGV, sigHandler);
    signal(SIGQUIT, sigHandler);
    signal(SIGKILL, sigHandler);
    signal(SIGHUP,  sigHandler);
    signal(SIGSTOP, sigHandler);
    signal(SIGTSTP, sigHandler);

    init_uevent_thread();

    gRegArb = true;
    gAck = false;

    do {
        looping = false;
        err = drawSplash(gRegArb, gAck, gEvent, looping);
        if (err) {
            ALOGE("drawSplash error=%d", err);
            break;
        }
        gRunning = false;
        if (looping) {
            mWaitForFinishLock.signal();
            mEventLock.wait();
        }
    } while(looping);

    cleanUpResources(true, false, MDP_ARB_NOTIFICATION_DOWN);
    return 0;
}
