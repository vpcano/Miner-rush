CC = gcc
LDLIBS = -lrt -lpthread

%.o: %.c
	# $(CC) -c $<
	# For debugging
	$(CC) -g -c $<

all: miner

miner: miner.o
	gcc  $< $(LDLIBS) -o $@

clean:
	rm -f *.o miner
