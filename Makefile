CPP=g++
all: server client
server: 
	$(CPP) -pthread -o server server.cpp
client: 
	$(CPP) -pthread -o client client.cpp
clean:
	rm -rf server client