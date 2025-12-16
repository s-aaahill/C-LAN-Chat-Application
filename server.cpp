#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <map>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cstdlib>

// Platform specific includes and definitions
#ifdef _WIN32

    #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600 // Target Windows Vista / Server 2008 or later
    #endif

    #define _WINSOCK_DEPRECATED_NO_WARNINGS // Suppress warnings for older Winsock functions
    #include <winsock2.h>
    #include <ws2tcpip.h> // Include for inet_ntop and other TCP/IP functions
    #pragma comment(lib, "ws2_32.lib") // Link with Winsock library
    using socket_t = SOCKET;
    using socklen_t = int;
    #define close_socket(s) closesocket(s)
    #define socket_error() WSAGetLastError()
    inline void init_sockets() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed. Error Code: " << socket_error() << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    inline void cleanup_sockets() { WSACleanup(); }
#else // Linux, macOS, etc. (POSIX)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    using socket_t = int;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    #define close_socket(s) close(s)
    #define socket_error() errno
    inline void init_sockets() {} // No-op on POSIX
    inline void cleanup_sockets() {} // No-op on POSIX
#endif

int PORT = 8080; // Changed to non-const to allow modification from env
const int BUFFER_SIZE = 4096;
const int MAX_CLIENTS = 10;

// Map to store client sockets and their usernames
std::map<socket_t, std::string> clients;
std::mutex clients_mutex;

// Helper function for timestamped logging
void log_event(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
    #ifdef _WIN32
        localtime_s(&now_tm, &now_c);
    #else
        localtime_r(&now_c, &now_tm);
    #endif
    
    std::cout << "[" << std::put_time(&now_tm, "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
}

// Function to broadcast a message to all clients except the sender
void broadcast_message(const std::string& message, socket_t sender_socket = INVALID_SOCKET) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (const auto& pair : clients) {
        socket_t client_socket = pair.first;
        if (client_socket != sender_socket && client_socket != INVALID_SOCKET) {
            if (send(client_socket, message.c_str(), message.length(), 0) == SOCKET_ERROR) {
                // Log error but don't spam stderr too much
            }
        }
    }
}

// Function to handle communication with a single client
void handle_client(socket_t client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_received;
    std::string username;

    // 1. Receive Username
    bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        username = std::string(buffer);
        // Trim whitespace
        size_t last_char = username.find_last_not_of(" \n\r\t");
        if (last_char != std::string::npos) {
            username = username.substr(0, last_char + 1);
        } else {
            username = "Anonymous";
        }
        if (username.empty()) username = "Anonymous";
    } else {
        close_socket(client_socket);
        return;
    }

    // 2. Register Client
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        
        // Handle duplicate names
        std::string final_username = username;
        int count = 1;
        bool exists = true;
        while (exists) {
            exists = false;
            for (const auto& pair : clients) {
                if (pair.second == final_username) {
                    exists = true;
                    final_username = username + "_" + std::to_string(count++);
                    break;
                }
            }
        }
        username = final_username;
        clients[client_socket] = username;
    }

    log_event("User connected: " + username + " (Socket: " + std::to_string(client_socket) + ")");
    std::string welcome_msg = "Welcome, " + username + "!\n";
    send(client_socket, welcome_msg.c_str(), welcome_msg.length(), 0);
    broadcast_message(username + " has joined the chat.", client_socket);

    // 3. Message Loop
    while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        std::string message(buffer);
        
        // Trim
       size_t last_char = message.find_last_not_of(" \n\r\t");
        if (last_char != std::string::npos) {
            message = message.substr(0, last_char + 1);
        } else {
            message = "";
        }

        if (message.empty()) continue;

        log_event("Message from " + username + ": " + message);
        
        std::string broadcast_msg = "[" + username + "]: " + message;
        broadcast_message(broadcast_msg, client_socket);
    }

    // 4. Disconnect
    log_event("User disconnected: " + username);
    broadcast_message(username + " has left the chat.", client_socket);

    // Clean up
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.erase(client_socket);
    }
    
    close_socket(client_socket);
}

int main() {
    init_sockets();

    // 1. Configuration from Env
    const char* env_port = std::getenv("PORT");
    if (env_port) {
        try {
            int p = std::stoi(env_port);
            if (p > 0 && p <= 65535) PORT = p;
            else std::cerr << "Invalid PORT environment variable. Using default 8080." << std::endl;
        } catch (...) {
            std::cerr << "Invalid PORT format. Using default 8080." << std::endl;
        }
    }

    socket_t server_fd = INVALID_SOCKET;
    struct sockaddr_in server_address;

    // 2. Create Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    int opt = 1;
    #ifdef _WIN32
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
             // warning
        }
    #else
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            // warning
        }
    #endif

    // 3. Bind
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        close_socket(server_fd);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    // 4. Listen
    if (listen(server_fd, MAX_CLIENTS) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        close_socket(server_fd);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    log_event("Server started on port " + std::to_string(PORT));

    // 5. Accept Loop
    while (true) {
        struct sockaddr_in client_address;
        socklen_t client_addr_len = sizeof(client_address);
        socket_t client_socket = accept(server_fd, (struct sockaddr*)&client_address, &client_addr_len);

        if (client_socket == INVALID_SOCKET) {
            continue; 
        }
        
        // Check capacity
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.size() < MAX_CLIENTS) {
                accepted = true;
                // Temporarily add with placeholder until thread updates it
                clients[client_socket] = "Connecting..."; 
            }
        }

        if (accepted) {
            std::thread(handle_client, client_socket).detach();
        } else {
             const char* msg = "Server full.\n";
             send(client_socket, msg, strlen(msg), 0);
             close_socket(client_socket);
        }
    }

    if (server_fd != INVALID_SOCKET) close_socket(server_fd);
    cleanup_sockets();
    return 0;
}
