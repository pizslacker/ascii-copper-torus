CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lm
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