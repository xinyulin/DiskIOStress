CC=gcc
CFLAGS=
LIBS=-lpthread
OBJ = DiskIOStress.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

DiskIOStress: $(OBJ)
	gcc -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f *.o *~ .c~ *.dat
