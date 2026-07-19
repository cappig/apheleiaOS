PROFILE ?= fast

DOCKER_ARCH_VARS :=

GNU_CC_CANDIDATES_x86_64 := x86_64-elf-gcc x86_64-linux-gnu-gcc gcc
GNU_CC_CANDIDATES_x86_32 := i686-elf-gcc i386-elf-gcc i686-linux-gnu-gcc i386-linux-gnu-gcc gcc

ifndef GNU_CC_x86_64
GNU_CC_x86_64 := $(call pick_tool,$(GNU_CC_CANDIDATES_x86_64))
endif
ifndef GNU_CC_x86_32
GNU_CC_x86_32 := $(call pick_tool,$(GNU_CC_CANDIDATES_x86_32))
endif

ifndef GNU_LD_x86_64
GNU_LD_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),ld,x86_64-elf-ld x86_64-linux-gnu-ld ld)
endif
ifndef GNU_LD_x86_32
GNU_LD_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),ld,i686-elf-ld i386-elf-ld i686-linux-gnu-ld i386-linux-gnu-ld)
endif

ifndef GNU_AR_x86_64
GNU_AR_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),ar,x86_64-elf-ar x86_64-linux-gnu-ar ar)
endif
ifndef GNU_AR_x86_32
GNU_AR_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),ar,i686-elf-ar i386-elf-ar i686-linux-gnu-ar i386-linux-gnu-ar ar)
endif

ifndef GNU_OC_x86_64
GNU_OC_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),objcopy,x86_64-elf-objcopy x86_64-linux-gnu-objcopy objcopy)
endif
ifndef GNU_OC_x86_32
GNU_OC_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),objcopy,i686-elf-objcopy i386-elf-objcopy i686-linux-gnu-objcopy i386-linux-gnu-objcopy)
endif

ifndef GNU_ST_x86_64
GNU_ST_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),strip,x86_64-elf-strip x86_64-linux-gnu-strip strip)
endif
ifndef GNU_ST_x86_32
GNU_ST_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),strip,i686-elf-strip i386-elf-strip i686-linux-gnu-strip i386-linux-gnu-strip)
endif

LLVM_AR_x86_64 ?= $(call pick_tool,llvm-ar ar)
LLVM_CC_x86_64 ?= clang
LLVM_LD_x86_64 ?= ld.lld
LLVM_OC_x86_64 ?= llvm-objcopy
LLVM_ST_x86_64 ?= llvm-strip

LLVM_AR_x86_32 ?= $(call pick_tool,llvm-ar ar)
LLVM_CC_x86_32 ?= clang
LLVM_LD_x86_32 ?= ld.lld
LLVM_OC_x86_32 ?= llvm-objcopy
LLVM_ST_x86_32 ?= llvm-strip
