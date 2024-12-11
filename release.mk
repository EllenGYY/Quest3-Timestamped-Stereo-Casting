# This makefile provides recipes to build a "portable" version of scrcpy for
# Windows.
#
# Here, "portable" means that the client and server binaries are expected to be
# anywhere, but in the same directory, instead of well-defined separate
# locations (e.g. /usr/bin/scrcpy and /usr/share/scrcpy/scrcpy-server).
#
# In particular, this implies to change the location from where the client push
# the server to the device.

.PHONY: default clean \
	prepare-deps \
	build-win64 \
	dist-win64 \
	zip-win64 \
	release

GRADLE ?= ./gradlew

WIN64_BUILD_DIR := build-win64

VERSION ?= $(shell git describe --tags --exclude='*install-release' --always)

DIST := dist
WIN64_TARGET_DIR := scrcpy-win64-$(VERSION)
WIN64_TARGET := $(WIN64_TARGET_DIR).zip

RELEASE_DIR := release-$(VERSION)

release: clean zip-win64
	mkdir -p "$(RELEASE_DIR)"
	cp scrcpy-server-v2.7 \
		"$(RELEASE_DIR)/scrcpy-server-$(VERSION)"
	cp "$(DIST)/$(WIN64_TARGET)" "$(RELEASE_DIR)"
	cd "$(RELEASE_DIR)" && \
		sha256sum "scrcpy-server-$(VERSION)" \
			"scrcpy-win64-$(VERSION).zip" > SHA256SUMS.txt
	@echo "Release generated in $(RELEASE_DIR)/"

clean:
	$(GRADLE) clean
	rm -rf "$(DIST)" "$(WIN64_BUILD_DIR)"


prepare-deps-win64:
	@app/deps/adb.sh win64
	@app/deps/sdl.sh win64
	@app/deps/ffmpeg.sh win64
	@app/deps/libusb.sh win64
	@app/deps/opencv.sh win64

build-win64: prepare-deps-win64
	rm -rf "$(WIN64_BUILD_DIR)"
	mkdir -p "$(WIN64_BUILD_DIR)/local"
	meson setup "$(WIN64_BUILD_DIR)" \
		--pkg-config-path="app/deps/work/install/win64/lib/pkgconfig" \
		-Dc_args="-I$(PWD)/app/deps/work/install/win64/include" \
		-Dc_link_args="-L$(PWD)/app/deps/work/install/win64/lib" \
		--cross-file=cross_win64.txt \
		--buildtype=release --strip -Db_lto=true \
		-Dprebuilt_server=scrcpy-server-v2.7 \
		-Dportable=true
	ninja -C "$(WIN64_BUILD_DIR)"

dist-win64: build-win64
	mkdir -p "$(DIST)/$(WIN64_TARGET_DIR)"
	cp scrcpy-server-v2.7 "$(DIST)/$(WIN64_TARGET_DIR)/scrcpy-server"
	cp "$(WIN64_BUILD_DIR)"/app/scrcpy.exe "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/data/scrcpy-console.bat "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/data/scrcpy-noconsole.vbs "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/data/icon.png "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/data/open_a_terminal_here.bat "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/deps/work/install/win64/bin/*.dll "$(DIST)/$(WIN64_TARGET_DIR)/"
	cp app/deps/work/install/win64/bin/adb.exe "$(DIST)/$(WIN64_TARGET_DIR)/"

zip-win64: dist-win64
	cd "$(DIST)"; \
		zip -r "$(WIN64_TARGET)" "$(WIN64_TARGET_DIR)"
