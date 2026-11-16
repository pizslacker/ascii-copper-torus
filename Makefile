CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libxmp libpulse-simple 2>/dev/null)
LDFLAGS = $(shell pkg-config --libs libxmp libpulse-simple 2>/dev/null || echo "-lxmp -lpulse-simple -lpulse") -lm -lpthread
SRC = ascii-copper-torus.c
BIN = ascii-copper-torus

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)