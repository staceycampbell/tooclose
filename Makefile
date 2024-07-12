CC := cc
CFLAGS := -I/usr/include/libxml2 -O2 -Wall -Wno-dangling-else -Wno-stringop-truncation -Wno-unknown-warning-option
LDLIBS := -lm -lcurl -lxml2

OBJS := metar.o datetoepoch.o

all: tooclose

tooclose: tooclose.o $(OBJS)

test: tooclose
	nc localhost 30003 | stdbuf -oL tooclose -l | stdbuf -oL tee test.log

clean:
	rm -f tooclose tooclose.o tb tb.o $(OBJS) test.log latestmetar.xml

.PHONY: all clean test
