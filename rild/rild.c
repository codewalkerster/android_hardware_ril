/* //device/system/rild/rild.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <telephony/ril.h>
#define LOG_TAG "RILD"
#include <utils/Log.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <linux/capability.h>
#include <linux/prctl.h>
#include <sys/ioctl.h>
#include <private/android_filesystem_config.h>
#include "hardware/qemu_pipe.h"

#define LIB_PATH_PROPERTY   "rild.libpath"
#define LIB_ARGS_PROPERTY   "rild.libargs"
#define MODEM_DEV_PATH	  "/dev/voice_modem"
#define MAX_LIB_ARGS        16
#define BP_IOCTL_BASE 0x1a

#define BP_IOCTL_RESET 		_IOW(BP_IOCTL_BASE, 0x01, int)
#define BP_IOCTL_POWOFF 	_IOW(BP_IOCTL_BASE, 0x02, int)
#define BP_IOCTL_POWON 		_IOW(BP_IOCTL_BASE, 0x03, int)

#define BP_IOCTL_WRITE_STATUS 	_IOW(BP_IOCTL_BASE, 0x04, int)
#define BP_IOCTL_GET_STATUS 	_IOR(BP_IOCTL_BASE, 0x05, int)
#define BP_IOCTL_SET_PVID 	_IOW(BP_IOCTL_BASE, 0x06, int)
#define BP_IOCTL_GET_BPID 	_IOR(BP_IOCTL_BASE, 0x07, int)
static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s -l <ril impl library> [-- <args for impl library>]\n", argv0);
    exit(-1);
}

extern void RIL_register (const RIL_RadioFunctions *callbacks);

extern void RIL_onRequestComplete(RIL_Token t, RIL_Errno e,
                           void *response, size_t responselen);

extern void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen);

extern void RIL_requestTimedCallback (RIL_TimedCallback callback,
                               void *param, const struct timeval *relativeTime);


static struct RIL_Env s_rilEnv = {
    RIL_onRequestComplete,
    RIL_onUnsolicitedResponse,
    RIL_requestTimedCallback
};

extern void RIL_startEventLoop();

static int make_argv(char * args, char ** argv)
{
    // Note: reserve argv[0]
    int count = 1;
    char * tok;
    char * s = args;

    while ((tok = strtok(s, " \0"))) {
        argv[count] = tok;
        s = NULL;
        count++;
    }
    return count;
}

/*
 * switchUser - Switches UID to radio, preserving CAP_NET_ADMIN capabilities.
 * Our group, cache, was set by init.
 */
void switchUser() {
    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
    setuid(AID_RADIO);

    struct __user_cap_header_struct header;
    struct __user_cap_data_struct cap;
    header.version = _LINUX_CAPABILITY_VERSION;
    header.pid = 0;
    cap.effective = cap.permitted = (1 << CAP_NET_ADMIN) | (1 << CAP_NET_RAW);
    cap.inheritable = 0;
    capset(&header, &cap);
}
int getBpID(){
	int bp_fd = -1;
	int biID =-1;
	int err = -1;
	bp_fd = open(MODEM_DEV_PATH, O_RDWR);
	if(bp_fd > 0){		
		err = ioctl(bp_fd,BP_IOCTL_GET_BPID,&biID);
		if(err < 0){
			ALOGE("biID=%d getBpID failed  ioctrl err =%d bp_fd=%d",biID,err,bp_fd);
			close(bp_fd);
			return -1;
		}else{
			ALOGD("biID=%d getBpID sucessed",biID);
			close(bp_fd);
			return biID;
		} 
	}
	ALOGE("biID=%d getBpID failed bp_fd = ",biID,bp_fd);
	return -1;	
}
void startmux(int bp_id){
	char *muxbin =NULL;
	if(bp_id < 0){
		ALOGE("bp_id=%d cann`t found mux bin to start",bp_id);
	}else{
		asprintf(&muxbin, "muxd%d",bp_id); 
		property_set("ctl.start",muxbin); 
		ALOGD("bp_id=%d found %s to start",bp_id,muxbin);
		free(muxbin);
	}
}
int main(int argc, char **argv)
{
    const char * rilLibPath = NULL;
    char **rilArgv;
    void *dlHandle;
    const RIL_RadioFunctions *(*rilInit)(const struct RIL_Env *, int, char **);
    const RIL_RadioFunctions *funcs;
    char libPath[PROPERTY_VALUE_MAX];
    unsigned char hasLibArgs = 0;

    int i;
	int bpID = -1;
	char *rilLibPathTemp = NULL;
    umask(S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH);
    for (i = 1; i < argc ;) {
        if (0 == strcmp(argv[i], "-l") && (argc - i > 1)) {
            rilLibPath = argv[i + 1];
            i += 2;
        } else if (0 == strcmp(argv[i], "--")) {
            i++;
            hasLibArgs = 1;
            break;
        } else {
            usage(argv[0]);
        }
    }
	
	char multiRild[10]= {0};
    property_get("ro.multi.rild", multiRild, "false");
	if(strcmp(multiRild, "true") != 0){
		char dataonly[2];
		property_get("ril.function.dataonly", dataonly, NULL);
		if(strcmp(dataonly,"1") == 0 || strcmp(dataonly,"2") == 0 || strcmp(dataonly,"3") == 0){
			strcpy(libPath,"libril-rk29-dataonly.so");
			property_set("ril.function.dataonly","2");
			rilLibPath = libPath;
		}
	}
    if (rilLibPath == NULL) {
	  bpID = getBpID();
	  if(bpID < 0){
		if ( 0 == property_get(LIB_PATH_PROPERTY, libPath, NULL)) {
            	// No lib sepcified on the command line, and nothing set in props.
            	// Assume "no-ril" case.
            		rilLibPath = "/system/lib/libril-rk29-dataonly.so";
					system("stop ril-daemon3");
            		//goto done;
        	} else {
            		rilLibPath = libPath;
        	}
	  }else{	  	
		asprintf(&rilLibPathTemp, "%s%d",LIB_PATH_PROPERTY,bpID); 
		if ( 0 == property_get(rilLibPathTemp, libPath, NULL)) {
            	// No lib sepcified on the command line, and nothing set in props.
            	// Assume "no-ril" case.
            		free(rilLibPathTemp);
				    rilLibPath = "/system/lib/libril-rk29-dataonly.so";
            		//goto done;
        	} else {
        		ALOGD("rilLibPathTemp=%s found %s to install",rilLibPathTemp,libPath);
        		free(rilLibPathTemp);
            		rilLibPath = libPath;
        	}
	  }
        
    }
    startmux(bpID);
    /* special override when in the emulator */
#if 1
    {
        static char*  arg_overrides[3];
        static char   arg_device[32];
        int           done = 0;

#define  REFERENCE_RIL_PATH  "/system/lib/libreference-ril.so"

        /* first, read /proc/cmdline into memory */
        char          buffer[1024], *p, *q;
        int           len;
        int           fd = open("/proc/cmdline",O_RDONLY);

        if (fd < 0) {
            ALOGD("could not open /proc/cmdline:%s", strerror(errno));
            goto OpenLib;
        }

        do {
            len = read(fd,buffer,sizeof(buffer)); }
        while (len == -1 && errno == EINTR);

        if (len < 0) {
            ALOGD("could not read /proc/cmdline:%s", strerror(errno));
            close(fd);
            goto OpenLib;
        }
        close(fd);

        if (strstr(buffer, "android.qemud=") != NULL)
        {
            /* the qemud daemon is launched after rild, so
            * give it some time to create its GSM socket
            */
            int  tries = 5;
#define  QEMUD_SOCKET_NAME    "qemud"

            while (1) {
                int  fd;

                sleep(1);

                fd = qemu_pipe_open("qemud:gsm");
                if (fd < 0) {
                    fd = socket_local_client(
                                QEMUD_SOCKET_NAME,
                                ANDROID_SOCKET_NAMESPACE_RESERVED,
                                SOCK_STREAM );
                }
                if (fd >= 0) {
                    close(fd);
                    snprintf( arg_device, sizeof(arg_device), "%s/%s",
                                ANDROID_SOCKET_DIR, QEMUD_SOCKET_NAME );

                    arg_overrides[1] = "-s";
                    arg_overrides[2] = arg_device;
                    done = 1;
                    break;
                }
                ALOGD("could not connect to %s socket: %s",
                    QEMUD_SOCKET_NAME, strerror(errno));
                if (--tries == 0)
                    break;
            }
            if (!done) {
                ALOGE("could not connect to %s socket (giving up): %s",
                    QEMUD_SOCKET_NAME, strerror(errno));
                while(1)
                    sleep(0x00ffffff);
            }
        }

        /* otherwise, try to see if we passed a device name from the kernel */
        if (!done) do {
#define  KERNEL_OPTION  "android.ril="
#define  DEV_PREFIX     "/dev/"

            p = strstr( buffer, KERNEL_OPTION );
            if (p == NULL)
                break;

            p += sizeof(KERNEL_OPTION)-1;
            q  = strpbrk( p, " \t\n\r" );
            if (q != NULL)
                *q = 0;

            snprintf( arg_device, sizeof(arg_device), DEV_PREFIX "%s", p );
            arg_device[sizeof(arg_device)-1] = 0;
            arg_overrides[1] = "-d";
            arg_overrides[2] = arg_device;
            done = 1;

        } while (0);

        if (done) {
            argv = arg_overrides;
            argc = 3;
            i    = 1;
            hasLibArgs = 1;
            rilLibPath = REFERENCE_RIL_PATH;

            ALOGD("overriding with %s %s", arg_overrides[1], arg_overrides[2]);
        }
    }
OpenLib:
#endif
   // switchUser();
    ALOGD("Do not switch user to radio");
    dlHandle = dlopen(rilLibPath, RTLD_NOW);

    if (dlHandle == NULL) {
        ALOGE("dlopen failed: %s", dlerror());
        exit(-1);
    }

    RIL_startEventLoop();

    rilInit = (const RIL_RadioFunctions *(*)(const struct RIL_Env *, int, char **))dlsym(dlHandle, "RIL_Init");

    if (rilInit == NULL) {
        ALOGE("RIL_Init not defined or exported in %s\n", rilLibPath);
        exit(-1);
    }

    if (hasLibArgs) {
        rilArgv = argv + i - 1;
        argc = argc -i + 1;
    } else {
        static char * newArgv[MAX_LIB_ARGS];
        static char args[PROPERTY_VALUE_MAX];
        rilArgv = newArgv;
        property_get(LIB_ARGS_PROPERTY, args, "");
        argc = make_argv(args, rilArgv);
    }

    // Make sure there's a reasonable argv[0]
    rilArgv[0] = argv[0];

    funcs = rilInit(&s_rilEnv, argc, rilArgv);

    RIL_register(funcs);

done:

    while(1) {
        // sleep(UINT32_MAX) seems to return immediately on bionic
        sleep(0x00ffffff);
    }
}

