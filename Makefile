CC := cc
CFLAGS := -I/usr/include/libxml2 -O2 -Wall -Wno-dangling-else
LDLIBS := -lm -lcurl -lxml2

OBJS := castotas.o metar.o datetoepoch.o

all: tooclose

tooclose: tooclose.o $(OBJS)

test: tooclose
	nc localhost 30003 | stdbuf -oL tooclose | stdbuf -oL tee test.log

clean:
	rm -f tooclose tooclose.o tb tb.o $(OBJS) test.log

.PHONY: all clean test
