#!/bin/sh

FW1_ORIG="/usr/local/Kobo/nickel -qws \&"
FW1_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws \&"

FW2_ORIG="/usr/local/Kobo/nickel -qws -skipFontLoad \&"
FW2_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws -skipFontLoad \&"

FW3_ORIG="/usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"
FW3_NEW="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -platform kobo -skipFontLoad \&"

sed -i "s;^$FW1_ORIG;$FW1_NEW;" /etc/init.d/rcS
sed -i "s;^$FW2_ORIG;$FW2_NEW;" /etc/init.d/rcS
sed -i "s;^$FW3_ORIG;$FW3_NEW;" /etc/init.d/rcS

#cleanup
rm /etc/udev/rules.d/98-autorunHook.rules
rm /root/autorun.sh
sync
reboot
