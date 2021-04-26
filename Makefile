CC := gcc
LD := gcc
CFLAGS := -Wall -g -O0 -I.

LIBS :=  -libverbs -lmlx5  -lpthread

HEADERS := pp_common.h
OBJS_VERB := sock.o pp_common.o pp_verb.o

all: server client.verb

server: server.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

client.verb: client.verb.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client.verb *.o
