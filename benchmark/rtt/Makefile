# Makefile for RTT latency benchmark

CC = gcc
CFLAGS = -O3 -march=native -Wall -Wextra -g -static -mclflushopt
LDFLAGS = -lrt -pthread

TARGET = rtt

.PHONY: all clean

all: $(TARGET)

$(TARGET): rtt.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
