CC := g++
CXXFLAGS := -std=c++11 -W -Wall -Werror -Wextra -O2 -m64 -g
CXX_NO_ERROR_FLAGS := -Wno-deprecated -Wno-unused-result -Wno-unused-function -Wno-unused-parameter
NO_DEPRECATED_WARN := -Wno-deprecated-declarations # -Wno-error=deprecated-declarations
SOFLAGS := -fPIC

DEST_PATH := /usr/local
HEADER_LIST = $(wildcard *.h)
HEADER_LIST += $(wildcard ./detail/*.h)

INCLUDE_PATH := -I./

LIB_PATH := -L/usr/local/lib/
LIB_LIST := /usr/local/lib/libcrypto.a -lutils -llog -lpthread -ldl
GTEST_LIB := /usr/local/lib/libgtest.a

INCLUDE_LIST := $(HEADER_LIST)
INCLUDE_LIST += $(wildcard *.hpp)

SRC_LIST := $(wildcard *.cpp)
SRC_LIST += $(wildcard ./detail/*.cpp)
OBJ_LIST := $(patsubst %.cpp, %.o, $(SRC_LIST))

TESTSRCLIST := $(wildcard ./test/*.cc)
TESTOBJLIST := $(patsubst %.cc, %.o, $(TESTSRCLIST))

TARGET := libeularcrypto.so

$(TARGET) : $(OBJ_LIST)
	$(CC) $^ -o $@ -shared $(LIB_PATH) $(LIB_LIST)
	rm -rf $(OBJ_LIST)

install : $(TARGET)
	-(sudo mv $(TARGET) $(DEST_PATH)/lib)
	-(sudo ldconfig)
	-(sudo rm -rf $(DEST_PATH)/include/crypto/)
	-(if [ ! -d "$(DEST_PATH)/include/crypto/" ]; then sudo mkdir $(DEST_PATH)/include/crypto/; fi)
	-(sudo cp $(HEADER_LIST) $(DEST_PATH)/include/crypto/)
	-(sudo make clean)

uninstall :
	-(sudo rm -rf $(DEST_PATH)/lib/$(TARGET))
	-(sudo rm -rf $(DEST_PATH)/include/crypto)

test: test/testmain

test/testmain : $(TESTOBJLIST) $(OBJ_LIST)
	$(CC) $^ -o $@ $(CXXFLAGS) $(NO_DEPRECATED_WARN) $(CXX_NO_ERROR_FLAGS) $(INCLUDE_PATH) $(LIB_PATH) $(GTEST_LIB) $(LIB_LIST)

%.o : %.cpp
	$(CC) -c $^ -o $@ $(CXXFLAGS) $(NO_DEPRECATED_WARN) $(CXX_NO_ERROR_FLAGS) $(INCLUDE_PATH) $(SOFLAGS)

%.o : %.cc
	$(CC) -c $^ -o $@ $(CXXFLAGS) $(NO_DEPRECATED_WARN) $(CXX_NO_ERROR_FLAGS) $(INCLUDE_PATH) $(SOFLAGS)

.PHONY: install uninstall clean $(TARGET) \
		test test/testmain

clean :
	-rm -rf $(OBJ_LIST) $(TESTOBJLIST)
	-rm -rf test/testmain