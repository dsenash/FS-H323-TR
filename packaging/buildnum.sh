#!/bin/sh
# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Номер сборки. Счётчик хранится в .buildnum (корень проекта): "<хеш исходников> <номер>".
#   buildnum.sh            - если исходники изменились с прошлой сборки, увеличить номер; напечатать номер
#   buildnum.sh get        - только напечатать текущий номер (0, если сборок не было)
#   buildnum.sh subst IN OUT - записать IN в OUT, заменив "0.0.X" на "0.0.<номер>"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
F="$ROOT/.buildnum"

cur() { [ -f "$F" ] && awk '{print $2+0}' "$F" || echo 0; }

case "${1:-}" in
get) cur ;;
subst)
	[ -n "$2" ] && [ -n "$3" ] || { echo "usage: $0 subst IN OUT" >&2; exit 2; }
	sed "s/0\.0\.X/0.0.$(cur)/g" "$2" > "$3"
	;;
*)
	H="$(cd "$ROOT" && cat aa_*.c aa_*.h fs/mod_aa_h323_tr.c 2>/dev/null | md5sum | cut -c1-8)"
	OLD_H=""; [ -f "$F" ] && OLD_H="$(awk '{print $1}' "$F")"
	N="$(cur)"
	if [ "$H" != "$OLD_H" ]; then
		N=$((N + 1))
		echo "$H $N" > "$F"
	fi
	echo "$N"
	;;
esac
