# global directory layout
BIN          := $(ROOT)/bin
INC          := $(ROOT)/inc
LIB          := $(ROOT)/lib
SRC          := $(ROOT)/src/main/c

# project directories and files
PROJECTDIR   := .
OBJDIR       := $(PROJECTDIR)/obj
OUTPUTDIR    := $(BIN)

OBJFILES     := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(CFILES))) \
                $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(CCFILES)))
TARGETFILE   := $(OUTPUTDIR)/$(PROJECT)

# Java path
#JAVA_HOME    ?= /usr/lib/jvm/java-1.7.0
JAVA_HOME    ?= /opt/jdk7

# DPDK path
DPDK_DIR     ?= /home/azq/build/dpdk-2.1.0/x86_64-native-linuxapp-gcc

# SPDK path
SPDK_DIR     ?= /home/azq/build/spdk-af2a731

# compiler setup
AMD64 = $(shell uname -m | grep 64)
ifeq "$(strip $(AMD64))" ""
    ARCH     := -m32
else
    ARCH     := -m64
endif

ifeq ($(dbg),1)
    DEBUG    := -g -D_DEBUG
    CONFIG   := debug
else
    DEBUG    := -O2 -fno-strict-aliasing
    CONFIG   := release
endif

INCLUDES     := -I$(PROJECTDIR) -I$(INC)
ifeq ($(jni),1)
    INCLUDES += -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
endif
ifeq ($(spdk),1)
    INCLUDES += -I$(SPDK_DIR)/include -I$(DPDK_DIR)/include
endif

LIBRARIES    := -L$(LIB) -L$(OUTPUTDIR)
ifeq ($(rdma),1)
   LIBRARIES += -libverbs
endif
ifeq ($(spdk),1)
   LIBRARIES += -L$(SPDK_DIR)/lib/nvme -L$(SPDK_DIR)/lib/memory -L$(SPDK_DIR)/lib/util \
                -lpciaccess -lpthread \
                -lspdk_nvme -lspdk_memory -lspdk_util \
                -L$(DPDK_DIR)/lib -lrte_eal -lrte_mempool -lrte_ring -Wl,-rpath=$(DPDK_DIR)/lib -lrt
endif

CC           := gcc
CFLAGS       := -fPIC -fstack-protector -std=c99 $(ARCH) $(DEBUG) $(INCLUDES)
CXX          := g++
CXXFLAGS     := -fPIC $(ARCH) $(DEBUG) $(INCLUDES)

LINK         := g++
LDFLAGS     := -fPIC -Wl,-rpath -Wl,\$$ORIGIN
ifeq ($(shared),1)
     LDFLAGS += -shared
  TARGETFILE := $(addsuffix .so,$(TARGETFILE))
endif
ifeq ($(spdk),1)
     LDFLAGS += -Wl,-z,relro,-z,now -Wl,-z,noexecstack
endif

# misc.
ifeq ($(quiet), 1)
    QUIET    := @
else
    QUIET    :=
endif

# build rules
.PHONY: all
all: build

.PHONY: build
build: $(TARGETFILE)

$(OBJDIR)/%.o: %.c $(DEPFILES)
	$(QUIET)$(CC) $(CFLAGS) -o $@ -c $<

$(OBJDIR)/%.o: %.cpp $(DEPFILES)
	$(QUIET)$(CXX) $(CXXFLAGS) -o $@ -c $<

$(TARGETFILE): $(OBJFILES) | $(OUTPUTDIR)
	$(QUIET)$(LINK) $(LDFLAGS) -o $@ $+ $(LIBRARIES)

$(OUTPUTDIR):
	mkdir -p $(OUTPUTDIR)

$(OBJFILES): | $(OBJDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

.PHONY: run
run: build
	$(QUIET)$(TARGETFILE)

.PHONY: tidy
tidy:
	$(QUIET)find . | egrep "#" | xargs rm -f
	$(QUIET)find . | egrep "\~" | xargs rm -f

.PHONY: clean
clean: tidy
	$(QUIET)rm -f $(OBJFILES)
	$(QUIET)rm -f $(TARGETFILE)

.PHONY: clobber
clobber: clean
	$(QUIET)rm -rf $(OBJDIR)
