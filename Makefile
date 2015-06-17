CC=gcc
CFLAGS=-Wall -Os --std=gnu99 -Wextra

%.o: %.c Makefile
	$(CC) $(CFLAGS) -c $<

pipepulse: pipepulse.o
	$(CC) $(CFLAGS) -o pipepulse pipepulse.o
