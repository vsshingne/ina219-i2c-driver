CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

all: ina219_read

ina219_read: ina219_read.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ina219_read
