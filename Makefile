all:
	$(MAKE) -C src
	$(MAKE) -C installer
	$(MAKE) -C uninstaller
	rm -f kobo-nightmode_build*.zip
	zip -qr kobo-nightmode_build6.zip installer uninstaller src integration Makefile
