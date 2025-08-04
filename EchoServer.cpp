#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

using json = nlohmann::json;

// Global list of client sockets and a mutex to protect it
std::vector<int> clients;
std::mutex clientsMutex;

// Function to send a JSON message over the socket
void sendJsonMessage(int clientSocket, const json& j) {
    std::string message = j.dump();
    message += "\n";
    send(clientSocket, message.c_str(), message.length(), 0);
}

// Thread function to handle a single client's connection and broadcast messages
void handleClientConnection(int clientSocket) {
    // Add this client to our list of clients
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(clientSocket);
        std::cout << "(Orge) [Echo Server] Client " << clientSocket << " connected. Total clients: " << clients.size() << std::endl;
    }

    char buffer[1024] = {0};
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) {
            // Client disconnected or an error occurred.
            break;
        }

        std::string message(buffer, bytesRead);
        
        // Broadcast the received message to all other clients
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (int otherClientSocket : clients) {
                if (otherClientSocket != clientSocket) {
                    std::cout << "(Orge) [Echo Server] Broadcasting from " << clientSocket << " to " << otherClientSocket << ": " << message << std::endl;
                    send(otherClientSocket, message.c_str(), message.length(), 0);
                }
            }
        }
    }

    // Client disconnected, so remove it from our list
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (*it == clientSocket) {
                clients.erase(it);
                break;
            }
        }
        std::cout << "(Orge) [Echo Server] Client " << clientSocket << " disconnected. Total clients: " << clients.size() << std::endl;
    }

#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "(Orge) [Echo Server] WSAStartup failed." << std::endl;
        return 1;
    }
#endif

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "(Orge) [Echo Server] Socket creation failed." << std::endl;
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(6969);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "(Orge) [Echo Server] Bind failed." << std::endl;
        return 1;
    }

    listen(serverSocket, 5); // Listen for up to 5 pending connections
    std::cout << "(Orge) [Echo Server]  C++ Echo server listening on port 6969..." << std::endl;
    
    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientSize = sizeof(clientAddr);
        int clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientSize);
        if (clientSocket < 0) {
            std::cerr << "(Orge) [Echo Server] Accept failed." << std::endl;
            continue;
        }

        // Spawn a new thread to handle this client
        std::thread clientThread(handleClientConnection, clientSocket);
        clientThread.detach(); // Allow the thread to run independently
    }
    
#ifdef _WIN32
    closesocket(serverSocket);
    WSACleanup();
#else
    close(serverSocket);
#endif

    return 0;
}
//g++ EchoServer.cpp -o EchoServer -lws2_32 -std=c++11 -Isrc/Include