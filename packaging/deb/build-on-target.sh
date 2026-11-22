#!/bin/sh
# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Сборка deb-пакета mod-aa-h323-tr НА ЦЕЛЕВОЙ СИСТЕМЕ (с установленным FreeSWITCH) из исходников проекта:
#   исходники -> make (fs/) -> mod_aa_h323_tr.so -> mod-aa-h323-tr_<VER>_<ARCH>.deb
#
#   cd /opt/mod_aa_h323_tr
#   packaging/deb/build-on-target.sh
#   sudo apt install ./mod-aa-h323-tr_1.0.0_amd64.deb
#
# Переменные окружения (все необязательные):
#   FS_SRC=/usr/src/freeswitch   каталог исходников FreeSWITCH, если пакет freeswitch-dev не установлен
#   FS_CFLAGS=... FS_LIBS=...    флаги компиляции/линковки вручную (приоритетнее автоопределения)
#   VER=1.0.0  ARCH=<dpkg arch>  версия и архитектура пакета
#   NOBUILD=1                    не вызывать make, взять уже собранный fs/mod_aa_h323_tr.so
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=""
for c in "$HERE/../.." "$HERE" "$PWD"; do
    if [ -f "$c/fs/Makefile" ]; then SRC="$(cd "$c" && pwd)"; break; fi
done
[ -n "$SRC" ] || { echo "ОШИБКА: не найден корень проекта (fs/Makefile). Запустите скрипт из каталога проекта." >&2; exit 1; }
SP="$SRC/packaging/deb"
cd "$SRC"
die() { echo "ОШИБКА: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# --- 1. инструменты ---------------------------------------------------------
miss=""
for t in gcc make dpkg-deb objdump tr grep sed; do command -v $t >/dev/null 2>&1 || miss="$miss $t"; done
[ -z "$miss" ] || die "не хватает:$miss
    Установите: sudo apt install build-essential binutils dpkg-dev pkg-config"
ARCH=${ARCH:-$(dpkg --print-architecture)}
export ARCH VER=${VER:-1.0.0}

# --- 2. заголовки и библиотека FreeSWITCH ---------------------------------
if [ "${NOBUILD:-0}" != 1 ]; then
if [ -z "${FS_CFLAGS:-}" ]; then
    if pkg-config --exists freeswitch 2>/dev/null && \
       [ -f "$(pkg-config --variable=includedir freeswitch 2>/dev/null)/freeswitch/switch.h" -o -f "$(pkg-config --variable=includedir freeswitch 2>/dev/null)/switch.h" ]; then
        info "FreeSWITCH: freeswitch.pc"
    else
        FS_SRC=${FS_SRC:-/usr/src/freeswitch}
        if [ -f /usr/include/freeswitch/switch.h ]; then
            info "FreeSWITCH: установленные заголовки /usr/include/freeswitch"
            FS_CFLAGS="-I/usr/include/freeswitch"
        elif [ -f "$FS_SRC/src/include/switch.h" ]; then
            info "FreeSWITCH: заголовки из исходников $FS_SRC"
            inc="-I$FS_SRC/src/include"
            for d in libs/libteletone/src libs/apr/include libs/apr-util/include libs/srtp/include \
                     libs/spandsp/src libs/sofia-sip/libsofia-sip-ua/su libs/stfu libs/libzrtp/include; do
                [ -d "$FS_SRC/$d" ] && inc="$inc -I$FS_SRC/$d"
            done
            FS_CFLAGS="$inc"
        else
            die "заголовки FreeSWITCH не найдены. Установите freeswitch-dev, либо задайте FS_SRC=<каталог исходников>,
    либо FS_CFLAGS=\"-I...\" FS_LIBS=\"-L... -lfreeswitch\" вручную."
        fi
    fi
fi
[ -n "${FS_CFLAGS:-}" ] && export FS_CFLAGS
[ -n "${FS_LIBS:-}" ] && export FS_LIBS
info "FS_CFLAGS=${FS_CFLAGS:-<pkg-config>}"
info "FS_LIBS=${FS_LIBS:-<pkg-config>}"
info "make -C fs clean all"
make -C fs clean >/dev/null 2>&1 || true
make -C fs ${FS_CFLAGS:+FS_CFLAGS="$FS_CFLAGS"} ${FS_LIBS:+FS_LIBS="$FS_LIBS"} all
fi

SO="$SRC/fs/mod_aa_h323_tr.so"
[ -f "$SO" ] || die "нет $SO - сборка не удалась"

# --- 3. упаковка -----------------------------------------------------------
info "сборка deb"
sh "$SP/build.sh" "$SO"
DEB="$SRC/mod-aa-h323-tr_${VER}_${ARCH}.deb"
echo
echo "Готово: $DEB"
dpkg-deb -I "$DEB" | grep -E 'Package|Version|Architecture|Depends' || true
echo "Установка:  sudo apt install $DEB"
echo "Проверка:   dpkg -L mod-aa-h323-tr ; ls /usr/share/mod-aa-h323-tr"
