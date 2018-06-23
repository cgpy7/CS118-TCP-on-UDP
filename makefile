servermake: server.cpp
	g++ -std=c++11 server.cpp -o Server
	g++ -std=c++11 temp_client.cpp -o Client -w

tempc: temp_client.cpp
	g++ -std=c++11 temp_client.cpp -o Client -w