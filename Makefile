CC = gcc
CFLAGS = -Wall -O2
TARGET = dat_tool
OBJS = main.o extract.o record.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c dat_tool.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
