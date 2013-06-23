#!/bin/sh

OLD_START="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws \&"
NEW_START="/usr/local/Kobo/nickel -qws \&"

NEWFW_OLD_START="LD_PRELOAD=/root/screenInv.so /usr/local/Kobo/nickel -qws -skipFontLoad \&"
NEWFW_NEW_START="/usr/local/Kobo/nickel -qws -skipFontLoad \&"

sed -i "s;^$OLD_START;$NEW_START;" /etc/init.d/rcS
sed -i "s;^$NEWFW_OLD_START;$NEWFW_NEW_START;" /etc/init.d/rcS

rm /root/screenInv.so

#cleanup
rm /etc/udev/rules.d/98-autorunHook.rules
rm /root/autorun.sh
sync
reboot
