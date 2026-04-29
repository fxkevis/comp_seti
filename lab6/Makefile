TARGETS   = server client
CXXFLAGS  = -std=c++11 -Wall

all: $(TARGETS)

server: server.cpp
	g++ $(CXXFLAGS) server.cpp -o server -lpthread

client: client.cpp
	g++ $(CXXFLAGS) client.cpp -o client -lpthread -lm

clean:
	rm -f $(TARGETS) *.o chat_history.json net_diag_*.json
