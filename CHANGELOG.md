CHANGELOG
=========

build 13
--------
+ support for kobo fw 3.3.0
  + stopping EVIOCGRAB ioctl to allow access to the buttons
  + updated installer due to new nickel startup line

build 12
--------
+ kobo fw 3.2.0 changes the screen orientation, this is now intercepted
+ optimized copy routine for virtual framebuffer
+ added gcc optimization flag

build 11
--------
+ fixed segfault (thanks to KenMacD)
+ added some debug code for forced refresh

build 10
--------
+ supports the new Aura 6' (codename: pheonix)
  + added software inversion mode due to kernel bug

build 9 
-------
+ bugfix: prevent overriding of configfile

build 8
-------
+ user can now configure refresh rate in nightmode
+ cleanup

build 7
-------
+ added configuration
+ shortened standard long-press duration to 0.8s
+ uninstaller now completely removes the hack

build 6
-------
+ moved to hard-float ABI, neccessary for fw 2.6.1
+ changed (un)installer code to work with fw 2.6.1

build 5
-------
+ moved application to /root to prevent conflicts when attached to a pc
+ added support to toggle with physical buttons
