INCLUDE_DIR := -I./include/
POSIX_MICRO := -D_GNU_SOURCE -D__USE_FILE_OFFSET64 -D_LARGEFILE64_SOURCE 
CXX_FLAGS := -std=c++11 -DNDEBUG $(POSIX_MICRO) -W -Wall -Werror -Wextra -O2 -m64 -g
CXX_NO_ERROR_FLAGS := -Wno-deprecated -Wno-unused-result -Wno-class-memaccess -Wno-unused-function -Wno-unused-parameter
SO_FLAGS := -fPIC
LIB_LIST := -lpthread -liconv
CC := g++
DEST_DIR := /usr/local

SRC_LIST := $(wildcard ./src/*.cpp)
HEADER_FILE_LIST := $(wildcard ./include/utils/*.h)
HEADER_FILE_LIST += $(wildcard ./include/utils/*.hpp)
OBJ_LIST := $(patsubst %.cpp, %.o, $(SRC_LIST))

libutils.so : $(OBJ_LIST)
	$(CC) $^ -o $@ -shared $(LIB_LIST)

%.o : %.cpp
	$(CC) $^ -c -o $@ $(CXX_FLAGS) $(CXX_NO_ERROR_FLAGS) $(SO_FLAGS) $(INCLUDE_DIR)

debug:
	@echo $(SRC_LIST)
	@echo $(OBJ_LIST)
	@echo $(HEADER_FILE_LIST)

.PHONY:clean install uninstall

install : libutils.so
	-sudo mv ./libutils.so $(DEST_DIR)/lib
	-sudo ldconfig
	-sudo rm -rf $(DEST_DIR)/include/utils
	-sudo cp ./include/* $(DEST_DIR)/include/ -r

uninstall:
	-sudo rm -rf $(DEST_DIR)/lib/libutils.so
	-sudo rm -rf $(DEST_DIR)/include/utils

clean:
	-rm -rf $(OBJ_LIST)