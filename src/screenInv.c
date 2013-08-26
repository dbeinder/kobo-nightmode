#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <unistd.h>

#include <fcntl.h>
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

#include "iniParser/iniparser.h"
#include "screenInv.h"


static void initialize() __attribute__((constructor));
static void cleanup() __attribute__((destructor));
int ioctl(int filp, unsigned long cmd, unsigned long arg);

static int (*ioctl_orig)(int filp, unsigned long cmd, unsigned long arg) = NULL;
static pthread_t cmdReaderThread = 0, buttonReaderThread;
static struct mxcfb_update_data fullUpdRegion, workaroundRegion;
static int fb0fd = 0;
static time_t configLastChange = 0;

static dictionary* configIni = NULL;
static bool inversionActive = false;
static bool retainState = false;
static int longPressTimeout = 800;
static int thresholdScreenArea = 0;
static int nightRefresh = 3;
static int nightRefreshCnt = 0;

static void forceUpdate()
{
	fullUpdRegion.flags = inversionActive? EPDC_FLAG_ENABLE_INVERSION : 0;
    int ret = ioctl_orig(fb0fd , MXCFB_SEND_UPDATE, (unsigned long int)&fullUpdRegion);
    
    if(ret < 0)
    {
        DEBUGPRINT("ScreenInverter: Full redraw failed! Error %d: %s\n", ret, strerror(errno));
        #ifdef SI_DEBUG
        system("dmesg | grep mxc_epdc_fb: > /mnt/onboard/.kobo/screenInvertLogFB");
        #endif
    }
}

static void readConfigFile(bool readState)
{
    configIni = iniparser_load(SI_CONFIG_FILE);
    if (configIni != NULL)
    {
        if(readState)
            inversionActive = iniparser_getboolean(configIni, "state:invertActive", 0);
        
        retainState = iniparser_getboolean(configIni, "state:retainStateOverRestart", 0);
        longPressTimeout = iniparser_getint(configIni, "control:longPressDurationMS", 800);
        nightRefresh = iniparser_getint(configIni, "nightmode:refreshScreenPages", 3);
        
        if(longPressTimeout < 1) longPressTimeout = 800;
        if(nightRefresh < 1) nightRefresh = 0;
        
        DEBUGPRINT("ScreenInverter: Read config: invert(%s), retain(%s), longPressTimeout(%d), nightRefresh(%d)\n", 
					inversionActive? "yes" : "no",
					retainState? "yes" : "no", 
					longPressTimeout, nightRefresh);
    }
    else
        DEBUGPRINT("ScreenInverter: No config file invalid or not found, using defaults\n");
}

static time_t getLastConfigChange()
{
    struct stat confStat;
    if(stat(SI_CONFIG_FILE, &confStat))
    {
        DEBUGPRINT("ScreenInverter: Could no stat() config file\n");
        return 0;
    }
    
    return confStat.st_ctime;
}

static void setNewState(bool newState)
{
    inversionActive = newState;
    forceUpdate();
    
    if(getLastConfigChange() != configLastChange)
        readConfigFile(false);
    
    if(retainState)
    {
        iniparser_set(configIni, "state:invertActive", inversionActive? "yes" : "no");
        FILE *configFP = fopen(SI_CONFIG_FILE, "w");
        if(configFP != NULL)
        {
            fprintf(configFP, "# config file for kobo-nightmode\n\n");
            iniparser_dump_ini(configIni, configFP);
            fclose(configFP);
        }
        
        configLastChange = getLastConfigChange();
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
            setNewState(!inversionActive);
            DEBUGPRINT("ScreenInverter: Toggled using button\n");
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
                timeOut = longPressTimeout;
            else
                timeOut = -1;
        }
    }
    
    return NULL;
}

static void *cmdReader(void *arg)
{
    char input;
    int fd = open(SI_CONTROL_PIPE, O_NONBLOCK);
    
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
                fd = open(SI_CONTROL_PIPE, O_NONBLOCK);
                readEvent.data.fd = fd;
                epoll_ctl(epollFD, EPOLL_CTL_ADD, fd, &readEvent);
                continue;
            }
            
            switch(input)
            {
                case 't': //toggle
                    setNewState(!inversionActive);
                    DEBUGPRINT("ScreenInverter: Toggled\n");
                    break;
                case 'y': //yes
                    setNewState(true);
                    DEBUGPRINT("ScreenInverter: Inversion on\n");
                    break;
                case 'n': //no
                    setNewState(false);
                    DEBUGPRINT("ScreenInverter: Inversion off\n");
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
    
    #ifdef SI_DEBUG
    char execPath[32];
    int end = readlink("/proc/self/exe", execPath, 31);
    execPath[end] = 0;

    remove(SI_DEBUG_LOGPATH);
    #endif
    
    DEBUGPRINT("ScreenInverter: Hooked to %s!\n", execPath);
    
    unsetenv("LD_PRELOAD");
    DEBUGPRINT("ScreenInverter: Removed LD_PRELOAD!\n");
 
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
    
    thresholdScreenArea = (SI_AREA_THRESHOLD * vinfo.xres * vinfo.yres) / 100;
    
    fullUpdRegion.update_marker = 999;
    fullUpdRegion.update_region.top = 0;
    fullUpdRegion.update_region.left = 0;
    fullUpdRegion.update_region.width = vinfo.xres;
    fullUpdRegion.update_region.height = vinfo.yres;
    fullUpdRegion.waveform_mode = WAVEFORM_MODE_AUTO;
    fullUpdRegion.update_mode = UPDATE_MODE_FULL;
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
    
    remove(SI_CONTROL_PIPE); //just to be sure
    mkfifo(SI_CONTROL_PIPE, 0600);
    pthread_create(&cmdReaderThread, NULL, cmdReader, NULL);
    pthread_create(&buttonReaderThread, NULL, buttonReader, NULL);
    
    readConfigFile(true);
    configLastChange = getLastConfigChange();
}

static void cleanup()
{
    iniparser_freedict(configIni);
    
    if(cmdReaderThread)
    {
        pthread_cancel(cmdReaderThread);
        pthread_cancel(buttonReaderThread);
        remove(SI_CONTROL_PIPE);
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Shut down!\n");
    }
}

int ioctl(int filp, unsigned long cmd, unsigned long arg)
{
    if(inversionActive & (cmd == MXCFB_SEND_UPDATE))
    {
		struct mxcfb_update_data *region = (struct mxcfb_update_data *)arg;
		
        DEBUGPRINT("ScreenInverter: update: type:%s, size:%dx%d (%d%% updated)\n",
					region->update_mode == UPDATE_MODE_PARTIAL? "partial" : "full",
					region->update_region.width, region->update_region.height, 
					(100*region->update_region.width*region->update_region.height) /
						(fullUpdRegion.update_region.width*fullUpdRegion.update_region.height));
        
        ioctl_orig(filp, MXCFB_SEND_UPDATE, (long unsigned)&workaroundRegion);
        //necessary because there's a bug in the driver (or i'm doing it wrong):
        //  i presume the device goes into some powersaving mode when usb und wifi are not used (great for debugging ;-) )
        //  it takes about 10sec after the last touch to enter this mode. after that it is necessary to issue a screenupdate 
        //  without inversion flag, otherwise it will ignore the inversion flag and draw normally (positive).
        //  so i just update a 1px region in the top-right corner, this costs no time and the pixel should be behind the bezel anyway.
        
        
        if(nightRefresh)
        {
			if(region->update_region.width * region->update_region.height >= thresholdScreenArea)
			{
				nightRefreshCnt++;
				if(nightRefreshCnt >= nightRefresh)
				{	
					region->update_region.top = 0;
					region->update_region.left = 0;
					region->update_region.width = fullUpdRegion.update_region.width;
					region->update_region.height = fullUpdRegion.update_region.height;
				    region->update_mode = UPDATE_MODE_FULL;
				    nightRefreshCnt = 0;
				}
				else
					region->update_mode = UPDATE_MODE_PARTIAL;
			}
		}
		
		region->flags ^= EPDC_FLAG_ENABLE_INVERSION;
    }
    
    return ioctl_orig(filp, cmd, arg);
}
