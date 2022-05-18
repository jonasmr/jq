UNAME_S := $(shell uname -s)


LDFLAGS=-lpthread -lcurses

CFLAGS=-I.. -I../microprofile -I. -DGLEW_STATIC -Wall
#CFLAGS+=-fsanitize=undefined-trap -fsanitize-undefined-trap-on-error
CFLAGS+=-g -O0 -Wno-invalid-offsetof -Wno-format
CFLAGS+=-DJQ_ASSERT_LOCKS
CFLAGS+=-DJQ_MICROPROFILE
CFLAGS+=-DMICROPROFILE_MAX_THREADS=128
CPPFLAGS=$(CFLAGS)

#CPPFLAGS+=-stdlib=libc++ -std=c++11

CPPFLAGS+=-DMICROPROFILE_GPU_TIMERS=0
#CPPFLAGS+=-DJQ_MICROPROFILE




CPP_SOURCES = ../microprofile/microprofile.cpp ../jqpipe.cpp  ../jqinternal.cpp
DEMO_SOURCES = demo_stress.cpp demo_priority.cpp demo_cancel.cpp
JQ_SOURCES=../jqlocked.cpp ../jqmultilocked.cpp
C_SOURCES =
ASM_SOURCES = ../boost_context/jump_x86_64_sysv_macho_gas.S ../boost_context/make_x86_64_sysv_macho_gas.S


#osx
ifeq ($(UNAME_S),Darwin)
	CPPFLAGS+=-stdlib=libc++ -std=c++11
	CCFLAGS += -D OSX
	ASM_SOURCES = ../boost_context/jump_x86_64_sysv_macho_gas.S ../boost_context/make_x86_64_sysv_macho_gas.S
endif

#linux
ifeq ($(UNAME_S),Linux)
	LDFLAGS += -latomic
	ASM_SOURCES = ../boost_context/jump_x86_64_sysv_elf_gas.S ../boost_context/make_x86_64_sysv_elf_gas.S
	CCFLAGS += LINUX
endif


CC=clang
CPP=clang++
LD=clang++

CPP_OBJS = $(patsubst %.cpp,%.o,$(CPP_SOURCES))
C_OBJS = $(patsubst %.c,%.o,$(C_SOURCES))
ASM_OBJS = $(patsubst %.S,%.o,$(ASM_SOURCES))
OBJS = $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS)
OM = ../jqlocked.o
OMM = ../jqmultilocked.o
O2 = ../jq2.o

default: mm m jq2

ll: demo_priority_ll demo_stress_ll demo_cancel_ll
mm: demo_priority_mm demo_stress_mm demo_cancel_mm demo_job_profile_mm
m: demo_priority_m demo_stress_m demo_cancel_m demo_job_profile_m
jq2: demo_priority_jq2 demo_stress_jq2 demo_cancel_jq2 demo_job_profile_jq2

all: clean default

x: clean demo_stress_ll

#lockless demo


#mutex demo
demo_stress_m: demo_stress.o $(OBJS) $(OM)
	$(LD) -o demo_stress_m demo_stress.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_priority_m: demo_priority.o $(OBJS) $(OM)
	$(LD) -o demo_priority_m demo_priority.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_cancel_m: demo_cancel.o $(OBJS) $(OM)
	$(LD) -o demo_cancel_m demo_cancel.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_job_profile_m: demo_job_profile.o $(OBJS) $(OM)
	$(LD) -o demo_job_profile_m demo_job_profile.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)


#multi mutex demo
demo_stress_mm: demo_stress.o $(OBJS) $(OMM)
	$(LD) -o demo_stress_mm demo_stress.o $(OBJS) $(OMM) $(LDFLAGS) $(CPPFLAGS)

demo_priority_mm: demo_priority.o $(OBJS) $(OMM)
	$(LD) -o demo_priority_mm demo_priority.o $(OBJS) $(OMM) $(LDFLAGS) $(CPPFLAGS)

demo_cancel_mm: demo_cancel.o $(OBJS) $(OMM)
	$(LD) -o demo_cancel_mm demo_cancel.o $(OBJS) $(OMM) $(LDFLAGS) $(CPPFLAGS)

demo_job_profile_mm: demo_job_profile.o $(OBJS) $(OMM)
	$(LD) -o demo_job_profile_mm demo_job_profile.o $(OBJS) $(OMM) $(LDFLAGS) $(CPPFLAGS)


#jq2 demo
demo_stress_jq2: demo_stress.o $(OBJS) $(O2)
	$(LD) -o demo_stress_jq2 demo_stress.o $(OBJS) $(O2) $(LDFLAGS) $(CPPFLAGS)

demo_priority_jq2: demo_priority.o $(OBJS) $(O2)
	$(LD) -o demo_priority_jq2 demo_priority.o $(OBJS) $(O2) $(LDFLAGS) $(CPPFLAGS)

demo_cancel_jq2: demo_cancel.o $(OBJS) $(O2)
	$(LD) -o demo_cancel_jq2 demo_cancel.o $(OBJS) $(O2) $(LDFLAGS) $(CPPFLAGS)

demo_job_profile_jq2: demo_job_profile.o $(OBJS) $(O2)
	$(LD) -o demo_job_profile_jq2 demo_job_profile.o $(OBJS) $(OMM) $(LDFLAGS) $(CPPFLAGS)


-include .depend

.cpp.o:
	$(CPP) -c $< $(CPPFLAGS) -o $@

.c.o:
	$(CC) -c $< $(CFLAGS) -o $@

.S.o:
	$(CC) -c $< -o $@

remove_files:
	rm -f *.o ../*.o ../microprofile/*.o demo_priority_mm demo_stress_mm demo_cancel_mm .depend


clean: remove_files depend


depend: $(CPP_SOURCES) $(C_SOURCES) $(DEMO_SOURCES) $(JQ_SOURCES)
	$(CPP) -MM $(CPPFLAGS) $(CPP_SOURCES) $(DEMO_SOURCES) $(JQ_SOURCES) > .depend