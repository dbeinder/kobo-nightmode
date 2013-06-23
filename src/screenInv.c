#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/mxcfb.h>

#include <asm-generic/ioctl.h>


//for logging, compile with "make debug"
#define SI_DEBUG_LOGPATH    "/mnt/onboard/.kobo/screenInvertLog"
#ifdef SI_DEBUG
    #define DEBUGPRINT(_fmt, ...) \
        do { \
            FILE *logFP = fopen(SI_DEBUG_LOGPATH, "a"); \
            fprintf(logFP, _fmt, ##__VA_ARGS__); \
            fclose(logFP); \
       } while (0)
#else
    #define DEBUGPRINT(_fmt, ...) /**/
#endif

static void initialize() __attribute__((constructor));
static void cleanup() __attribute__((destructor));
int ioctl(int filp, unsigned long cmd, unsigned long arg);

static const char ctlPipe[] = "/tmp/invertScreen";
static int (*ioctl_orig)(int filp, unsigned long cmd, unsigned long arg) = NULL;
static pthread_t cmdReaderThread = 0, buttonReaderThread;
static bool inversionActive = false;
static struct mxcfb_update_data fullUpdRegion, workaroundRegion;
static int fb0fd = 0;

static void forceUpdate()
{
    int ret = ioctl(fb0fd , MXCFB_SEND_UPDATE, (unsigned long int)&fullUpdRegion);
    
    if(ret < 0)
    {
        DEBUGPRINT("ScreenInverter: Full redraw failed! Error %d: %s\n", ret, strerror(errno));
        #ifdef SI_DEBUG
        system("dmesg | grep mxc_epdc_fb: > /mnt/onboard/.kobo/screenInvertLogFB");
        #endif
    }
}

static void *buttonReader(void *arg)
{
    uint16_t inputBuffer[16];
    int fd = open("/dev/input/event0", O_NONBLOCK);
    
    int epollFD = epoll_create(1);
    struct epoll_event readEvent;
    readEvent.events = EPOLLIN;
    readEvent.data.fd = fd;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
    int timeOut = -1; //infinity
    
    while(1)
    {
        int err = epoll_wait(epollFD, &readEvent, 1, timeOut);
        
        if(err == 0) //nothing to read, but timeout
        {
            timeOut = -1;
            inversionActive = !inversionActive;
            DEBUGPRINT("ScreenInverter: Toggled using button\n");
            forceUpdate();
            continue;
        }
        
        if(err > 0) //data available
        {
            int bytesRead = read(fd, &inputBuffer, sizeof(inputBuffer));
            if(bytesRead != 16)
                continue;
            
            //Buttons:  0x5a -> FrontLight on/off @Glo, 0x66 -> HOME @Touch
            //to see the data for yourself, execute: "hexdump /dev/input/event0"
            if(inputBuffer[6] == 1 && (inputBuffer[5] == 0x5a || inputBuffer[5] == 0x66))
                timeOut = 2000;
            else
                timeOut = -1;
        }
    }
    
    return NULL;
}

static void *cmdReader(void *arg) //reader thread
{
    char input;
    int fd = open(ctlPipe, O_NONBLOCK);
    
    int epollFD = epoll_create(1);
    struct epoll_event readEvent;
    readEvent.events = EPOLLIN;
    readEvent.data.fd = fd;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
    
    while(1)
    {
        int err = epoll_wait(epollFD, &readEvent, 1, -1);
        if(err > 0)
        {
            int bytesRead = read(fd, &input, sizeof(input));
            
            if(bytesRead == 0) //writing application left the pipe -> reopen
            {
                close(fd);
                fd = open(ctlPipe, O_NONBLOCK);
                readEvent.data.fd = fd;
                epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
                continue;
            }
            
            switch(input)
            {
                case 't': //toggle
                    inversionActive = !inversionActive;
                    DEBUGPRINT("ScreenInverter: Toggled\n");
                    forceUpdate();
                    break;
                case 'y': //yes
                    inversionActive = true;
                    DEBUGPRINT("ScreenInverter: Inversion on\n");
                    forceUpdate();
                    break;
                case 'n': //no
                    inversionActive = false;
                    DEBUGPRINT("ScreenInverter: Inversion off\n");
                    forceUpdate();
                    break;
                case 10: //ignore linefeed
                    break;
                default:
                    DEBUGPRINT("ScreenInverter: Unknown command!\n");
                    break;
            }
        }
    }
    
    return NULL;
}

static void initialize()
{
    ioctl_orig = (int (*)(int filp, unsigned long cmd, unsigned long arg)) dlsym(RTLD_NEXT, "ioctl");
    
    char execPath[32];
    int end = readlink("/proc/self/exe", execPath, 31);
    execPath[end] = 0;

    #ifdef SI_DEBUG
    remove(SI_DEBUG_LOGPATH);
    #endif
    
    DEBUGPRINT("ScreenInverter: Hooked to %s!\n", execPath);
    
    unsetenv("LD_PRELOAD");
    DEBUGPRINT("ScreenInverter: Removed LD_PRELOAD!");
 
    if ((fb0fd = open("/dev/fb0", O_RDWR)) == -1)
    {
        DEBUGPRINT("ScreenInverter: Error opening /dev/fb0!\n");
        
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!\n");
        return;
    }
    
    //get the screen's resolution
    struct fb_var_screeninfo vinfo;
    if (ioctl_orig(fb0fd, FBIOGET_VSCREENINFO, (long unsigned)&vinfo) < 0) {
		DEBUGPRINT("ScreenInverter: Couldn't get display dimensions!\n");
		
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!\n");
        return;
	}
    
    DEBUGPRINT("ScreenInverter: Got screen resolution: %dx%d\n", vinfo.xres, vinfo.yres);
    
    fullUpdRegion.update_marker = 999;
    fullUpdRegion.update_region.top = 0;
    fullUpdRegion.update_region.left = 0;
    fullUpdRegion.update_region.width = vinfo.xres;
    fullUpdRegion.update_region.height = vinfo.yres;
    fullUpdRegion.waveform_mode = WAVEFORM_MODE_AUTO;
    fullUpdRegion.update_mode = UPDATE_MODE_PARTIAL;
    fullUpdRegion.temp = TEMP_USE_AMBIENT;
    fullUpdRegion.flags = 0;
    
    workaroundRegion.update_marker = 998;
    workaroundRegion.update_region.top = 0;
    workaroundRegion.update_region.left = 0;
    workaroundRegion.update_region.width = 1;
    workaroundRegion.update_region.height = 1; //1px in the top right(!) corner
    workaroundRegion.waveform_mode = WAVEFORM_MODE_AUTO;
    workaroundRegion.update_mode = UPDATE_MODE_PARTIAL;
    workaroundRegion.temp = TEMP_USE_AMBIENT;
    workaroundRegion.flags = 0;
    
    remove(ctlPipe); //just to be sure
    mkfifo(ctlPipe, 0600);
    pthread_create(&cmdReaderThread, NULL, cmdReader, NULL);
    pthread_create(&buttonReaderThread, NULL, buttonReader, NULL);
}

static void cleanup()
{
    if(cmdReaderThread)
    {
        pthread_cancel(cmdReaderThread);
        pthread_cancel(buttonReaderThread);
        remove(ctlPipe);
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Shut down!\n");
    }
}

int ioctl(int filp, unsigned long cmd, unsigned long arg)
{
    int ret;
    
    if(inversionActive & (cmd == MXCFB_SEND_UPDATE))
    {
        ioctl_orig(filp, MXCFB_SEND_UPDATE, (long unsigned)&workaroundRegion);
        //necessary because there's a bug in the driver (or i'm doing it wrong):
        //  i presume the device goes into some powersaving moden when usb und wifi are not used (great for debugging ;-) )
        //  it takes about 10sec after the last touch to enter this mode. after that it is necessary to issue a screenupdate 
        //  without inversion flag, otherwise it will ignore the inversion flag and draw normally (positive).
        //  so i just update a 1px region in the top-right corner, this costs no time and the pixel should be behind the bezel anyway.
        
        struct mxcfb_update_data *region = (struct mxcfb_update_data *)arg;
        region->flags ^= EPDC_FLAG_ENABLE_INVERSION;
        ret = ioctl_orig(filp, cmd, arg);
        region->flags ^= EPDC_FLAG_ENABLE_INVERSION; //not necessary for nickel, but other apps might not rewrite the request everytime
    }
    else
        ret = ioctl_orig(filp, cmd, arg);
    
//     if(cmdReaderThread)
//     {
//         char nameBuf[256];
//         char linkPath[256];
//         sprintf(linkPath, "/proc/self/fd/%d", filp);
//         int end = readlink(linkPath, nameBuf, 255);
//         nameBuf[end] = 0;
//         printf("IOCTL! dev: %s[%d], cmd: %lu, arg: %lu, ret: %d\n", nameBuf, filp, _IOC_NR(cmd), arg, ret);
//     }
    //if(ret < 0) fprintf(logFP, "IOCTLfailed: %s!\n", strerror(errno));
    //fflush(stdout);
    return ret;
}

/* Code to get the device file from the file descriptor "filp"

    char nameBuf[256];
    char linkPath[256];
    sfprintf(logFP, linkPath, "/proc/self/fd/%d", filp);
    int end = readlink(linkPath, nameBuf, 255);
    nameBuf[end] = 0;
    if(!strncmp(nameBuf, "/dev/fb0", 255)) { modify request}
    
shouldn't be necessary as IOCTLs *should* be unique, and unambiguously identifiable by "cmd"
*/
