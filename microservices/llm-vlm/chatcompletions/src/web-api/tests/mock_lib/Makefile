
CC = gcc
CFLAGS = -fPIC -Wall
LDFLAGS = -shared
TARGET = libllmservice.so
SRC = llm-buffer.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
