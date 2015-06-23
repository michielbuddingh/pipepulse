CC=gcc
CFLAGS=-Wall -Os --std=gnu99 -Wextra -Werror -Wconversion -Wno-sign-conversion -Wstrict-prototypes -Wunreachable-code -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align -Wcast-qual

FILES = pipepulse
OBJS = $(patsubst %,%.o,$(FILES))
DEPS = $(patsubst %,%.mk,$(FILES))

.SUFFIXES:

%.o: %.c Makefile
	$(CC) $(CFLAGS) -c $<
	$(CC) $(CFLAGS) -MM -MF $(@:.o=.mk) $<

all:	pipepulse pipepulse.1

pipepulse: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

pipepulse.1: README.rst
	rst2man --no-generator $< $@

clean:
	-rm pipepulse
	-rm pipepulse.1
	-$(foreach obj,$(OBJS),rm $(obj);)
	-$(foreach dep,$(DEPS),rm $(dep);)
	-rm *.h.gch
	-rm *.hh.gch

.PHONY:	clean
