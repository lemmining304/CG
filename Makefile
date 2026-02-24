CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

TARGET := cg
SRC := src/main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET)
