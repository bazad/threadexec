THREADEXEC_LIB = $(LIB_DIR)/libthreadexec.a

DEBUG      ?= 0
ARCH       ?= x86_64
SDK        ?= macosx
SIGNING_ID ?= -
EXTRA_CFLAGS ?=

SYSROOT  := $(shell xcrun --sdk $(SDK) --show-sdk-path)
ifeq ($(SYSROOT),)
$(error Could not find SDK "$(SDK)")
endif
CLANG    := $(shell xcrun --sdk $(SDK) --find clang)
AR       := $(shell xcrun --sdk $(SDK) --find ar)
CC       := $(CLANG) -isysroot $(SYSROOT) -arch $(ARCH)

SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = bin
LIB_DIR = lib

ERRFLAGS = -Wall -Werror
CFLAGS   = -O2 $(ERRFLAGS) -I$(INC_DIR) -I$(SRC_DIR) $(DEFINES) $(EXTRA_CFLAGS)
ARFLAGS  = r

ifneq ($(DEBUG),0)
DEFINES += -DDEBUG=$(DEBUG)
endif

THREADEXEC_SRCS = $(THREADEXEC_ARCH_SRCS) \
		  task_api/tx_init_task.c \
		  thread_api/tx_init_thread.c \
		  thread_api/tx_stage0_mach_ports.c \
		  thread_api/tx_stage0_read_write.c \
		  thread_api/tx_stage1_shared_memory.c \
		  thread_call.c \
		  threadexec_base.c \
		  threadexec_call.c \
		  threadexec_init.c \
		  threadexec_mach_port.c \
		  threadexec_read_write.c \
		  threadexec_shared_vm.c \
		  tx_call.c \
		  tx_init_shmem.c \
		  tx_log.c \
		  tx_utils.c

THREADEXEC_HDRS = $(THREADEXEC_ARCH_HDRS) \
		  task_api/tx_init_task.h \
		  thread_api/tx_init_thread.h \
		  thread_api/tx_stage0_mach_ports.h \
		  thread_api/tx_stage0_read_write.h \
		  thread_api/tx_stage1_shared_memory.h \
		  thread_call.h \
		  tx_call.h \
		  tx_init_shmem.h \
		  tx_internal.h \
		  tx_log.h \
		  tx_params.h \
		  tx_prototypes.h \
		  tx_utils.h

THREADEXEC_INCS = $(THREADEXEC_ARCH_INCS) \
		  threadexec.h

THREADEXEC_INCS := $(THREADEXEC_INCS:%=threadexec/%)

THREADEXEC_ARCH_arm64_SRCS = thread_call_arm64.c

THREADEXEC_ARCH_arm64_HDRS = thread_call_arm64.h

THREADEXEC_ARCH_x86_64_SRCS = thread_call_x86_64.c

THREADEXEC_ARCH_x86_64_HDRS = thread_call_x86_64.h

THREADEXEC_ARCH_SRCS = $(THREADEXEC_ARCH_$(ARCH)_SRCS:%=$(ARCH)/%)
THREADEXEC_ARCH_HDRS = $(THREADEXEC_ARCH_$(ARCH)_HDRS:%=$(ARCH)/%)

THREADEXEC_SRCS := $(THREADEXEC_SRCS:%=$(SRC_DIR)/%)
THREADEXEC_HDRS := $(THREADEXEC_HDRS:%=$(SRC_DIR)/%)
THREADEXEC_INCS := $(THREADEXEC_INCS:%=$(INC_DIR)/%)
THREADEXEC_OBJS := $(THREADEXEC_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(THREADEXEC_LIB)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(THREADEXEC_HDRS) $(THREADEXEC_INCS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(THREADEXEC_LIB): $(THREADEXEC_OBJS)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

clean:
	rm -rf -- $(OBJ_DIR) $(LIB_DIR)
