#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
#endif

using json = nlohmann::json;

// Function to send a JSON message over the socket
void sendJsonMessage(int clientSocket, const json& j) {
    std::string message = j.dump();
    message += "\n";
    send(clientSocket, message.c_str(), message.length(), 0);
}

// Thread function to handle incoming messages from the server
void receiveMessages(int clientSocket, std::atomic<bool>& shutdown) {
    char buffer[1024] = {0};
    while (!shutdown) {
        memset(buffer, 0, sizeof(buffer));
        int bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) {
            std::cout << "\n[Server Disconnected] Press Enter to exit." << std::endl;
            shutdown = true;
            break;
        }
        std::string message(buffer, bytesRead);
        std::cout << "\n[Broadcast Message] " << message << std::endl;
        std::cout << "> "; // Reprint the prompt
        std::cout.flush();
    }
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }
#endif

    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) {
        std::cerr << "Socket creation failed." << std::endl;
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(6969);
    #ifdef _WIN32
        InetPton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    #else
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    #endif

    if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Connection failed." << std::endl;
        return 1;
    }

    std::cout << "Connected to C++ broadcast server. You can send commands now." << std::endl;
    std::cout << "Format: x y z value (e.g., 10 20 30 liquid)" << std::endl;
    
    std::atomic<bool> shutdown(false);
    std::thread receiverThread(receiveMessages, clientSocket, std::ref(shutdown));

    // Main thread loop for sending messages
    while (!shutdown) {
        int x, y, z;
        std::string value;

        std::cout << "> ";
        std::cin >> x >> y >> z >> value;
        
        if (std::cin.fail()) {
            std::cout << "Invalid input. Exiting." << std::endl;
            shutdown = true;
            break;
        }

        json blockChangeMessage;
        blockChangeMessage["world"] = 0;
        blockChangeMessage["type"] = "block";
        blockChangeMessage["location"]["x"] = x;
        blockChangeMessage["location"]["y"] = y;
        blockChangeMessage["location"]["z"] = z;
        blockChangeMessage["action"] = "set_state";
        blockChangeMessage["key"] = "";
        blockChangeMessage["value"] = value;

        sendJsonMessage(clientSocket, blockChangeMessage);
    }

    receiverThread.join();

#ifdef _WIN32
    closesocket(clientSocket);
    WSACleanup();
#else
    close(clientSocket);
#endif

    return 0;
}