CC ?= gcc
CFLAGS ?= -O3 -march=native -Wall -Wextra -pthread $(shell pkg-config --cflags gtk+-3.0 json-c)
LIBS = $(shell pkg-config --libs gtk+-3.0 json-c) -lm -lpthread -ldl

TARGET = macro_app

all: $(TARGET)

$(TARGET): macro_app.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
