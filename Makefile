CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread
TARGET = dat_tool
OBJS = main.o extract.o record.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c dat_tool.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
