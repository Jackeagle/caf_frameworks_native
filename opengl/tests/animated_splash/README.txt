---------------------------------------------------------------------------------------

ANIMATED SPLASH README.TXT

---------------------------------------------------------------------------------------

Before using the animated splash feature, please ensure the following is in order:

Step 1) Push splash image bmp content to /data/ directory on device
On target device:
	- adb root
	- adb remount
	- adb push /BMP_content_folder/ /data/

Step 2) Update the file names in the animated_splash.cpp file "char
		file_list[][MAX_FILENAME_LENGTH]" structure.

Step 3) Ensure the .bmp files are in the correct format defined by
		"BMP_PIXEL_FORMAT"

Step 4) Need to manually build app and push to device.
In <buildroot>/frameworks/native/opengl/tests/animated_splash:
	- mm
On target device:
	- adb root
	- adb remount
	- adb push <buildroot>/<outdir>/system/bin/animated-splash /system/bin/animated-splash

Step 5) Double check that the app is executable:
On target device:
	- adb shell
	- cd system/bin
	- ls -al ani*
	(make sure the rwx is seen)
If not executable:
	- chmod 777 animated-splash

Step 6) After doing the above steps, the app and BMP files will be stored on the
		device and will not need to be pushed again when the device is rebooted unless changes
		are made to the app or new BMP files need to be added. The next time the 
		device boots, the application will be automatically launched by the 
		init.rc script. 

NOTES:
------
A. Please ensure the following defines at the top of the file are in check or modified if need be:

	#define MAX_FILENAME_LENGTH 20
	#define NUM_OF_BMPS 16
	#define BMP_HEADER_IN_BYTES 54
	#define P_SPLASH_IMAGE_WIDTH     1280
	#define P_SPLASH_IMAGE_HEIGHT    768
	#define P_SPLASH_2S_HOLD 26
	#define BMP_PIXEL_FORMAT HAL_PIXEL_FORMAT_BGR_888
	#define BMP_BPP 3

B. Uncomment out any one of following line to choose which display is being tested.
#define TEST_FB GRALLOC_HARDWARE_FB_PRIMARY
//#define TEST_FB GRALLOC_HARDWARE_FB_SECONDARY
//#define TEST_FB GRALLOC_HARDWARE_FB_TERTIARY

C. Direct memory address access through private_handle_t is not supported anymore.
Please use copybit for any memory blit.

D. To debug after system boots up:
	- adb root
	- adb shell
	- ./system/bin/animated-splash
	- logs will get printed to logcat
	- you can grep for log tag LOG_TAG "ASPLASH" in adb logcat to view more
	  easily. (adb logcat | grep "ASPLASH")

E. System properties for feature control:
	- sys.asplash.keep_running
	  Setting this property to true before running application could trigger application to
	  non-stop mode.
	- sys.asplash.stop
	  Setting this property to true results application to exit from non-stop
	  mode.