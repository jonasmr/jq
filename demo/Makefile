
LDFLAGS=-framework OpenGL `sdl2-config --static-libs` -lpthread -lcurses 

CFLAGS=`sdl2-config --cflags` -I.. -Iglew -I. -DGLEW_STATIC -Wno-c++11-extensions -Wall
#CFLAGS+=-fsanitize=undefined-trap -fsanitize-undefined-trap-on-error
CFLAGS+=-g -O3 -Wno-invalid-offsetof 

CPPFLAGS=$(CFLAGS)
CPPFLAGS+=-stdlib=libc++ -std=c++11
#CPPFLAGS+=-DJQ_MICROPROFILE
#CPPFLAGS+=-DJQ_MICROPROFILE_VERBOSE
CPPFLAGS+=-DMICROPROFILE_GPU_TIMERS=0
CPPFLAGS+=-DJQ_USE_LOCKLESS
CPPFLAGS+=-DJQ_USE_LOCKLESS
#CPPFLAGS+=-DMICROPROFILE_ENABLED=0
#CPPFLAGS+=-DJQ_ASSERT_LOCKS
#define JQ_IMPL
#define JQ_MICROPROFILE
#define JQ_MICROPROFILE_VERBOSE
#define JQ_ASSERT_LOCKS


CPP_SOURCES = microprofile.cpp ../jqpipe.cpp ../jq1.cpp ../jq2.cpp ../jqinternal.cpp ../jqnode.cpp
C_SOURCES =
ASM_SOURCES = ../boost_context/jump_x86_64_sysv_macho_gas.S ../boost_context/make_x86_64_sysv_macho_gas.S

TARGET=demo
CC=clang
CPP=clang++
LD=clang++

CPP_OBJS = $(patsubst %.cpp,%.o,$(CPP_SOURCES))
C_OBJS = $(patsubst %.c,%.o,$(C_SOURCES))
ASM_OBJS = $(patsubst %.S,%.o,$(ASM_SOURCES))
OBJS = $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS)

default:demo_priority demo_stress demo_node demo_cancel
all: clean default 

demo_stress: demo_stress.o $(OBJS)
	$(LD) -o demo_stress demo_stress.o $(OBJS) $(LDFLAGS) $(CPPFLAGS)

demo_priority: demo_priority.o $(OBJS)
	$(LD) -o demo_priority demo_priority.o $(OBJS) $(LDFLAGS) $(CPPFLAGS)

demo_node: demo_node.o $(OBJS) 
	$(LD) -o demo_node demo_node.o $(OBJS) $(LDFLAGS) $(CPPFLAGS)

demo_cancel: demo_cancel.o $(OBJS)
	$(LD) -o demo_cancel demo_cancel.o $(OBJS) $(LDFLAGS) $(CPPFLAGS)


-include .depend

.cpp.o: 
	$(CPP) -c $< $(CPPFLAGS) -o $@

.c.o:
	$(CC) -c $< $(CFLAGS) -o $@

.S.o:
	$(CC) -c $< -o $@

remove_files:
	rm -f *.o ../*.o demo_stress demo_priority demo_node demo_cancel .depend


clean: remove_files depend
	

depend: $(CPP_SOURCES) $(C_SOURCES)
	rm -f .depend
	$(CPP) -MM $(CPPFLAGS) $(CPP_SOURCES) >> .depend