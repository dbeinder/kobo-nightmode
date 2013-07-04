all:
	$(MAKE) -C src
	$(MAKE) -C installer
	$(MAKE) -C uninstaller
	rm -f kobo-nightmode_build*.zip
	zip -qr kobo-nightmode_build8.zip installer/KoboRoot.tgz uninstaller/KoboRoot.tgz extra
