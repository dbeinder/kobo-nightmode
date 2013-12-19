debug: all
all:
	$(MAKE) $(MAKECMDGOALS) -C src
	$(MAKE) -C installer
	$(MAKE) -C uninstaller
	rm -f kobo-nightmode_build*.zip
	zip -qr kobo-nightmode_build10.zip installer/KoboRoot.tgz uninstaller/KoboRoot.tgz extra
