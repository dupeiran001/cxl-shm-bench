CC = gcc
CFLAGS = -O3 -march=native -Wall -Wextra -g -static -mclflushopt
LDFLAGS = -pthread -lrt

TARGET = lock_bench
SRC = lock_bench.c lfm_lock.c peterson_lock.c bakery_lock.c centralized_lock.c spin_lock.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
