# Модуль H323 trunk в FreeSWITCH (mod_aa_h323_tr)
# Copyright (c) 2026: Сенашенко Дмитрий.
# Разработано в Российской Федерации.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -I. -Itests
SAN = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -I. -Itests
.PHONY: test asan tsan fs-syntax clean
test: build/test_tr_msg build/test_tr_config build/test_tr_engine
	./build/test_tr_msg
	./build/test_tr_config
	./build/test_tr_engine
build/test_tr_msg: tests/test_tr_msg.c aa_tr_msg.c aa_tr_msg.h aa_tr_tpl.h tests/test_tr_vec.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_tr_msg.c aa_tr_msg.c
build/test_tr_engine: tests/test_tr_engine.c aa_tr_engine.c aa_tr_engine.h aa_tr_msg.c aa_tr_config.c aa_json.c tests/test_tr_vec.h
	@mkdir -p build
	$(CC) $(CFLAGS) -pthread -o $@ tests/test_tr_engine.c aa_tr_engine.c aa_tr_msg.c aa_tr_config.c aa_json.c
build/test_tr_config: tests/test_tr_config.c aa_tr_config.c aa_tr_config.h aa_json.c aa_json.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_tr_config.c aa_tr_config.c aa_json.c
asan:
	@mkdir -p build
	$(CC) $(SAN) -o build/asan_tr_msg tests/test_tr_msg.c aa_tr_msg.c && ./build/asan_tr_msg
	$(CC) $(SAN) -o build/asan_tr_config tests/test_tr_config.c aa_tr_config.c aa_json.c && ./build/asan_tr_config
	$(CC) $(SAN) -pthread -o build/asan_tr_engine tests/test_tr_engine.c aa_tr_engine.c aa_tr_msg.c aa_tr_config.c aa_json.c && ./build/asan_tr_engine
tsan:
	@mkdir -p build
	$(CC) -O1 -g -fsanitize=thread -pthread -Wall -I. -Itests -o build/tsan_tr_engine tests/test_tr_engine.c aa_tr_engine.c aa_tr_msg.c aa_tr_config.c aa_json.c && ./build/tsan_tr_engine
fs-syntax:
	$(CC) -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Ifs/stub -I. fs/mod_aa_h323_tr.c
clean:
	rm -rf build
