#!/usr/bin/env bash
# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

#
# mod_aa_h323_tr -- сборка + установка + настройка одним запуском.
#
# По шагам: сборка (make -C fs), установка модуля, пример XML-конфига (только если ещё нет),
# aa_h323_tr.json (IP этого сервера и порт приёма H.225 - спрашивает, по умолчанию берёт уже
# настроенное), подключение mod_aa_h323_tr в modules.conf.xml, проверка dialplan, перезагрузка
# модуля в работающем FreeSWITCH. Безопасно запускать повторно: транки и правила номеров в
# aa_h323_tr.json сохраняются - правятся только local.ip и local.h225_port.
#
# Транки (IP АТС, порт, кодек, число линий, правила номеров) правятся вручную в aa_h323_tr.json.
#
# Использование: ./install.sh   (от root или через sudo, из этого каталога)
#   AA_TR_LOCAL_IP=1.2.3.4 AA_TR_PORT=1722 ./install.sh   - без вопросов (для автоматизации)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/fs"

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }
ok()   { printf '    %s\n' "$1"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$1"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "запустите от root (или через sudo) - скрипт ставит файлы в каталоги FreeSWITCH."
command -v pkg-config >/dev/null 2>&1 || die "pkg-config не найден (apt install pkg-config)."
command -v awk >/dev/null 2>&1 || die "awk не найден."
. "$SCRIPT_DIR/packaging/jsonkey.sh"

DEF_PORT=1722

# ---------------------------------------------------------------------
step "1/7  Сборка mod_aa_h323_tr.so"
# ---------------------------------------------------------------------
make
ok "сборка OK"

# ---------------------------------------------------------------------
step "2/7  Установка модуля"
# ---------------------------------------------------------------------
make install

# ---------------------------------------------------------------------
step "3/7  Пример XML-конфига и dialplan (только если их ещё нет)"
# ---------------------------------------------------------------------
make install-conf

# Каталог конфигурации спрашиваем у Makefile (он уже умеет искать freeswitch.pc и
# запасные пути, например /usr/local/freeswitch).
FS_CONF_DIR="$(make print-confdir 2>/dev/null | tail -n1)"
if [ -z "$FS_CONF_DIR" ]; then
	FS_CONF_DIR="/etc/freeswitch"
	warn "не удалось определить каталог конфигурации FreeSWITCH - беру $FS_CONF_DIR"
fi
CONFIG_JSON="$FS_CONF_DIR/aa_h323_tr/aa_h323_tr.json"

# ---------------------------------------------------------------------
step "4/7  aa_h323_tr.json (local.ip - IP ЭТОГО сервера; local.h225_port - порт приёма, необязателен)"
# ---------------------------------------------------------------------
mkdir -p "$(dirname "$CONFIG_JSON")"
if [ ! -f "$CONFIG_JSON" ]; then
	cp ../build/aa_h323_tr.example.json "$CONFIG_JSON"
	ok "создан $CONFIG_JSON из примера (версия 0.0.<номер сборки>)"
else
	ok "найден $CONFIG_JSON - транки и правила номеров сохраняются, правятся только local.ip и local.h225_port"
fi

# --- IP этого сервера ---
CURRENT_IP="$(local_ip_get "$CONFIG_JSON")"
[ -n "$CURRENT_IP" ] || CURRENT_IP="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") print $(i+1)}')"
[ -n "$CURRENT_IP" ] || CURRENT_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

ok "local.ip - это IP САМОГО ЭТОГО СЕРВЕРА (FreeSWITCH). Модуль слушает на нём H.225 (TCP) и"
ok "сообщает его АТС как адрес для сигнализации и RTP. Это НЕ IP АТС - IP АТС задаётся в trunk[].ip."
if [ -n "${AA_TR_LOCAL_IP:-}" ]; then
	INPUT_IP="$AA_TR_LOCAL_IP"
elif [ -n "$CURRENT_IP" ]; then
	read -r -p "    Введите IP этого сервера ($CURRENT_IP): " INPUT_IP
	INPUT_IP="${INPUT_IP:-$CURRENT_IP}"
else
	read -r -p "    Введите IP этого сервера: " INPUT_IP
	[ -n "$INPUT_IP" ] || die "IP не введён и не удалось определить автоматически."
fi
if [ -z "${AA_TR_LOCAL_IP:-}" ] && command -v ip >/dev/null 2>&1; then
	while ! ip -4 addr show 2>/dev/null | grep -q "inet ${INPUT_IP}/"; do
		warn "$INPUT_IP не найден ни на одном интерфейсе этой машины ('ip -4 addr show')."
		warn "Если оставить как есть - bind сокета завершится ошибкой 'Cannot assign requested address'."
		read -r -p "    Другой IP этого сервера (Enter = оставить $INPUT_IP): " RETRY_IP
		[ -n "$RETRY_IP" ] || break
		INPUT_IP="$RETRY_IP"
	done
fi
local_ip_set "$CONFIG_JSON" "$INPUT_IP" || die "в $CONFIG_JSON не найден local.ip - впишите адрес вручную."
ok "local.ip = $INPUT_IP"

# --- порт приёма H.225 (необязателен) ---
CURRENT_PORT="$(local_port_get "$CONFIG_JSON")"
[ -n "$CURRENT_PORT" ] || CURRENT_PORT="$DEF_PORT"
ok "local.h225_port - порт, на котором модуль принимает вызовы от АТС (необязательный ключ, по умолчанию $DEF_PORT)."
ok "Порт 1720 обычно занят mod_aa_h323; на CM его надо указать в сигнальной группе как Far-end Listen Port."
if [ -n "${AA_TR_PORT:-}" ]; then
	INPUT_PORT="$AA_TR_PORT"
else
	read -r -p "    Порт приёма H.225 ($CURRENT_PORT): " INPUT_PORT
	INPUT_PORT="${INPUT_PORT:-$CURRENT_PORT}"
fi
case "$INPUT_PORT" in ''|*[!0-9]*) die "порт '$INPUT_PORT' - не число";; esac
{ [ "$INPUT_PORT" -ge 1 ] && [ "$INPUT_PORT" -le 65535 ]; } || die "порт $INPUT_PORT вне диапазона 1..65535"
if [ "$INPUT_PORT" = "$DEF_PORT" ] && [ -z "$(local_port_get "$CONFIG_JSON")" ]; then
	ok "h225_port не задан - действует значение по умолчанию $DEF_PORT"
else
	local_port_set "$CONFIG_JSON" "$INPUT_PORT" || die "в $CONFIG_JSON нет блока local - впишите h225_port вручную."
	ok "local.h225_port = $INPUT_PORT"
fi

# Занятость порта чужим процессом (свой FreeSWITCH при переустановке - норма).
if command -v ss >/dev/null 2>&1; then
	BUSY="$(ss -H -ltnp "sport = :$INPUT_PORT" 2>/dev/null | grep -v freeswitch || true)"
	if [ -n "$BUSY" ]; then
		warn "порт $INPUT_PORT/tcp уже занят не FreeSWITCH:"
		printf '%s\n' "$BUSY" | sed 's/^/        /'
		warn "выберите другой порт (local.h225_port) или остановите занявший его процесс."
	fi
fi
ok "ДАЛЬШЕ ВРУЧНУЮ: в $CONFIG_JSON в блоке trunk впишите реальный IP АТС (ip), порт (port), кодек (codec),"
ok "число линий (lines) и правила номеров (in_number_rules); затем: fs_cli -x \"aa_h323_tr reload\""

# ---------------------------------------------------------------------
step "5/7  Подключение mod_aa_h323_tr в modules.conf.xml"
# ---------------------------------------------------------------------
MODULES_XML="$FS_CONF_DIR/autoload_configs/modules.conf.xml"
if [ -f "$MODULES_XML" ]; then
	if grep -q 'mod_aa_h323_tr' "$MODULES_XML"; then
		ok "mod_aa_h323_tr уже есть в $MODULES_XML"
	else
		sed -i 's#</modules>#  <load module="mod_aa_h323_tr"/>\n</modules>#' "$MODULES_XML"
		ok "добавлено <load module=\"mod_aa_h323_tr\"/> в $MODULES_XML"
	fi
else
	warn "$MODULES_XML не найден - добавьте <load module=\"mod_aa_h323_tr\"/> в конфигурацию модулей вручную."
fi

# ---------------------------------------------------------------------
step "6/7  Проверка подключения dialplan"
# ---------------------------------------------------------------------
FS_XML="$FS_CONF_DIR/freeswitch.xml"
DIALPLAN_XML="$FS_CONF_DIR/dialplan.xml"
if [ -f "$FS_XML" ] && grep -q 'dialplan/\*\.xml' "$FS_XML"; then
	ok "dialplan/*.xml подключается маской в freeswitch.xml - aa_h323_tr.xml подхватится сам"
elif [ -f "$DIALPLAN_XML" ] && grep -q 'aa_h323_tr' "$DIALPLAN_XML"; then
	ok "aa_h323_tr.xml уже подключён в $DIALPLAN_XML"
elif [ -f "$DIALPLAN_XML" ]; then
	sed -i 's#</include>#  <X-PRE-PROCESS cmd="include" data="aa_h323_tr.xml"/>\n</include>#' "$DIALPLAN_XML"
	ok "добавлен include aa_h323_tr.xml в $DIALPLAN_XML"
else
	warn "не нашёл $FS_XML или $DIALPLAN_XML - подключите dialplan/aa_h323_tr.xml сами"
fi

# ---------------------------------------------------------------------
step "7/7  Перезагрузка модуля в работающем FreeSWITCH и проверка статуса"
# ---------------------------------------------------------------------
FS_CLI_BIN="fs_cli"
if ! command -v "$FS_CLI_BIN" >/dev/null 2>&1; then
	CANDIDATE="$(dirname "$FS_CONF_DIR")/bin/fs_cli"
	[ -x "$CANDIDATE" ] && FS_CLI_BIN="$CANDIDATE"
fi
if ! command -v "$FS_CLI_BIN" >/dev/null 2>&1; then
	warn "fs_cli не найден - когда FreeSWITCH будет запущен, выполните сами:"
	warn "  fs_cli -x \"unload mod_aa_h323_tr\""
	warn "  fs_cli -x \"load mod_aa_h323_tr\""
	warn "  fs_cli -x \"aa_h323_tr status\""
else
	set +e   # unload при первой установке закономерно падает - это не ошибка
	ok "fs_cli -x \"unload mod_aa_h323_tr\"";  "$FS_CLI_BIN" -x "unload mod_aa_h323_tr"
	ok "fs_cli -x \"load mod_aa_h323_tr\"";    "$FS_CLI_BIN" -x "load mod_aa_h323_tr"
	ok "fs_cli -x \"aa_h323_tr status\"";      "$FS_CLI_BIN" -x "aa_h323_tr status"
	set -e
	warn "Если новая сборка не подхватилась после unload/load: systemctl restart freeswitch"
	warn "Если load не удался - смотрите хвост лога: tail -100 /var/log/freeswitch/freeswitch.log"
fi

step "Готово"
cat <<EOT

    Если "aa_h323_tr status" не появился или выглядит странно - смотрите лог:
      tail -100 /var/log/freeswitch/freeswitch.log
    Конфиг: $CONFIG_JSON

EOT
