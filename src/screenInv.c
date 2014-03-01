#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

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

#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/types.h>

#include <linux/types.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>

//alternative headers without function definition
#include <asm-generic/ioctl.h>
#include <asm-generic/mman-common.h>

#include "iniParser/iniparser.h"
#include "screenInv.h"

//interposed funcs
int ioctl(int filp, unsigned long cmd, unsigned long arg);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

//original funcs
static int (*ioctl_orig)(int filp, unsigned long cmd, unsigned long arg) = NULL;
static void* (*mmap_orig)(void *addr, size_t length, int prot, int flags, int fd, off_t offset) = NULL;


static void initialize() __attribute__((constructor));
static void cleanup() __attribute__((destructor));

static pthread_t cmdReaderThread = 0, buttonReaderThread;
static struct mxcfb_update_data fullUpdRegion, workaroundRegion;
static int fb0fd = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint16_t *fbMemory = NULL;
static uint16_t *virtualFB = NULL;
static time_t configLastChange = 0;

static dictionary* configIni = NULL;
static bool useHWInvert = false;
static bool inversionActive = false;
static bool retainState = false;
static int longPressTimeout = 800;
static int thresholdScreenArea = 0;
static int nightRefresh = 3;
static int nightRefreshCnt = 0;

static void forceUpdate()
{
    int ret = -1;

    if(useHWInvert)
    {
	    fullUpdRegion.flags = inversionActive? EPDC_FLAG_ENABLE_INVERSION : 0;
        ret = ioctl_orig(fb0fd , MXCFB_SEND_UPDATE, (unsigned long int)&fullUpdRegion);
    }
    else
    {
        fullUpdRegion.flags = 0;
        ret = ioctl(fb0fd , MXCFB_SEND_UPDATE, (unsigned long int)&fullUpdRegion);
    }
	
    #ifdef SI_DEBUG
    if(ret < 0)
    {
        DEBUGPRINT("ScreenInverter: Full redraw failed! Error %d: %s\n", ret, strerror(errno));
        system("dmesg | grep mxc_epdc_fb: > /mnt/onboard/.kobo/screenInvertLogFB");
    }
	#else
	ret++; //avoid compiler warning ;)
    #endif
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
		
		if(iniparser_getboolean(configIni, "nightmode:forceSWInvert", 0))
		{
			DEBUGPRINT("ScreenInverter: Forcing SW inversion mode!\n");
			useHWInvert = false;
        }
		
        if(longPressTimeout < 1) longPressTimeout = 800;
        if(nightRefresh < 1) nightRefresh = 0;
        
        DEBUGPRINT("ScreenInverter: Read config: invert(%s), retain(%s), longPressTimeout(%d), nightRefresh(%d)\n", 
					inversionActive? "yes" : "no",
					retainState? "yes" : "no", 
					longPressTimeout, nightRefresh);
    }
    else
        DEBUGPRINT("ScreenInverter: Config file invalid or not found, using defaults\n");
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

static bool updateVarScreenInfo()
{
    if (ioctl_orig(fb0fd, FBIOGET_FSCREENINFO, (long unsigned)&finfo) < 0)
    {
		DEBUGPRINT("ScreenInverter: Couldn't get framebuffer infos!\n");
        return false;
	}
	
	if (ioctl_orig(fb0fd, FBIOGET_VSCREENINFO, (long unsigned)&vinfo) < 0)
    {
		DEBUGPRINT("ScreenInverter: Couldn't get display dimensions!\n");
		return false;
	}
	
    DEBUGPRINT("ScreenInverter: Got screen resolution: %dx%d @%dBPP, at %d° degrees rotation\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, 90*vinfo.rotate);
    thresholdScreenArea = (SI_AREA_THRESHOLD * vinfo.xres * vinfo.yres) / 100;
    
    fullUpdRegion.update_region.width = vinfo.xres;
    fullUpdRegion.update_region.height = vinfo.yres;
	
	return true;
}

static void initialize()
{
    ioctl_orig = (int (*)(int filp, unsigned long cmd, unsigned long arg)) dlsym(RTLD_NEXT, "ioctl");
    mmap_orig = (void* (*)(void *addr, size_t length, int prot, int flags, int fd, off_t offset)) dlsym(RTLD_NEXT, "mmap");
	
    #ifdef SI_DEBUG
    char execPath[32];
    int end = readlink("/proc/self/exe", execPath, 31);
    execPath[end] = 0;

    remove(SI_DEBUG_LOGPATH);
    #endif
    
    DEBUGPRINT("ScreenInverter: Hooked to %s!\n", execPath);
    
    unsetenv("LD_PRELOAD");
    DEBUGPRINT("ScreenInverter: Removed LD_PRELOAD!\n");
 
    //read device
    FILE *devReader = NULL;
    char codename[32];
    DEBUGPRINT("ScreenInverter: Reading device type: ");
    if((devReader = popen("/bin/kobo_config.sh", "r")) < 0)
    {
	    DEBUGPRINT("... failed!\n");
	    return;
    }

    fgets(codename, sizeof(codename)-1, devReader);
    
    int lastChar = strlen(codename)-1;
    if(codename[lastChar] == '\n')
	    codename[lastChar] = 0;
    
    DEBUGPRINT("%s\n", codename);
    if( !strcmp("pixie", codename) || 
	    !strcmp("trilogy", codename) ||
	    !strcmp("kraken", codename) ||
	    !strcmp("dragon", codename))
    {
	    useHWInvert = true;
	    DEBUGPRINT("ScreenInverter: Device supports HW invert!\n");
    }
    else
	    DEBUGPRINT("ScreenInverter: No HW inversion support, falling back to SW.\n");
    
    pclose(devReader);
    
    if ((fb0fd = open("/dev/fb0", O_RDWR)) == -1)
    {
        DEBUGPRINT("ScreenInverter: Error opening /dev/fb0!\n");
        
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!\n");
        return;
    }
    
    //get the screen's resolution
	if(!updateVarScreenInfo())
	{
        close(fb0fd);
        DEBUGPRINT("ScreenInverter: Disabled!\n");
        return;
	}
	
    fbMemory = (uint16_t*) mmap_orig(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb0fd, 0);

	if ((int)fbMemory == -1) {
		DEBUGPRINT("ScreenInverter: Failed to map framebuffer device to memory.\n");

        close(fb0fd);
		return;
	}

	fullUpdRegion.update_marker = 999;
    fullUpdRegion.update_region.top = 0;
    fullUpdRegion.update_region.left = 0;
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
	
	if(!useHWInvert)
	{
		virtualFB = (uint16_t *) mmap_orig(NULL, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	}
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

static void swCopyRegion(struct mxcfb_rect *region)
{
	int pixelPerLine = finfo.line_length / 2;
	int deltaToNextLine = pixelPerLine - region->width;
	int addr = region->top * pixelPerLine + region->left;
	int xStart = region->width;
	
	for(int yCnt = region->height; yCnt != 0; yCnt--)
    {
		for(int xCnt = xStart; xCnt != 0; xCnt--)
		{
			fbMemory[addr] = virtualFB[addr];
			addr++;
		}
		
		addr += deltaToNextLine;
	}
}

static void swInvertCopy(struct mxcfb_rect *region)
{
	int pixelPerLine = finfo.line_length / 2;
	int deltaToNextLine = pixelPerLine - region->width;
	int addr = region->top * pixelPerLine + region->left;
	int xStart = region->width;
	
	for(int yCnt = region->height; yCnt != 0; yCnt--)
    {
		for(int xCnt = xStart; xCnt != 0; xCnt--)
		{
			fbMemory[addr] = 0xffff - virtualFB[addr];
			addr++;
		}
		
		addr += deltaToNextLine;
	}
}
/*
void benchmark(struct mxcfb_rect *region)
{
	struct timeval stop, start;
	
	gettimeofday(&start, NULL);
	for(int i = 0; i < 10; i++)
	swCopyRegion(region);
	gettimeofday(&stop, NULL);
	uint64_t diff = ((uint64_t)stop.tv_sec * 1000000 + (uint64_t)stop.tv_usec) - ((uint64_t)start.tv_sec * 1000000 + (uint64_t)start.tv_usec);
	DEBUGPRINT("10x copy took %" PRIu64 "\n", diff);
}*/

int ioctl(int filp, unsigned long cmd, unsigned long arg)
{
    if(cmd == MXCFB_SEND_UPDATE)
    {
	
		struct mxcfb_update_data *region = (struct mxcfb_update_data *)arg;

		DEBUGPRINT("ScreenInverter: update: type:%s, flags:0x%x, size:%dx%d (%d%% updated)\n",
					region->update_mode == UPDATE_MODE_PARTIAL? "partial" : "full", region->flags,
					region->update_region.width, region->update_region.height, 
					(100*region->update_region.width*region->update_region.height) /
					(fullUpdRegion.update_region.width*fullUpdRegion.update_region.height));
	
		if(inversionActive && nightRefresh)
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
					DEBUGPRINT("ScreenInverter: nightRefresh: refreshing screen\n");
				}
				else
				{
					region->update_mode = UPDATE_MODE_PARTIAL;
					DEBUGPRINT("ScreenInverter: nightRefresh: no refresh, page %d\n", nightRefreshCnt);
				}
			}
			else
			{
				DEBUGPRINT("ScreenInverter: nightRefresh: small update, ignoring\n");
			}
		}
		
		if(useHWInvert)
		{
			if(inversionActive)
			{
				ioctl_orig(filp, MXCFB_SEND_UPDATE, (long unsigned)&workaroundRegion);
				//necessary because there's a bug in the driver (or i'm doing it wrong):
				//  i presume the device goes into some powersaving mode when usb und wifi are not used (great for debugging ^^)
				//  it takes about 10sec after the last touch to enter this mode. after that it is necessary to issue a screenupdate 
				//  without inversion flag, otherwise it will ignore the inversion flag and draw normally (positive).
				//  so i just update a 1px region in the top-right corner, this costs no time and the pixel should be behind the bezel anyway.
		
				region->flags ^= EPDC_FLAG_ENABLE_INVERSION;
			}
		}
		else
		{
			if(inversionActive)
				swInvertCopy(&region->update_region);
			else
				swCopyRegion(&region->update_region);
		}
    } 
	else if(cmd == FBIOPUT_VSCREENINFO)
	{
		DEBUGPRINT("ScreenInverter: Screen dimensions changed, updating...\n");
		int ret =  ioctl_orig(filp, cmd, arg); //neccessary, since the kernel makes changes to var & fix infos
		updateVarScreenInfo();
		return ret;
	}			
			
    return ioctl_orig(filp, cmd, arg);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	if(!useHWInvert && length == finfo.smem_len)
	{
		char link[32];
		char path[32];
		sprintf(link, "/proc/self/fd/%d", fd);
		int end = readlink(link, path, 31);
		path[end] = 0;
		if(strncmp("/dev/fb0", path, 31) == 0) //okay, nickel is mmap'ing the framebuffer
		{
			DEBUGPRINT("ScreenInverter: mmap'ed the virtual framebuffer!\n");
			return virtualFB;
		}
	}
	return mmap_orig(addr, length, prot, flags, fd, offset);
}
