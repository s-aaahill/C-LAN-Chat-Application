CXX = g++
CXXFLAGS = -std=c++11 -Wall -pthread

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client server.exe client.exe
