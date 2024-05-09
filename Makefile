CC = gcc
CFLAGS = -Wall -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread -lrt

# 定义源文件和对象文件
SOURCES = main.c \
          list/list.c \
          timer/timerthread.c \
          list/socketqueue.c \
          hashtable/hashtable.c \
          tcppool/tcppool.c \
          threadpool/threadpool.c \
          threadpool/threadpoolmanager.c \
          list/RequestQueue.c \
          socketserver/socketserver.c

OBJECTS = $(SOURCES:.c=.o)

# 可执行文件
EXECUTABLE = WebServer

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 为每个源文件指定其头文件依赖
main.o: list/list.h timer/timerthread.h list/socketqueue.h hashtable/hashtable.h tcppool/tcppool.h threadpool/threadpool.h threadpool/threadpoolmanager.h list/RequestQueue.h socketserver/socketserver.h
list/list.o: list/list.h
timer/timerthread.o: timer/timerthread.h
list/socketqueue.o: list/socketqueue.h
hashtable/hashtable.o: hashtable/hashtable.h
tcppool/tcppool.o: tcppool/tcppool.h
threadpool/threadpool.o: threadpool/threadpool.h
threadpool/threadpoolmanager.o: threadpool/threadpoolmanager.h
list/RequestQueue.o: list/RequestQueue.h
socketserver/socketserver.o: socketserver/socketserver.h

# 编译规则
.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

.PHONY: all clean
