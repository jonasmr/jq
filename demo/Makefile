
LDFLAGS=-framework OpenGL `sdl2-config --static-libs` -lpthread -lcurses 

CFLAGS=`sdl2-config --cflags` -I.. -Iglew -I. -DGLEW_STATIC -Wno-c++11-extensions -Wall
#CFLAGS+=-fsanitize=undefined-trap -fsanitize-undefined-trap-on-error
CFLAGS+=-g -O3 -Wno-invalid-offsetof 

CPPFLAGS=$(CFLAGS)
CPPFLAGS+=-stdlib=libc++ -std=c++11
CPPFLAGS+=-DMICROPROFILE_GPU_TIMERS=0




CPP_SOURCES = microprofile.cpp ../jqpipe.cpp  ../jqinternal.cpp ../jqnode.cpp
DEMO_SOURCES = demo_stress.cpp demo_priority.cpp demo_node.cpp demo_cancel.cpp
JQ_SOURCES=../jqlockless.cpp ../jqmutex.cpp
C_SOURCES =
ASM_SOURCES = ../boost_context/jump_x86_64_sysv_macho_gas.S ../boost_context/make_x86_64_sysv_macho_gas.S

CC=clang
CPP=clang++
LD=clang++

CPP_OBJS = $(patsubst %.cpp,%.o,$(CPP_SOURCES))
C_OBJS = $(patsubst %.c,%.o,$(C_SOURCES))
ASM_OBJS = $(patsubst %.S,%.o,$(ASM_SOURCES))
OBJS = $(C_OBJS) $(CPP_OBJS) $(ASM_OBJS)
OLL = ../jqlockless.o
OM = ../jqmutex.o

default: ll m

ll: demo_priority_ll demo_stress_ll demo_node_ll demo_cancel_ll

m: demo_priority_m demo_stress_m demo_node_m demo_cancel_m

all: clean default 

x: clean demo_stress_ll

#lockless demo
demo_stress_ll: demo_stress.o $(OBJS) $(OLL)
	$(LD) -o demo_stress_ll demo_stress.o $(OBJS) $(OLL) $(LDFLAGS) $(CPPFLAGS)

demo_priority_ll: demo_priority.o $(OBJS) $(OLL)
	$(LD) -o demo_priority_ll demo_priority.o $(OBJS) $(OLL) $(LDFLAGS) $(CPPFLAGS)

demo_node_ll: demo_node.o $(OBJS) $(OLL)
	$(LD) -o demo_node_ll demo_node.o $(OBJS) $(OLL) $(LDFLAGS) $(CPPFLAGS)

demo_cancel_ll: demo_cancel.o $(OBJS) $(OLL)
	$(LD) -o demo_cancel_ll demo_cancel.o $(OBJS) $(OLL) $(LDFLAGS) $(CPPFLAGS)

#mutex demo
demo_stress_m: demo_stress.o $(OBJS) $(OM)
	$(LD) -o demo_stress_m demo_stress.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_priority_m: demo_priority.o $(OBJS) $(OM)
	$(LD) -o demo_priority_m demo_priority.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_node_m: demo_node.o $(OBJS) $(OM)
	$(LD) -o demo_node_m demo_node.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)

demo_cancel_m: demo_cancel.o $(OBJS) $(OM)
	$(LD) -o demo_cancel_m demo_cancel.o $(OBJS) $(OM) $(LDFLAGS) $(CPPFLAGS)



-include .depend

.cpp.o: 
	$(CPP) -c $< $(CPPFLAGS) -o $@

.c.o:
	$(CC) -c $< $(CFLAGS) -o $@

.S.o:
	$(CC) -c $< -o $@

remove_files:
	rm -f *.o ../*.o demo_stress_ll demo_priority_ll demo_node_ll demo_cancel_ll demo_stress_m demo_priority_m demo_node_m demo_cancel_m .depend


clean: remove_files depend
	

depend: $(CPP_SOURCES) $(C_SOURCES) $(DEMO_SOURCES) $(JQ_SOURCES)
	$(CPP) -MM $(CPPFLAGS) $(CPP_SOURCES) $(DEMO_SOURCES) $(JQ_SOURCES) > .depend