# wmpmeta: WMP9 CD metadata from MusicBrainz, for Windows 2000.
# Cross-built with MinGW-w64 (i686).
#
#   make        build/wmpmeta.exe
#   make check  verify every import exists in Windows 2000's system DLLs
#               (needs real Win2k SP4 DLLs: make check W2K_SYSDIR=path)
#   make dist   dist/wmpmeta-<version>.zip (portable) and
#               dist/wmpmeta-<version>-setup.exe (needs makensis)

CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip
PYTHON  ?= python3
W2K_SYSDIR ?= ../../filezilla-win2k/sysdlls/1
# NSIS for the setup.exe. Under Wine, paths need a drive letter:
#   make dist MAKENSIS="wine64 /path/to/nsis-3.10/makensis.exe" NSIS_DRIVE=Z:
MAKENSIS   ?= makensis
NSIS_DRIVE ?=
VERSION := $(shell sed -n 's/^\#define WM_VERSION "\(.*\)"/\1/p' src/wmpmeta.h)

BUILD   := build
TARGET  := $(BUILD)/wmpmeta.exe
MBED    := third_party/mbedtls

APP_SRCS  := src/main.c src/mb.c src/fetch.c src/guide.c src/radio.c src/wayback.c src/album.c src/skins.c src/tls.c src/util.c src/compat.c \
             third_party/cjson/cJSON.c
MBED_SRCS := $(filter-out $(MBED)/library/net_sockets.c,$(wildcard $(MBED)/library/*.c))
OBJS      := $(patsubst %.c,$(BUILD)/obj/%.o,$(APP_SRCS) $(MBED_SRCS)) $(BUILD)/obj/certs.o

# _WIN32_WINNT=0x0500 keeps XP+ declarations out of the headers.
# _USE_32BIT_TIME_T maps time()/gmtime() to the plain msvcrt.dll exports;
# Win2k's msvcrt has no _time64 family.
CPPFLAGS := -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 -D_USE_32BIT_TIME_T \
            -D__USE_MINGW_ANSI_STDIO=1 \
            -DMBEDTLS_CONFIG_FILE='"mbedtls_config_w2k.h"' \
            -Isrc -Ithird_party/cjson -I$(MBED)/include -I$(MBED)/library
CFLAGS   := -std=gnu99 -O2 -march=i686 -mtune=generic -fno-strict-aliasing \
            -ffunction-sections -fdata-sections -Wall
# Subsystem/OS version 5.0: the Win2k loader rejects newer stamps.
LDFLAGS  := -static -static-libgcc -mwindows \
            -Wl,--major-os-version,5 -Wl,--minor-os-version,0 \
            -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,0 \
            -Wl,--gc-sections -Wl,--disable-dynamicbase -Wl,--disable-nxcompat
LIBS     := -lws2_32 -ladvapi32

all: $(TARGET)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(if $(filter third_party/%,$<),-w,-Wextra -Wno-unused-parameter) -c $< -o $@

$(patsubst %.c,$(BUILD)/obj/%.o,$(filter src/%,$(APP_SRCS))): src/wmpmeta.h src/tlsnet.h src/mbedtls_config_w2k.h src/guide_img.h src/skins_data.h
$(patsubst %.c,$(BUILD)/obj/%.o,$(MBED_SRCS)): src/mbedtls_config_w2k.h

$(BUILD)/certs.c: certs/roots.pem tools/embed_certs.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/embed_certs.py $< $@

$(BUILD)/obj/certs.o: $(BUILD)/certs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@$(STRIP) $@

check: $(TARGET)
	$(PYTHON) tools/check_imports.py $(TARGET) $(W2K_SYSDIR)

# Text files get CRLF line endings for Notepad on Windows 2000.
dist: $(TARGET)
	rm -rf $(BUILD)/dist && mkdir -p $(BUILD)/dist/wmpmeta dist
	cp $(TARGET) $(BUILD)/dist/wmpmeta/
	for f in README.md:README.txt LICENSE:LICENSE.txt THIRD_PARTY_NOTICES.txt:THIRD_PARTY_NOTICES.txt \
	         scripts/install.cmd:install.cmd scripts/uninstall.cmd:uninstall.cmd; do \
	    $(PYTHON) -c "import sys; d=open(sys.argv[1]).read(); open(sys.argv[2],'w',newline='\r\n').write(d)" \
	        $${f%%:*} $(BUILD)/dist/wmpmeta/$${f##*:}; done
	cp third_party/mbedtls/LICENSE $(BUILD)/dist/wmpmeta/LICENSE-mbedtls.txt
	cp third_party/cjson/LICENSE $(BUILD)/dist/wmpmeta/LICENSE-cjson.txt
	rm -f dist/wmpmeta-$(VERSION).zip
	cd $(BUILD)/dist && zip -qr ../../dist/wmpmeta-$(VERSION).zip wmpmeta
	@echo "  dist/wmpmeta-$(VERSION).zip"
	WINEDEBUG=-all $(MAKENSIS) -V2 -DVERSION=$(VERSION) -DSRCDIR=$(NSIS_DRIVE)$(abspath $(BUILD)/dist/wmpmeta) \
	    -DOUTFILE=$(NSIS_DRIVE)$(abspath dist/wmpmeta-$(VERSION)-setup.exe) $(NSIS_DRIVE)$(abspath installer/wmpmeta.nsi)
	@echo "  dist/wmpmeta-$(VERSION)-setup.exe"

clean:
	rm -rf $(BUILD) dist

.PHONY: all check dist clean
