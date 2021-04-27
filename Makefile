CC = gcc
LDLIBS = -lrt -lpthread

%.o: %.c
	$(CC) -c $<
	# For debugging
	# $(CC) -c -g $<

all: miner

miner: miner.o
	gcc  $< $(LDLIBS) -o $@

clean:
	rm -f *.o miner
