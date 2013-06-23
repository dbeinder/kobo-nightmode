kobo-nightmode
==============

A tiny hack to read white-on-black on Kobo ebook readers. Works only on the eInk series:
+ Kobo Glo
+ Kobo Touch
+ Kobo Aura HD
+ Kobo Mini

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

A sample script to toggle is provided in `integration/nightmode.sh`.
As the Kobo Mini has no physical buttons, the fifo-interface is currently the only way to control nightmode and thus, 
relies on external tools such as:
+ [Kobo Tweaks](http://www.mobileread.com/forums/showthread.php?t=206180), maintained by ah- (currently not available for 2.6+)
+ [KoboLauncher](http://www.mobileread.com/forums/showthread.php?t=201632), maintained by sergeyvl12

Installation
------------
In the installer/uninstaller directories, two `KoboRoot.tgz` files are provided for automatic installation and removal.
Simply copy one of them into the .kobo folder on your device, safely remove, and wait for the reboot to finish.

How it works
------------
This hack works by intercepting and modifying screen-update requests on-the-fly from the main reader application. 
This is accomplished by interposing the `ioctl()` function using LD_PRELOAD.
Therefore `/etc/init.d/rcS` has to be modified to start `nickel`, the main app with the `screenInv.so` dynamic library.

Compile for yourself
--------------------
All you need can be found in Kobo's [Kobo-Reader](https://github.com/kobolabs/Kobo-Reader) repository:
+ An linux tarball for the imx507 platform
+ The gcc-linaro-arm-linux-gnueabihf toolchain to compile

