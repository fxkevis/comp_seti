TARGETS   = server client
CXXFLAGS  = -std=c++11 -Wall -I/opt/homebrew/opt/openssl@3/include
LDFLAGS   = -L/opt/homebrew/opt/openssl@3/lib

all: $(TARGETS)

server: server.cpp
	g++ $(CXXFLAGS) $(LDFLAGS) server.cpp -o server -lpthread -lssl -lcrypto

client: client.cpp
	g++ $(CXXFLAGS) $(LDFLAGS) client.cpp -o client -lpthread -lm -lssl -lcrypto

cert:
	openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=localhost"

clean:
	rm -f $(TARGETS) *.o chat_history.json net_diag_*.json server.crt server.key
