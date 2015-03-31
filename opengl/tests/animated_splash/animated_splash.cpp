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
#include <cutils/log.h>
#include <cutils/native_handle.h>
#include <sys/cdefs.h>
#include <stdint.h>
#include <linux/fb.h>
#include <copybit.h>

#define TEST_FB GRALLOC_HARDWARE_FB_PRIMARY
//#define TEST_FB GRALLOC_HARDWARE_FB_SECONDARY
//#define TEST_FB GRALLOC_HARDWARE_FB_TERTIARY

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

void place_marker(const char* str)
{
    FILE *fp;

    fp = fopen( "/proc/bootkpi/marker_entry" , "w" );
    if (fp) {
        fwrite(str , 1 , strlen(str) , fp );
        fclose(fp);
    }
}

int main(void)
{
    place_marker("ASPLASH: start");
    hw_module_t const* module;
    framebuffer_device_t* fbDev;
    alloc_device_t* grDev;
    hw_module_t const* copybit_module;
    copybit_device_t* copybitDev;
    char * inbuf;
    int err = 0;
    char * inBuf = NULL;
    int stride = 0;
    int i = 0;
    int32_t mNumBuffers = 0;
    int32_t mNumFreeBuffers = 0;
    int32_t mBufferHead = 0;
    copybit_image_t src, dst;
    copybit_iterator* clip = NULL;
    copybit_rect_t rect;
    copybit_rect_t src_rect;
    copybit_rect_t dst_rect;
    ANativeWindowBuffer nativeWindowBuf[MAX_NUM_FRAME_BUFFERS];
    int native_buf_index = 0;
    char gHeader[BMP_HEADER_IN_BYTES];

    char file_list[][MAX_FILENAME_LENGTH] =
    {
        "/data/pan_1.bmp",  "/data/pan_2.bmp",  "/data/pan_3.bmp",
        "/data/pan_4.bmp",  "/data/pan_5.bmp",  "/data/pan_6.bmp",
        "/data/pan_7.bmp",  "/data/pan_8.bmp",  "/data/pan_9.bmp",
        "/data/pan_10.bmp", "/data/pan_11.bmp", "/data/pan_12.bmp",
        "/data/pan_13.bmp", "/data/pan_14.bmp", "/data/pan_15.bmp",
        "/data/pan_16.bmp"
    };

    if (hw_get_module(COPYBIT_HARDWARE_MODULE_ID, &copybit_module) != 0) {
        ALOGE("ASPLASH: Can't open copybit module");
        return 0;
    } else {
        err = copybit_open(copybit_module, &copybitDev);
        if((err < 0) || (copybitDev == NULL)) {
            ALOGE_IF(err, "ASPLASH: couldn't open copybit HAL (%s)",
                        strerror(-err));
            return 0;
        }
    }


    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {

        gralloc_module_t const* grModule =
        reinterpret_cast<gralloc_module_t const*>(module);
        err = framebuffer_open(module, TEST_FB, &fbDev);
        if(err){
            ALOGE_IF(err, "ASPLASH: couldn't open framebuffer HAL (%s)",
                        strerror(-err));
        }

        err = gralloc_open(module, &grDev);
        if(err){
            ALOGE_IF(err, "ASPLASH: couldn't open gralloc HAL (%s)",
                        strerror(-err));
        }

        if (!fbDev || !grDev)
            return -1;

        if(fbDev->numFramebuffers >= MIN_NUM_FRAME_BUFFERS &&
            fbDev->numFramebuffers <= MAX_NUM_FRAME_BUFFERS){
            mNumBuffers = fbDev->numFramebuffers;
        } else {
            mNumBuffers = MIN_NUM_FRAME_BUFFERS;
        }

        /* setup buffers*/
        for (i = 0; i < mNumBuffers; i++)
        {
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
        for (i = 0; i < mNumBuffers; i++)
        {
            err = grDev->alloc(grDev,
            nativeWindowBuf[i].width, nativeWindowBuf[i].height,
            nativeWindowBuf[i].format, nativeWindowBuf[i].usage,
            &nativeWindowBuf[i].handle, &nativeWindowBuf[i].stride);

            ALOGE_IF(err, "ASPLASH: fb buffer %d allocation failed w=%d, " \
                     "h=%d, err=%s",
                     i, fbDev->width, fbDev->height, strerror(-err));

            if (err)
            {
                mNumBuffers = i;
                break;
            }
        }

        /* loop to go through animation */
        int aCount;
        for (aCount = 0; aCount < NUM_OF_BMPS; aCount++) {
            FILE * pFile;
            long fSize;

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
                ALOGE("ASPLASH: Can't open file=%s", file_list[aCount]);
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
            rect.l = 0;
            rect.t = 0;
            rect.r = src.w;
            rect.b = src.h;
            clip = new copybit_iterator(rect);
            if (clip == NULL) {
                ALOGE("ASPLASH: can't alloc clip");
                goto err_exit;
            } else {
                err = copybitDev->set_parameter(copybitDev, COPYBIT_TRANSFORM,
                            COPYBIT_TRANSFORM_FLIP_V);
                if (err != 0) {
                    ALOGE_IF(err, "ASPLASH: set_parameter FLIP_V error=%d (%s)",
                                err, strerror(-err));
                    goto err_exit;
                }
                err = copybitDev->set_parameter(copybitDev,
                                COPYBIT_BACKGROUND_COLOR, 0xFFFFFFFF);
                if (err != 0) {
                    ALOGE_IF(err, "ASPLASH: set_parameter BG error=%d (%s)",
                             err, strerror(-err));
                    goto err_exit;
                }
                err = copybitDev->sw_blit(copybitDev, &dst, &src, &dst_rect,
                                    &src_rect, (struct copybit_region_t *)clip);
                if (err != 0) {
                    ALOGE_IF(err, "ASPLASH: sw_blit error=%d (%s)",
                             err, strerror(-err));
                    goto err_exit;
                } else {
                    ALOGD("ASPLASH: sw_blit succeed!");
                }
            }

            void *vaddr = NULL;
            err = grModule->lock(grModule,
                    nativeWindowBuf[native_buf_index].handle,
                    (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK),
                    0,0,fbDev->width, fbDev->height, &vaddr);

            if(err != 0) {
                ALOGE("ASPLASH:  gralloc lock fail\n");
            }

            /* post to display */
            framebuffer_device_t* fb = fbDev;
            fb->post(fb, nativeWindowBuf[native_buf_index].handle);
            usleep(SLEEP_EACH_FRAME_IN_US);

            /*hold last image for 2s */
            if (aCount == (NUM_OF_BMPS - 1)){
                usleep(SLEEP_LAST_FRAME_IN_US);
            }

            err = grModule->unlock(grModule,
                    nativeWindowBuf[native_buf_index].handle);
            native_buf_index++;
            if (native_buf_index >= mNumBuffers)
                native_buf_index = 0;
            free(inBuf);
            inBuf = NULL;
        }
        ALOGD("ASPLASH Finished!\n");
    } else {
        ALOGE("ASPLASH couldn't get gralloc module\n");
    }

err_exit:
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

    return 0;
}
