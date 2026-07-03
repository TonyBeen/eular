CC := $(shell which g++)
CURRENT_PATH := $(shell pwd)
BIN_TARGET_DIR := $(CURRENT_PATH)/bin
XML_TARGET_DIR := $(CURRENT_PATH)/xml

CXX_FLAGS := -std=c++11 -m64 -O0 -g
INC_PATH := -I$(CURRENT_PATH) -I$(CURRENT_PATH)/../utils/include/

SRC_LIST := $(wildcard *.cc)
EXE_LIST := $(patsubst %.cc, $(BIN_TARGET_DIR)/%.out, $(SRC_LIST))

SHARED_LIB := -lutils -llog -lpthread
STATIC_LIB := /usr/local/lib/libgtest.a

# 检查Valgrind是否安装
VALGRIND_INSTALLED := $(shell command -v valgrind 2> /dev/null)

all:check_path $(EXE_LIST)

check_path:
	@if [ ! -d $(BIN_TARGET_DIR) ]; then mkdir -p $(BIN_TARGET_DIR); fi
	@if [ ! -d $(XML_TARGET_DIR) ]; then mkdir -p $(XML_TARGET_DIR); fi

memcheck: check_valgrind $(EXE_LIST) execute_with_valgrind

check_valgrind:
	@if [ ! "$(VALGRIND_INSTALLED)" ]; then \
		echo "Valgrind is not installed, please install Valgrind first."; \
		exit 1; \
	fi

execute_with_valgrind:
	@for executable in $(EXE_LIST); do \
		echo "Execute Valgrind command for memory check: $$executable"; \
		valgrind --xml=yes --xml-file=./xml/$$(basename $$executable)_mem_check.xml --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose $$executable; \
		echo "$$executable over..."; \
		echo ""; \
	done

$(BIN_TARGET_DIR)/%.out: %.cc
	$(CC) $^ -o $@ $(INC_PATH) $(CXX_FLAGS) $(STATIC_LIB) $(SHARED_LIB)

.PHONY : clean all

clean :
	-rm -rf bin xml