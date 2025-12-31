PWD := $(shell pwd)
CC := g++
include 3rd_party/3rd_party.mk # 必须放在PWD后面, 其他.mk文件会使用PWD

INCLUDE_DIR := -I$(PWD) $(YAML_CXX_INCLUDES) $(JSON_CXX_INCLUDES)
CPPFLAGS  := -std=c++11 # -m64
LIBFLAGS  := -fPIC
LIB_LIST  := -lutils -lpthread

GTEST_LIB := /usr/local/lib/libgtest.a

DEST_PATH := /usr/local
DEST_INC  := $(DEST_PATH)/include
DEST_LIB  := $(DEST_PATH)/lib/

SRC_LIST = $(wildcard $(PWD)/*.cpp)
OBJ_LIST = $(patsubst %.cpp, %.o, $(SRC_LIST))

HEADER_FILE_LIST = 	\
	IniConfig.h		\
	JsonConfig.h	\
	typecast.h		\
	XmlConfig.h		\
	YamlConfig.h	\

GEN_BIN := if [ ! -d bin ]; then mkdir bin; fi
GEN_OBJ := if [ ! -d obj ]; then mkdir obj; fi

LIB_NAME = libconfig.so
TARGET := bin/$(LIB_NAME)

all:
	$(GEN_BIN)
	make $(TARGET)

install: all
	if [ ! -d $(DEST_INC)/config ]; then mkdir $(DEST_INC)/config; fi
	cp -r $(HEADER_FILE_LIST) $(DEST_INC)/config
	mv $(TARGET) $(DEST_LIB)
	cp -r $(JSON_CXX_INCLUDE_PATH) $(DEST_INC)
	cp -r $(YAML_CXX_INCLUDE_PATH) $(DEST_INC)
	ldconfig

uninstall:
	rm -rf $(DEST_INC)/config
	rm $(DEST_LIB)/$(LIB_NAME)

$(TARGET) : $(OBJ_LIST) $(JSON_CXX_OBJ_LIST) $(YAML_CXX_OBJ_LIST)
	$(CC) $^ -o $@ -shared $(LIB_LIST)

# 以下表示编译当前文件夹下的源文件时使用以下编译方式，用于对jsoncpp和yamlcpp区分
$(PWD)/%.o : $(PWD)/%.cpp
	$(CC) $^ -c -o $@ $(CPPFLAGS) $(LIBFLAGS) $(INCLUDE_DIR)

bin/test_yaml_config : test/test_yaml_config.cpp YamlConfig.cpp rwmutex.cpp
	$(GEN_BIN)
	$(CC) $^ -o $@ $(INCLUDE_DIR) $(YAML_CXX_OBJ_LIST) $(GTEST_LIB) -lpthread

bin/test_json_config: test/test_json_config.cpp
	$(GEN_BIN)
	$(CC) $^ -o $@ $(INCLUDE_DIR) $(JSON_CXX_OBJ_LIST) $(GTEST_LIB) -lpthread

bin/test_xml_config: test/test_xml_config.cpp XmlConfig.cpp tinyxml2.cpp rwmutex.cpp
	$(GEN_BIN)
	$(CC) $^ -o $@ $(INCLUDE_DIR) -lpthread

bin/test_ini_config: test/test_ini_config.cpp IniConfig.cpp
	$(GEN_BIN)
	$(CC) $^ -o $@ $(INCLUDE_DIR)

bin/test_rwmutex: test/test_rwmutex.cpp rwmutex.cpp
	$(GEN_BIN)
	$(CC) $^ -o $@ $(INCLUDE_DIR) -lpthread

debug:
	@echo $(JSON_CXX_SRC_LIST)
	@echo $(JSON_CXX_FLAGS)
	@echo $(JSON_CXX_INCLUDES)
	@echo $(JSON_CXX_OBJ_LIST)

.PHONY: all clean install uninstall \
	bin/test_yaml_config	\
	bin/test_json_config	\
	bin/test_xml_config		\
	bin/test_rwmutex		\

clean:
	rm -rf bin $(OBJ_LIST) $(JSON_CXX_OBJ_LIST) $(YAML_CXX_OBJ_LIST) $(TARGET)