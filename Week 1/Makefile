CC = gcc
CFLAGS = -Wall -Wextra -std=c99

TARGET = main
SRCS = main.c account.c logger.c
OBJS = $(SRCS:.c=.o)
HEADERS = account.h logger.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe log_*.txt

.PHONY: all clean run
