# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# jsonkey.sh - чтение/правка ключей aa_h323_tr.json БЕЗ Python (awk). Подключается через `. jsonkey.sh`.
# Ожидает раскладку как в config/aa_h323_tr.example.json: объект "local": { "ip": "...", "h225_port": N, ... }.
# Остальной файл не трогается (комментарии/порядок/отступы сохраняются).

local_ip_get() {   # local_ip_get FILE -> текущий local.ip (пусто, если нет)
    awk '
        /"local"[[:space:]]*:/ { inl = 1 }
        inl && /"ip"[[:space:]]*:/ {
            s = $0; sub(/.*"ip"[[:space:]]*:[[:space:]]*"/, "", s); sub(/".*/, "", s); print s; exit
        }
        inl && /}/ { exit }
    ' "$1" 2>/dev/null
}

local_ip_set() {   # local_ip_set FILE IP -> 0 заменено, 1 ключ не найден
    local f="$1" ip="$2" tmp rc
    tmp="$(mktemp)" || return 1
    awk -v ip="$ip" '
        /"local"[[:space:]]*:/ { inl = 1 }
        inl && !done && /"ip"[[:space:]]*:/ {
            sub(/"ip"[[:space:]]*:[[:space:]]*"[^"]*"/, "\"ip\": \"" ip "\""); done = 1
        }
        inl && /}/ { sub(/,[[:space:]]*}/, " }"); inl = 0 }
        { print }
        END { exit done ? 0 : 1 }
    ' "$f" > "$tmp" && cat "$tmp" > "$f"
    rc=$?
    rm -f "$tmp"
    return $rc
}

# local.h225_port: пусто, если ключа нет (значит действует порт по умолчанию 1722).
local_port_get() {
    awk '
        /"local"[[:space:]]*:/ { inl = 1 }
        inl && /"h225_port"[[:space:]]*:/ {
            s = $0; sub(/.*"h225_port"[[:space:]]*:[[:space:]]*/, "", s); sub(/[^0-9].*/, "", s); print s; exit
        }
        inl && /}/ { exit }
    ' "$1" 2>/dev/null
}

local_port_set() {   # local_port_set FILE PORT -> 0 значение ПЕРЕЗАПИСАНО (или добавлено, если ключа не было), 1 нет блока local
    # Два прохода: 1) есть ли h225_port в local; 2) если есть - первое вхождение перезаписывается, лишние дубли
    # (от старых версий скрипта) удаляются; если нет - ключ добавляется после "ip".
    local f="$1" port="$2" tmp rc
    tmp="$(mktemp)" || return 1
    awk -v port="$port" '
        NR == FNR {
            if ($0 ~ /"local"[[:space:]]*:/) l1 = 1
            if (l1 && $0 ~ /"h225_port"[[:space:]]*:/) has = 1
            if (l1 && $0 ~ /}/) l1 = 0
            next
        }
        /"local"[[:space:]]*:/ { inl = 1 }
        inl && /"h225_port"[[:space:]]*:/ {
            if (done) {                         # дубль: удалить строку (или только пару ключ-значение в одной строке)
                if ($0 ~ /^[[:space:]]*"h225_port"[[:space:]]*:[[:space:]]*[0-9]+[[:space:]]*,?[[:space:]]*(\"_note\".*)?$/ && $0 !~ /_note/) next
                sub(/[[:space:]]*"h225_port"[[:space:]]*:[[:space:]]*[0-9]+[[:space:]]*,?/, "")
            } else {
                sub(/"h225_port"[[:space:]]*:[[:space:]]*[0-9]+/, "\"h225_port\": " port); done = 1
                p = index($0, "\"h225_port\"") + 10; rest = substr($0, p + 1)   # дубли в той же строке
                gsub(/"h225_port"[[:space:]]*:[[:space:]]*[0-9]+[[:space:]]*,?[[:space:]]*/, "", rest)
                $0 = substr($0, 1, p) rest
            }
        }
        inl && !has && !done && /"ip"[[:space:]]*:/ {      # ключа нет совсем: добавить после строки ip
            line = $0
            if (line ~ /}/) {                              # блок local в одну строку
                sub(/"ip"[[:space:]]*:[[:space:]]*"[^"]*"/, "&, \"h225_port\": " port); done = 1
            } else {
                if (line !~ /,[[:space:]]*$/) line = line ","
                print line; print "    \"h225_port\": " port ","; done = 1; next
            }
        }
        inl && /}/ { sub(/,[[:space:]]*}/, " }"); inl = 0 }
        { print }
        END { exit done ? 0 : 1 }
    ' "$f" "$f" > "$tmp" && cat "$tmp" > "$f"
    rc=$?
    rm -f "$tmp"
    return $rc
}
