# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# mod_aa_h323_tr -- H.323 trunk to Avaya PBXs (FreeSWITCH endpoint module)
#
# Standalone Makefile for building this module OUTSIDE the full FreeSWITCH
# source tree, against an already-installed FreeSWITCH (headers + pkg-config
# file). This is the normal
# way to build a third-party module once FreeSWITCH itself is installed from
# a package (apt/yum) rather than built from source.
#
# Requirements on the build machine:
#   - freeswitch-dev / freeswitch-devel (or a from-source install) providing
#     `pkg-config --cflags --libs freeswitch`
#   - a C compiler (gcc or clang)
#
# Build:
#   make
# Install (copies mod_aa_h323_tr.so into FreeSWITCH's mod dir, from pkg-config):
#   sudo make install
# Then add mod_aa_h323_tr to modules.conf.xml (see README.md) and either
# restart FreeSWITCH or run `load mod_aa_h323_tr` from fs_cli.

CC       ?= gcc
PKGCONFIG ?= pkg-config

FS_CFLAGS  := $(shell $(PKGCONFIG) --cflags freeswitch 2>/dev/null)
FS_LIBS    := $(shell $(PKGCONFIG) --libs freeswitch 2>/dev/null)
FS_MODDIR  := $(shell $(PKGCONFIG) --variable=modulesdir freeswitch 2>/dev/null)
FS_CONFDIR := $(shell $(PKGCONFIG) --variable=confdir freeswitch 2>/dev/null)



# Many FreeSWITCH packages (in particular the SignalWire .deb/.rpm repos
# commonly used on Debian/Ubuntu/CentOS) don't ship a freeswitch.pc at all,
# even though the headers/module dir/config dir are all present in their
# usual, well-known locations. Rather than fail outright when pkg-config
# comes back empty, probe those locations directly.
#
# First, a from-source install often DOES generate a real freeswitch.pc,
# just under $prefix/lib/pkgconfig - a directory pkg-config doesn't search
# by default. Try pkg-config again with those added to its search path
# before falling back to guessing paths ourselves; a real .pc file is more
# trustworthy than any guess.
empty :=
space := $(empty) $(empty)

ifeq ($(strip $(FS_CFLAGS))$(strip $(FS_LIBS)),)
FS_PC_SEARCH := $(dir $(wildcard \
	/usr/local/freeswitch/lib/pkgconfig/freeswitch.pc \
	/opt/freeswitch/lib/pkgconfig/freeswitch.pc \
	/usr/local/lib/pkgconfig/freeswitch.pc))
ifneq ($(strip $(FS_PC_SEARCH)),)
FS_PC_SEARCH_COLON := $(subst $(space),:,$(strip $(FS_PC_SEARCH)))
FS_CFLAGS  := $(shell PKG_CONFIG_PATH="$(FS_PC_SEARCH_COLON)" $(PKGCONFIG) --cflags freeswitch 2>/dev/null)
FS_LIBS    := $(shell PKG_CONFIG_PATH="$(FS_PC_SEARCH_COLON)" $(PKGCONFIG) --libs freeswitch 2>/dev/null)
FS_MODDIR  := $(shell PKG_CONFIG_PATH="$(FS_PC_SEARCH_COLON)" $(PKGCONFIG) --variable=modulesdir freeswitch 2>/dev/null)
FS_CONFDIR := $(shell PKG_CONFIG_PATH="$(FS_PC_SEARCH_COLON)" $(PKGCONFIG) --variable=confdir freeswitch 2>/dev/null)
ifneq ($(strip $(FS_CFLAGS)),)
$(info Found freeswitch.pc outside pkg-config's default search path, under $(FS_PC_SEARCH) - using it.)
endif
endif
endif

# Still nothing - guess well-known header locations, but check for switch.h
# ITSELF (not just that some "include" directory exists - a from-source
# build's $prefix/include can exist yet be empty, with headers actually one
# level deeper at $prefix/include/freeswitch, mirroring how a packaged
# build lays out /usr/include/freeswitch).
ifeq ($(strip $(FS_CFLAGS)),)
FS_SWITCH_H := $(firstword $(wildcard \
	/usr/include/freeswitch/switch.h \
	/usr/local/include/freeswitch/switch.h \
	/usr/local/freeswitch/include/freeswitch/switch.h \
	/usr/local/freeswitch/include/switch.h \
	/opt/freeswitch/include/freeswitch/switch.h \
	/opt/freeswitch/include/switch.h))
ifneq ($(strip $(FS_SWITCH_H)),)
FS_CFLAGS := -I$(patsubst %/,%,$(dir $(FS_SWITCH_H)))
$(info pkg-config has no "freeswitch.pc" - found headers directly at $(FS_CFLAGS:-I%=%) instead.)
endif
endif

# Last resort: none of the well-known layouts matched - actually search the
# filesystem for switch.h rather than keep guessing directory names. Bounded
# to a handful of common install roots so it stays fast.
ifeq ($(strip $(FS_CFLAGS)),)
FS_SWITCH_H := $(firstword $(shell find /usr/local /usr/include /opt -maxdepth 6 -name switch.h 2>/dev/null))
ifneq ($(strip $(FS_SWITCH_H)),)
FS_CFLAGS := -I$(patsubst %/,%,$(dir $(FS_SWITCH_H)))
$(info Found switch.h by searching the filesystem, at $(FS_SWITCH_H).)
endif
endif

ifeq ($(strip $(FS_LIBS)),)
FS_LIBDIR := $(firstword $(dir $(wildcard \
	/usr/lib/libfreeswitch.so /usr/lib64/libfreeswitch.so \
	/usr/lib/x86_64-linux-gnu/libfreeswitch.so /usr/lib/aarch64-linux-gnu/libfreeswitch.so \
	/usr/local/freeswitch/lib/libfreeswitch.so /usr/local/lib/libfreeswitch.so)))
ifneq ($(strip $(FS_LIBDIR)),)
FS_LIBS := -L$(FS_LIBDIR) -lfreeswitch
else
FS_LIBS := -lfreeswitch
endif
endif

ifeq ($(strip $(FS_MODDIR)),)
FS_MODDIR := $(firstword $(wildcard \
	/usr/lib/freeswitch/mod /usr/lib64/freeswitch/mod \
	/usr/lib/x86_64-linux-gnu/freeswitch/mod /usr/lib/aarch64-linux-gnu/freeswitch/mod \
	/usr/local/freeswitch/mod))
endif

ifeq ($(strip $(FS_CONFDIR)),)
FS_CONFDIR := $(firstword $(wildcard /etc/freeswitch /usr/local/freeswitch/conf))
endif

ifeq ($(strip $(FS_CFLAGS)),)
$(warning Could not find FreeSWITCH's headers anywhere (no freeswitch.pc, and none of the usual paths exist).)
$(warning Set FS_CFLAGS by hand, e.g.: make FS_CFLAGS="-I/path/to/freeswitch/include")
endif

CORE    := ..
CFLAGS  ?= -O2 -g -Wall -fPIC -Wno-unused-parameter
CFLAGS  += $(FS_CFLAGS) -I$(CORE)
LDFLAGS += -shared
LIBS    := $(FS_LIBS) -lpthread -lm

TARGET  := mod_aa_h323_tr.so
# Ядро (чистый C, без FreeSWITCH): сообщения H.225/H.245, config.json. Новые файлы ядра добавлять сюда.
CORE_SRC := aa_tr_msg.c aa_tr_config.c aa_tr_engine.c aa_json.c aa_smap.c
SOURCES := mod_aa_h323_tr.c $(addprefix $(CORE)/,$(CORE_SRC))
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean install install-conf setup print-confdir print-moddir

all: $(TARGET)

# Сборка + установка + настройка одним вызовом (см. ../install.sh). Запускать от root.
setup:
	@../install.sh

print-confdir:
	@echo $(FS_CONFDIR)

print-moddir:
	@echo $(FS_MODDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

# Номер сборки: хэш всех исходников и заголовков. Меняется при любой правке, `aa_h323_tr status` показывает его.
BUILD_HASH := $(shell cat $(SOURCES) $(wildcard $(CORE)/*.h) 2>/dev/null | md5sum | cut -c1-8)
.buildstamp: FORCE
	@echo '$(BUILD_HASH)' | cmp -s - $@ || echo '$(BUILD_HASH)' > $@
FORCE:

%.o: %.c $(wildcard $(CORE)/*.h) .buildstamp
	$(CC) $(CFLAGS) -DAA_TR_BUILD=\"$(BUILD_HASH)\" -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET) .buildstamp

# Копирует модуль в каталог модулей FreeSWITCH.
install: $(TARGET)
ifeq ($(strip $(FS_MODDIR)),)
	$(error Не удалось определить каталог модулей FreeSWITCH; установите вручную, например: install -m755 $(TARGET) /usr/lib/freeswitch/mod/)
endif
	install -d $(FS_MODDIR)
	install -m755 $(TARGET) $(FS_MODDIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(FS_MODDIR)"

# Копирует XML-конфиг и диалплан, только если их ещё нет (существующие не трогает).
install-conf:
ifeq ($(strip $(FS_CONFDIR)),)
	$(error Не удалось определить каталог конфигурации FreeSWITCH; скопируйте conf/aa_h323_tr.conf.xml вручную.)
endif
	install -d $(FS_CONFDIR)/autoload_configs
	test -f $(FS_CONFDIR)/autoload_configs/aa_h323_tr.conf.xml || \
	  install -m644 conf/aa_h323_tr.conf.xml $(FS_CONFDIR)/autoload_configs/aa_h323_tr.conf.xml
	install -d $(FS_CONFDIR)/dialplan
	test -f $(FS_CONFDIR)/dialplan/aa_h323_tr.xml || \
	  install -m644 conf/dialplan-aa_h323_tr.xml $(FS_CONFDIR)/dialplan/aa_h323_tr.xml
	@echo "Installed sample config (only where nothing existed yet) - review and edit before use."
