CC := gcc
LD := gcc
CFLAGS := -Wall -g -O0 -I.

LIBS :=  -libverbs -lmlx5  -lpthread

HEADERS := pp_common.h
OBJS_VERB := sock.o pp_common.o pp_verb.o
OBJS_DV := sock.o pp_common.o pp_dv.o
OBJS_VFIO := sock.o pp_common.o pp_dv.o pp_vfio.o

all: server.raw_wqe client.raw_wqe #server client.verb client.dv client.vfio

server.raw_wqe: server.raw_wqe.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

client.raw_wqe: client.raw_wqe.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o server.raw_wqe client.raw_wqe
