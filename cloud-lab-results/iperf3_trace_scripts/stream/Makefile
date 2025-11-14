CC = gcc
CFLAGS = -O3 -msse4 -mavx512f -g -march=native -funroll-loops

all: stream

stream:
	$(CC) $(CFLAGS) -DNTIMES=1000 -DSTREAM_ARRAY_SIZE=134217728 stream.c -o stream

clean:
	rm -f stream stream_f.exe stream_c.exe *.o
