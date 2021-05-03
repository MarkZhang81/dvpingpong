CC := gcc
LD := gcc
CFLAGS := -Wall -g -O0 -I.

LIBS :=  -libverbs -lmlx5  -lpthread

HEADERS := pp_common.h
OBJS_VERB := sock.o pp_common.o pp_verb.o
OBJS_DV := sock.o pp_common.o pp_dv.o
OBJS_VFIO := sock.o pp_common.o pp_dv.o pp_vfio.o

all: server client.verb client.dv client.vfio

server: server.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

client.verb: client.verb.o $(OBJS_VERB)
	$(LD) -o $@ $^ $(LIBS)

client.dv: client.dv.o $(OBJS_DV)
	$(LD) -o $@ $^ $(LIBS)

client.vfio: client.vfio.o $(OBJS_VFIO)
	$(LD) -o $@ $^ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o server client.verb client.dv client.vfio
