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
#include <gralloc_priv.h>

#define MIN_NUM_FRAME_BUFFERS  1
#define MAX_NUM_FRAME_BUFFERS  2

#define LOG_TAG "ASPLASH"
#define MAX_FILENAME_LENGTH 20
#define NUM_OF_BMPS 16
#define BMP_HEADER_IN_BYTES 54
#define P_SPLASH_IMAGE_WIDTH     1280
#define P_SPLASH_IMAGE_HEIGHT    768
#define P_SPLASH_2S_HOLD 26
#define BMP_PIXEL_FORMAT HAL_PIXEL_FORMAT_BGR_888
#define BMP_BPP 3

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
	char * inbuf;

	char file_list[][MAX_FILENAME_LENGTH] =
	{
		"/data/pan_1.bmp","/data/pan_2.bmp","/data/pan_3.bmp","/data/pan_4.bmp",
		"/data/pan_5.bmp","/data/pan_6.bmp","/data/pan_7.bmp","/data/pan_8.bmp",
		"/data/pan_9.bmp","/data/pan_10.bmp","/data/pan_11.bmp","/data/pan_12.bmp",
		"/data/pan_13.bmp","/data/pan_14.bmp","/data/pan_15.bmp","/data/pan_16.bmp"
	};

	if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
		int stride;
		int err;
		int i,j;
		int32_t mNumBuffers;
		int32_t mNumFreeBuffers;
		int32_t mBufferHead;
		int spl_x_start, spl_y_start;

		struct private_handle_t *g_handle_off[MAX_NUM_FRAME_BUFFERS];
		ANativeWindowBuffer nativeWindowBuf[MAX_NUM_FRAME_BUFFERS];

		gralloc_module_t const* grModule =
		reinterpret_cast<gralloc_module_t const*>(module);
		err = framebuffer_open(module, &fbDev);
		if(err){
			ALOGE_IF(err, "ASPLASH: couldn't open framebuffer HAL (%s)", strerror(-err));
		}

		err = gralloc_open(module, &grDev);
		if(err){
			ALOGE_IF(err, "ASPLASH: couldn't open gralloc HAL (%s)", strerror(-err));
		}

		if (!fbDev || !grDev)
			return -1;

		if(fbDev->numFramebuffers >= MIN_NUM_FRAME_BUFFERS &&
			fbDev->numFramebuffers <= MAX_NUM_FRAME_BUFFERS){
			mNumBuffers = fbDev->numFramebuffers;
		} else {
			mNumBuffers = MIN_NUM_FRAME_BUFFERS;
		}
		mNumFreeBuffers = mNumBuffers;
		mBufferHead = mNumBuffers-1;

		/* setup buffers*/
		for (i = 0; i < mNumBuffers; i++)
		{
			nativeWindowBuf[i].width = fbDev->width;
			nativeWindowBuf[i].height = fbDev->height;
			nativeWindowBuf[i].format = BMP_PIXEL_FORMAT;
			nativeWindowBuf[i].usage = GRALLOC_USAGE_SW_WRITE_RARELY|GRALLOC_USAGE_SW_READ_RARELY;
		}

		/* allocate buffers*/
		for (i = 0; i < mNumBuffers; i++)
		{
			err = grDev->alloc(grDev,
			fbDev->width, fbDev->height, nativeWindowBuf[i].format,
			nativeWindowBuf[i].usage,
			&nativeWindowBuf[i].handle, &nativeWindowBuf[i].stride);
			g_handle_off[i] =
				static_cast< private_handle_t*>(const_cast<native_handle_t*>(nativeWindowBuf[i].handle));

			ALOGE_IF(err, "ASPLASH: fb buffer %d allocation failed w=%d, h=%d, err=%s",
			i, fbDev->width, fbDev->height, strerror(-err));

			if (err)
			{
				mNumBuffers = i;
				mNumFreeBuffers = i;
				mBufferHead = mNumBuffers-1;
				break;
			}
		}

		/* get pointer to base fb addr */
		char *memptr = (char *)g_handle_off[0]->base;
		if (memptr == NULL)
			return -1;

		/* centering image*/
		spl_x_start = (nativeWindowBuf[0].width - P_SPLASH_IMAGE_WIDTH) * BMP_BPP;
		spl_y_start = (nativeWindowBuf[0].height - (P_SPLASH_IMAGE_HEIGHT)) * BMP_BPP;
		if (spl_y_start <= 0)
			spl_y_start = 0;
		else
			spl_y_start = spl_y_start/2;
		if (spl_x_start <= 0)
			spl_x_start = 0;
		else
			spl_x_start = spl_x_start/2;

		/* blanking background to white */
		memset(memptr, 0xFF, nativeWindowBuf[0].stride*nativeWindowBuf[0].height*BMP_BPP);

		/* loop to go through animation */
		int aCount;
		for (aCount = 0; aCount < NUM_OF_BMPS; aCount++){

			FILE * pFile;
			long fSize;
			char * inBuf;
			char * gHeader;

			/* read from bmp file */
			pFile = fopen (file_list[aCount],"r");
			if (pFile!=NULL)
			{
				fseek(pFile,0,SEEK_END);
				fSize = ftell(pFile);
				rewind(pFile);

				inBuf = (char*)malloc(sizeof(char)*fSize);
				gHeader = (char*)malloc(sizeof(char)*BMP_HEADER_IN_BYTES);
				fread(gHeader,1,BMP_HEADER_IN_BYTES,pFile);
				fread(inBuf,1,fSize-BMP_HEADER_IN_BYTES,pFile);
				fclose (pFile);
			}

			/* copy to buffer - note bmp file saves vertically flipped */
			unsigned k, l;
			l = (nativeWindowBuf[0].height-1);
			for (k = (0 + spl_y_start); k < (nativeWindowBuf[0].height); k++){
				memcpy(memptr + spl_x_start + k*nativeWindowBuf[0].stride*BMP_BPP,
				inBuf + l* P_SPLASH_IMAGE_WIDTH*BMP_BPP, P_SPLASH_IMAGE_WIDTH*BMP_BPP);
				l--;
			}

			void *vaddr = NULL;
			err = grModule->lock(grModule, g_handle_off[0],
			(GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK),
			0,0,fbDev->width, fbDev->height, &vaddr);

			if(err != 0) {
				ALOGE("ASPLASH:  gralloc lock fail\n");
			}

			/* post to display */
			framebuffer_device_t* fb = fbDev;
			fb->post(fb, g_handle_off[0]);
			fb->post(fb, g_handle_off[0]);
			fb->post(fb, g_handle_off[0]);
			fb->post(fb, g_handle_off[0]);

			/*hold last image for 2s */
			if (aCount == (NUM_OF_BMPS - 1)){
				for (i =0; i < P_SPLASH_2S_HOLD; i++)
					fb->post(fb, g_handle_off[0]);
			}

			err = grModule->unlock(grModule, g_handle_off[0]);
			free(inBuf);
		}

		/* close devices and free module */
		delete []file_list;

		if (grDev) {
			for(int i = 0; i < mNumBuffers; i++) {
				if (nativeWindowBuf[i].handle != NULL) {
					grDev->free(grDev, nativeWindowBuf[i].handle);
				}
			}
			gralloc_close(grDev);
		}

		if (fbDev) {
			framebuffer_close(fbDev);
		}
	} else {
		ALOGE("ASPLASH couldn't get gralloc module\n");
	}
	return 0;
}
