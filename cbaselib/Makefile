BUILD_DIR = ./build
NET_DIR = ./net
BASE_DIR = ./base
TEST_DIR = ./test
INC_DIR = ./include

EXE_PATH = $(BUILD_DIR)/bin

TEST = YES

CPP = g++
CC = gcc
LD = g++
SZ = size

NET_SRCS += session.cpp jpoll.cpp parser.cpp protocol.cpp rpc.cpp 
NET_SRCS := $(addprefix $(NET_DIR)/src/,$(NET_SRCS))
BASE_SRCS += bytes.cpp spin_lock.cpp threadpool.cpp
BASE_SRCS := $(addprefix $(BASE_DIR)/src/,$(BASE_SRCS))
TEST_SRCS += test_proto.cpp
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(TEST_SRCS))

#这里用来表示EXE
TOTAL_TEST_SRCS += test.cpp #test2.cpp 
TOTAL_TEST_SRCS := $(addprefix $(TEST_DIR)/,$(TOTAL_TEST_SRCS))

TEST_EXE_SRCS += $(TOTAL_TEST_SRCS)

SRCS +=  $(NET_SRCS) $(BASE_SRCS) $(TEST_SRCS)

DEF += -ggdb -g
DEF += -Wall 
DEF += -std=gnu++11 
DEF += -Wno-unused-function -Wno-unused-but-set-variable -Wno-literal-suffix
#DEF += -D_SINGLE_THREAD_

INC= -I./ -I$(NET_DIR)/inc/ -I$(BASE_DIR)/inc -I$(INC_DIR)

LDFLAGS = -lpthread

CFLAGS += $(INC) $(DEF)
CXXFLAGS += $(INC) $(DEF)


OBJS := $(SRCS:.c=.o)
OBJS := $(SRCS:.cpp=.o)
OBJS := $(addprefix $(BUILD_DIR)/,$(OBJS))

ifeq ($(TEST),YES)
	TEST_OBJS := $(TEST_EXE_SRCS:.cpp=)
	TEST_OBJS := $(addprefix $(BUILD_DIR)/,$(TEST_OBJS))

	EXE := $(TEST_EXE_SRCS:.cpp=)
	EXE := $(addprefix $(BUILD_DIR)/,$(EXE))

endif

all: $(EXE)

$(BUILD_DIR)/%.o:%.c
	@mkdir -p $(dir $@)
	@$(CC) -c $(CFLAGS) -o $@ $^
	@printf "\033[32m%-48sComplied.\033[0m\n" $(notdir $@) 

$(BUILD_DIR)/%.o:%.cpp
	@mkdir -p $(dir $@)
	@$(CPP) -c $(CXXFLAGS) -o $@ $^
	@printf "\033[32m%-48sComplied.\033[0m\n" $(notdir $@) 

$(EXE):%:%.o $(OBJS)
	@$(LD) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@printf "\033[33m%-48sComplied.\033[0m\n" $(notdir $@) 
	@$(SZ) $@

.PHONY: clean test
clean:
	-rm -rf $(EXE)	$(OBJS) $(BUILD_DIR)

test:
	echo $(TOTAL_TEST_SRCS)
