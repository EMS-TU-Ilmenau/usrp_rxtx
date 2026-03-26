# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2023-2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

NAME=usrp_rxtx
BUILD_DIR=build
SHELL=/bin/sh

.PHONY: release
release: $(BUILD_DIR)/build.ninja
	meson configure --buildtype release --optimization 2 -Db_sanitize=none $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

.PHONY: debug
debug: $(BUILD_DIR)/build.ninja
	meson configure --buildtype debug --optimization g -Db_sanitize=none $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

.PHONY: debug_asan
debug_asan: $(BUILD_DIR)/build.ninja
	meson configure --buildtype debug --optimization g -Db_sanitize=address,undefined $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

.PHONY: debug_msan
debug_msan: $(BUILD_DIR)/build.ninja
	meson configure --buildtype debug --optimization g -Db_sanitize=memory $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

.PHONY: clean
clean:
	@ if [ -h $(BUILD_DIR) ] ; then \
		rm -rf `readlink $(BUILD_DIR)` ;\
		rm $(BUILD_DIR) ;\
	else \
		rm -rf $(BUILD_DIR) ;\
	fi

.PHONY: test
test: $(BUILD_DIR)/build.ninja
	meson configure --buildtype debug --optimization g -Db_sanitize=address,undefined $(BUILD_DIR)
	meson test -C $(BUILD_DIR) --print-errorlogs

$(BUILD_DIR)/build.ninja:
	@ if [ ! -e $(BUILD_DIR) ] ; then \
		ln -s `mktemp -d /tmp/$(NAME)-build.XXXXXXXX` $(BUILD_DIR) ;\
		chmod 700 `readlink $(BUILD_DIR)` ;\
	fi
	meson setup $(BUILD_DIR)
