TARGETS = server client

all: $(TARGETS)

server: server.cpp
	g++ server.cpp -o server

client: client.cpp
	g++ client.cpp -o client

clean:
	rm -f $(TARGETS) *.o