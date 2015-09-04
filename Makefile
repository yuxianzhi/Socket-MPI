CC=g++
CFLAGS = -c -O3 -Wall
LDFLAGS = 
SOURCES = n.cpp WAVE.cpp
OBJECTS = $(SOURCES:.c=.o)
EXECUTABLE = n


all: $(SOURCES) $(EXECUTABLE)
	rm -rf *.o

$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS) 
.c.o:
	$(CC) $(CFLAGS) $< -o $@ 

clean:
	rm -f *.o *~ \#* $(EXECUTABLE)
