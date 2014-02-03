kobo-nightmode
==============

A tiny hack to read white-on-black on Kobo ebook readers. Works only on the eInk series:
+ [Kobo Aura](http://kobo.com/koboaura)
+ [Kobo Aura HD](http://kobo.com/koboaurahd)
+ [Kobo Glo](http://kobo.com/koboglo)
+ [Kobo Touch](http://kobo.com/kobotouch)
+ [Kobo Mini](http://kobo.com/kobomini)

Since the update to firmware 2.6+ Kobo has moved to hardware float proccessing, 
requiring a new toolchain and partly breaking binary compatibility with older software. Currently this hack still works on  firmwares below 2.6, but it is highly recommended to upgrade.
If you need help or find a bug, check the [mobileread thread](http://www.mobileread.com/forums/showthread.php?t=212162), or create an issue on github.

Usage
-----
There are two ways to contol nightmode:
+ Long-press the LIGHT / HOME button on your device to toggle
+ Write into the `/tmp/invertScreen` fifo-interface
  + Send `y` to set nightmode to ON
  + Send `n` to set nightmode to OFF
  + Send `t` to toggle

A sample script to toggle is provided in `extra/nightmode.sh`.
As the Kobo Mini has no physical buttons, the fifo-interface is currently the only way to control nightmode when running and thus, 
relies on external tools such as:
+ [KoboLauncher](http://www.mobileread.com/forums/showthread.php?t=201632), maintained by sergeyvl12
+ [Kobo Tweaks](http://www.mobileread.com/forums/showthread.php?t=206180), maintained by ah- (currently not available for 2.6+)

Configuration
-------------
The configuration file called `nightmode.ini` is located in your `.kobo` folder:
```ini
# config file for kobo-nightmode

[state]
invertActiveOnStartup = no      # yes / no
retainStateOverRestart = yes    # yes / no

[control]
longPressDurationMS = 800       # time in milliseconds to toggle

[nightmode]
refreshScreenPages = 4  		#force refresh every X pages
```
`invertActiveOnStartup` determines whether nightmode is active after booting. 
`retainStateOverRestart` determines whether the state should be kept over a restart.
`refreshScreenPages` sets the number of pages between screen refreshs when nightmode is active.
If you want to disable this feature, set it to 0.

Installation
------------
In the installer/uninstaller directories, two `KoboRoot.tgz` files are provided for automatic installation and removal.
Simply copy one of them into the .kobo folder on your device, safely remove, and wait for the reboot to finish.

How it works
------------
This hack works by intercepting and modifying screen-update requests on-the-fly from the main reader application. 
This is accomplished by interposing the `ioctl()` function using LD_PRELOAD.
Therefore `/etc/init.d/rcS` has to be modified to start `nickel`, the main app with the `screenInv.so` dynamic library.
For the Kobo Aura, the inverting has to be done in software due to a kernel bug. By interposing `mmap()`, memory accesses to framebuffer can be redirected into a virtual buffer, from which the inverted data is pushed to the screen.

FAQ
----
+ I have updated my firmware and it stopped working!
  + Simply install the mod again, if it still doesn't work post in the thread.
+ Will this drain my battery?
  + The effect on battery life should be negligible
+ I can see more ghosting when the mod is activated!
  + This is normal, as eInk screens are optimized for black-on-white mode. Change the refresh rate if it annoys you.

Compile for yourself
--------------------
All you need can be found in Kobo's [Kobo-Reader](https://github.com/kobolabs/Kobo-Reader) repository:
+ An linux tarball for the imx507 platform
+ The gcc-linaro-arm-linux-gnueabihf toolchain to compile

Credits
-------
+ [KevinShort](http://www.mobileread.com/forums/member.php?u=154832), for the idea and a first proof of concept
+ [GitHub contributers](https://github.com/dbeinder/kobo-nightmode/graphs/contributors)
+ Nicolas Devillard and others, for the [iniParser](http://github.com/ndevilla/iniparser) library

