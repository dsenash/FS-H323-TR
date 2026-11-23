#!/bin/sh
# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Сборка deb-пакета mod-aa-h323-tr из готового mod_aa_h323_tr.so.
#   packaging/deb/build.sh /путь/к/mod_aa_h323_tr.so
# Переменные: VER (по умолчанию 1.0.0), ARCH (по умолчанию amd64), LIBC_MIN (иначе вычисляется по .so).
set -e
SP="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$SP/../.." && pwd)"
SO="${1:?usage: build.sh path/to/mod_aa_h323_tr.so}"
VER=${VER:-1.0.0}
ARCH=${ARCH:-amd64}
[ -f "$SO" ] || { echo "нет файла $SO" >&2; exit 1; }
[ -f "$SRC/LICENSE" ] || { echo "нет $SRC/LICENSE" >&2; exit 1; }

# минимальная версия libc по символам .so
if [ -z "${LIBC_MIN:-}" ]; then
    LIBC_MIN="$(objdump -T "$SO" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -n1)"
    [ -n "$LIBC_MIN" ] || LIBC_MIN=2.17
fi

P=${TMPDIR:-/tmp}/mod-aa-h323-tr-pkg; rm -rf "$P"
mkdir -p "$P/DEBIAN" "$P/usr/share/mod-aa-h323-tr" "$P/usr/sbin" "$P/usr/share/doc/mod-aa-h323-tr"
install -m755 "$SO" "$P/usr/share/mod-aa-h323-tr/mod_aa_h323_tr.so"
install -m755 "$SP/mod-aa-h323-tr-setup" "$P/usr/sbin/mod-aa-h323-tr-setup"
install -m644 "$SRC/packaging/jsonkey.sh" "$P/usr/share/mod-aa-h323-tr/jsonkey.sh"
cp -a "$SRC/fs/conf" "$SRC/config" "$P/usr/share/mod-aa-h323-tr/"
"$SRC/packaging/buildnum.sh" subst "$SRC/config/aa_h323_tr.example.json" "$P/usr/share/mod-aa-h323-tr/config/aa_h323_tr.example.json"
cp -a "$SRC/README.md" "$SRC/README.ru.md" "$SRC/docs" "$P/usr/share/doc/mod-aa-h323-tr/"
rm -f "$P/usr/share/doc/mod-aa-h323-tr/docs/PLAN.md"
install -m644 "$SRC/LICENSE" "$P/usr/share/doc/mod-aa-h323-tr/LICENSE"
cat > "$P/usr/share/doc/mod-aa-h323-tr/copyright" <<EOC
Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
Copyright (c) 2026: Сенашенко Дмитрий.
Разработано в Российской Федерации.
Лицензия: MIT (полный текст - /usr/share/doc/mod-aa-h323-tr/LICENSE).
EOC
find "$P/usr/share" -type d -exec chmod 755 {} +; find "$P/usr/share" -type f -exec chmod 644 {} +
chmod 755 "$P/usr/share/mod-aa-h323-tr/mod_aa_h323_tr.so"
for s in preinst postinst prerm postrm; do
    { echo '#!/bin/sh'; echo 'set -e'; cat "$SP/$s.body"; } > "$P/DEBIAN/$s"
    chmod 755 "$P/DEBIAN/$s"
done
SIZE=$(du -sk "$P" --exclude=DEBIAN | cut -f1)
cat > "$P/DEBIAN/control" <<EOC
Package: mod-aa-h323-tr
Version: $VER
Section: comm
Priority: optional
Architecture: $ARCH
Installed-Size: $SIZE
Depends: libc6 (>= $LIBC_MIN), gawk | awk
Recommends: iproute2
Maintainer: Dmitry Senashenko <dsenash@gmail.com>
Description: FreeSWITCH module: H.323 trunk to Avaya Communication Manager
 mod_aa_h323_tr is a FreeSWITCH module written in pure C that connects
 FreeSWITCH to Avaya Communication Manager as an H.323 trunk. The package
 puts the module and configuration into the FreeSWITCH directories
 (detected automatically; see mod-aa-h323-tr-setup).
EOC
OUT="$SRC/mod-aa-h323-tr_${VER}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$P" "$OUT"
echo "собран $OUT (libc6 >= $LIBC_MIN)"
