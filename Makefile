CC	= gcc
CFLAGS	= -O0 -Wall
PROGRAM	= DTM

all: $(PROGRAM)
$(PROGRAM): DistributedTransparentMemory.c
	$(CC) $(CFLAGS) DistributedTransparentMemory.c -o $(PROGRAM)

.PHONY: clean
clean:
	rm $(PROGRAM)
