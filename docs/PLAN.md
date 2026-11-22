# mod_aa_h323_tr — H.323-транк FreeSWITCH <-> Avaya CM (чистый C)

Модуль: `mod_aa_h323_tr`, команда `fs_cli -x "aa_h323_tr ..."`. Лицензирования и trial нет, лимитов нет.
Основа обвязки — mod_aa_h323 (endpoint, fsbridge, media). Кодеки: G.711A (PCMA), G.711μ (PCMU).

## Исходные решения (пользователь)
* Исходящая сторона строится по дампу h323_out_101_102.pcap (снят на 192.168.0.193, звонящая АТС).
* Входящая сторона — по h323_inc_101_102.pcap (снят на 192.168.0.6, принимающая АТС).
* Обслуживание транка — по h323_out_b_o.pcap (STATUS с глобальным call reference: cause 47 = вне обслуживания, 31/16 = в обслуживании).
* Адрес/версия «Adapter_3.5.8» — просто имя, игнорировать (на .6 эмулятор CM).
* Пока без: calling number (на CM ошибка настройки), DTMF-дампов, keepalive/Layer 3 test. Shuffling (direct IP-IP) нас не касается.
* RTP в дампах нет: подставляем в fastStart адрес/порт FreeSWITCH, принимаем RTP от шлюза соседа (relay/latch как в mod_aa_h323).

## Подтверждено пользователем (вопросы 6–14)
* Отдельный listener H.225 на порту 1722 (1720 занят mod_aa_h323).
* Транков несколько: `trunks.<name>` {peer_ip, peer_port=1720, ...}; конфиг JSON `aa_h323_tr.json`.
* Исходящий: `aa_h323_tr/<транк>/<номер>`; входящий: контекст `aa_h323_tr`, `destination_number` = called.
* DTMF: решено RFC 2833; дамп pn4 показывает, что CM шлёт H.245 UII — вопрос к пользователю. Product/version в UUIE — отдельные ключи конфига.
* Правила номеров, преобразование calling/called — на стороне FreeSWITCH (модуль не трогает номера).
* Нагрузка: до 100 одновременных вызовов.
* Лицензия — свободный пакет. Репозиторий AA-FS-H323-TR. Передача: mod_aa_h323_tr_N.tar.gz.

## Что видно в дампах
SETUP (fastStart: 4 OLC, RTP шлюза звонящего) -> CALL PROCEEDING -> ALERTING (fastStart-ответ с RTP шлюза принимающего)
-> ~11 FACILITY с туннелированным H.245 (TCS/MSD в обе стороны + ack) -> CONNECT (+Display) -> отбой: FACILITY (endSession) + RELEASE COMPLETE cause 16.
Не ответ: звонящий отбивает до CONNECT, тот же обмен, cause 16. Без RAS. Одно TCP-соединение на вызов.

## Проходы
1. PER-кодек подмножества H.225/H.245 + проверка на кадрах из дампов (tools/, tests/).
2. Транк-движок: автоматы входящего/исходящего вызова, H.245, fastStart, состояние транка.
3. Медиа (aa_h323_media), мост к каналам FS.
4. Модуль FS: endpoint aa_h323_tr, команда aa_h323_tr, конфиг, install.sh/deb/README.
5. Реплей дампов, ASan/TSan; сборка FS-части — у пользователя.

## Статус
* **Проход 1 — готов:** `aa_tr_msg.c/h` (разбор и сборка Q.931/H.225/туннелированного H.245 по шаблонам), `tools/gen_tr.py` (шаблоны из дампов),
  `tests/test_tr_msg.c`: все 135 кадров H.225 из 4 дампов разбираются и собираются обратно побайтно; PDU H.245 (MSD, MSD-ack, TCS-ack, endSession, UII 0–9,
  TCS обеих сторон) совпадают со сборщиками; правило master/slave (разность по модулю 2^24) сходится на 14 проверках. ASan/UBSan чисто. `make test`, `make asan`.
* Открыто: (1) какой из fastStart-кодеков G.711A / G.711μ — в шаблонах 4 OLC (каналы 0x20 и 0x60), пары совпадают побайтно;
  нужен дамп с изменённым списком кодеков на CM (только G.711A, потом только G.711μ); (2) 2 байта перед conferenceID в SETUP (43 50, в вызове на 999 — cc c0) — пока копируются;
  (3) UII кроме цифр 0–9 (#, *, A–D) собраны по формуле ASCII, на дампе не проверены; (4) повторы UII (12 кадров на 10 цифр) — смысл не выяснен.
* Дальше — проход 2: автомат вызова, H.245-обмен, состояние транка, таймеры.

## Проход 1б: конфиг, установка, заготовка модуля
- `config/aa_h323_tr.example.json`: `local.ip`, `local.h225_port` (необязательный, по умолчанию 1722), `identity`, массив `trunk[]`
  (`name`, `ip`, `port`=1720, `codec` PCMA|PCMU, `lines`=100, `in_number_rules[]` {match, strip, add}), `logging`.
- Правила входящего номера: первое подходящее; 111123 -> 0123 при {match:"111", strip:3, add:"0"}.
- `install.sh` (+ `packaging/jsonkey.sh`): сборка, установка .so, XML-конфиг/диалплан, правка local.ip/h225_port, modules.conf.xml, unload/load/status.
  Без вопросов: `AA_TR_LOCAL_IP=... AA_TR_PORT=... ./install.sh`.
- `fs/mod_aa_h323_tr.c`: заготовка (загрузка, API `aa_h323_tr help|status|trunks|reload|number`); в песочнице не компилировалась (нет заголовков FS).
- Установленный конфиг: `$FS_CONF_DIR/aa_h323_tr/aa_h323_tr.json`.

## Проход 2-4: движок вызовов, FS-endpoint, RTP (mod_aa_h323_tr_4)
- `aa_tr_engine.c/h` — чистый C (pthreads): listener local.ip:h225_port (роль «.6»), исходящие соединения к trunk[].ip:port (роль «193»),
  один TCP = один вызов = один поток. Автомат по дампам: входящий — CALL_PROC, ALERTING(fastStart), TCS, MSD сразу; на TCS -> TCSack, на MSD ->
  MSDack (master=0x80/slave=0xa0 по правилу 2^24), x1 шлёт slave после всех четырёх подтверждений; CONNECT по ответу FreeSWITCH.
  Исходящий — SETUP, затем на первый TCS/MSD дальней стороны отвечаем своими. Отбой: END + RELEASE_COMPLETE. STATUS (call ref 0) cause 47 =
  транк выведен (исходящие -> cause 38), 31/16 = в обслуживании. Лимит lines (cause 34), неизвестный IP (cause 21), нет общего кодека (cause 88),
  TCP keepalive. Правила входящего номера применяются в движке.
- `fs/mod_aa_h323_tr.c` — endpoint `aa_h323_tr` по образцу mod_aa_h323: RTP через switch_rtp внутри канала FS (отдельный реле не нужен:
  в fastStart сообщаем адрес FS, RTP шлём на адрес из fastStart дальней стороны), dial string `aa_h323_tr/<транк>/<номер>`, входящие в контекст XML
  `context` с destination_number после правил, DTMF: UII от АТС -> channel DTMF, DTMF FreeSWITCH -> UII; ответ FS -> CONNECT; hangup FS -> END+RC.
- Тесты: `make test` (msg 726, config 29, engine 93 проверок), `make asan`, `make tsan` — чисто; `make fs-syntax` — проверка синтаксиса против
  самописной заглушки (не настоящие заголовки FS: реальная сборка — на стенде).
- Не проверено на живой АТС: исходящий SETUP от FreeSWITCH к CM (по дампу 193->6), RFC 2833 (сейчас DTMF только через UII), ранние медиа.

## Проход 5
- Занятый порт H.225 больше не блокирует загрузку модуля: start() возвращает 0, listener повторно пробуется раз в 5 с, status показывает NOT LISTENING (причина). Исходящие вызовы работают без listener. Тест 10 в test_tr_engine.

## Проход 6
- install.sh: `local_port_set` перезаписывает h225_port и удаляет дубли (раньше дописывал строку при каждом запуске).
- Служебное соединение к каждому транку (по дампу h323_out_b_o): при подключении STATUS(global cr) cause 31, затем 16; далее STATUS 16 каждые 7.5 с;
  ответные STATUS 47/31/16 меняют состояние транка; обрыв - переподключение раз в 5 с. Причина: CM показывал trunk out-of-service-NE,
  пока на его порт не приходил STATUS от нашей стороны. Гипотеза, проверить на стенде по status trunk и pcap.

## Проход 7 (по дампам tr_hb и dump2111-3 со стенда)
- Служебное соединение к CM работает: наш STATUS 31/16 + 16 каждые 7.6 с совпадает с «.6» побайтно, CM подтверждает TCP, но молчит. Единственный ответ CM
  (через 27 с после старта, GUID эхом): STATUS cause 47, затем 16. У настоящего «.6» в дампах ответ CM: 31, затем 16. Транки группы 5 остаются out-of-service-NE
  при signaling group 7 in-service; CM к порту 1722 после перезагрузки модуля не подключался.
- Добавлено: на входящем соединении от транка без SETUP (постоянное соединение CM к Far-end Listen Port) через 1 с тишины шлём STATUS 31/16 и далее 16 каждые 7.6 с
  (в дампе pn4 «.6» шлёт 16 на такое соединение, и CM отвечает 31/16).
- tools/pcapdump.py понимает linktype 276 (Linux cooked v2, tcpdump -i any).
