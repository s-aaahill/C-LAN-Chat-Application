#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>      // <<< Added for std::mutex and std::lock_guard
#include <algorithm>
#include <cstring>    // For memset, strerror
#include <cerrno>     // For errno

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
    #include <arpa/inet.h>  // Include for inet_ntop
    #include <unistd.h>     // For close()
    #include <netdb.h>      // For gethostbyname (though not used directly here)
    using socket_t = int;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    #define close_socket(s) close(s)
    #define socket_error() errno
    inline void init_sockets() {} // No-op on POSIX
    inline void cleanup_sockets() {} // No-op on POSIX
#endif

const int PORT = 8080;
const int BUFFER_SIZE = 4096;
const int MAX_CLIENTS = 10;

std::vector<socket_t> clients;
std::mutex clients_mutex; // Global mutex to protect the clients vector

// Function to broadcast a message to all clients except the sender
void broadcast_message(const std::string& message, socket_t sender_socket) {
    std::lock_guard<std::mutex> lock(clients_mutex); // Lock the mutex
    for (socket_t client_socket : clients) {
        if (client_socket != sender_socket && client_socket != INVALID_SOCKET) { // Check for valid socket
            if (send(client_socket, message.c_str(), message.length(), 0) == SOCKET_ERROR) {
                // Log error but don't necessarily remove client here, recv error will handle removal
                std::cerr << "Send failed to client " << client_socket << ". Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
            }
        }
    }
    // Mutex is automatically unlocked when 'lock' goes out of scope
}

// Function to handle communication with a single client
void handle_client(socket_t client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_received;
    std::string client_id = "[Client " + std::to_string(client_socket) + "]"; // Simple ID based on socket descriptor

    // Announce new client connection
    std::cout << "Connection established for " << client_id << "." << std::endl;
    broadcast_message(client_id + " has joined the chat.", client_socket); // Announce to others

    // Loop to receive data from the client
    while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate the received string
        std::string message(buffer);

        // Trim potential newline characters sent by some telnet clients etc.
        message.erase(message.find_last_not_of(" \n\r\t")+1);

        if (message.empty()) continue; // Ignore empty messages

        std::cout << "Received from " << client_id << ": " << message << std::endl;

        // Prepare message for broadcast
        std::string broadcast_msg = client_id + ": " + message;
        broadcast_message(broadcast_msg, client_socket);
    }

    // Handle disconnection or error after the loop exits
    if (bytes_received == 0) {
        std::cout << client_id << " disconnected gracefully." << std::endl;
        broadcast_message(client_id + " has left the chat.", client_socket);
    } else { // bytes_received == SOCKET_ERROR
        std::cerr << "Recv failed for " << client_id << ". Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
         broadcast_message(client_id + " connection lost.", client_socket);
    }

    // Clean up: Remove client from the list and close socket
    {
        std::lock_guard<std::mutex> lock(clients_mutex); // Lock for safe removal
        auto it = std::find(clients.begin(), clients.end(), client_socket);
        if (it != clients.end()) {
            clients.erase(it);
        }
    } // Mutex unlocked here

    close_socket(client_socket);
    std::cout << "Closed socket for " << client_id << "." << std::endl;
}

int main() {
    init_sockets(); // Initialize Winsock on Windows

    socket_t server_fd = INVALID_SOCKET;
    struct sockaddr_in server_address;

    // 1. Create socket file descriptor
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        cleanup_sockets();
        return EXIT_FAILURE;
    }
    std::cout << "Socket created successfully." << std::endl;


    // --- Optional: Set socket options (allow address reuse) ---
    // This helps prevent "Address already in use" errors if the server restarts quickly
    int opt = 1;
    #ifdef _WIN32
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
            std::cerr << "setsockopt(SO_REUSEADDR) failed. Error: " << socket_error() << std::endl;
            // Optional: Decide if this is fatal or just a warning
        }
    #else // POSIX
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt(SO_REUSEADDR) failed");
            // Optional: Decide if this is fatal or just a warning
        }
    #endif
    //-----------------------------------------------------------


    // 2. Prepare the sockaddr_in structure
    memset(&server_address, 0, sizeof(server_address)); // Zero out the structure
    server_address.sin_family = AF_INET;                // IPv4 Address family
    server_address.sin_addr.s_addr = INADDR_ANY;        // Listen on all available network interfaces
    server_address.sin_port = htons(PORT);              // Convert port number to network byte order

    // 3. Bind the socket to the network address and port
    if (bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        std::cerr << "Bind failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        close_socket(server_fd);
        cleanup_sockets();
        return EXIT_FAILURE;
    }
    std::cout << "Socket bound to port " << PORT << "." << std::endl;

    // 4. Listen for incoming connections
    if (listen(server_fd, MAX_CLIENTS) == SOCKET_ERROR) {
        std::cerr << "Listen failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        close_socket(server_fd);
        cleanup_sockets();
        return EXIT_FAILURE;
    }
    std::cout << "Server listening on port " << PORT << "..." << std::endl;

    // 5. Accept incoming connections in a loop
    while (true) {
        struct sockaddr_in client_address;
        socklen_t client_addr_len = sizeof(client_address);
        socket_t client_socket = INVALID_SOCKET;

        // Accept a client connection (blocking call)
        client_socket = accept(server_fd, (struct sockaddr*)&client_address, &client_addr_len);

        if (client_socket == INVALID_SOCKET) {
            std::cerr << "Accept failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
            // Consider more robust error handling here. E.g., EAGAIN/EWOULDBLOCK are non-fatal
            if (socket_error() == EINTR) continue; // Interrupted system call, try again
            // For other errors, maybe log and continue accepting? Or break if fatal?
            continue; // Continue accepting other clients for now
        }

        // Get client IP address for logging
        char client_ip[INET_ADDRSTRLEN]; // Buffer for IPv4 or IPv6 string
        if (inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN) != NULL) {
             std::cout << "Connection accepted from " << client_ip << ":" << ntohs(client_address.sin_port) << " [Socket: " << client_socket << "]" << std::endl;
        } else {
             std::cerr << "inet_ntop failed for incoming connection. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
             std::cout << "Connection accepted from unknown IP on socket " << client_socket << std::endl;
        }


        // Add new client to the list (thread-safe) and check capacity
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.size() >= MAX_CLIENTS) {
                std::cerr << "Max clients (" << MAX_CLIENTS << ") reached. Connection rejected for socket " << client_socket << "." << std::endl;
                const char* msg = "Server is full. Please try again later.\n";
                send(client_socket, msg, strlen(msg), 0);
                close_socket(client_socket);
                continue; // Go back to accept loop
            }
             clients.push_back(client_socket);
        } // Mutex unlocked here

        try {
             std::thread client_thread(handle_client, client_socket);
             client_thread.detach(); // Allow thread to run independently
        } catch (const std::system_error& e) {
             std::cerr << "Failed to create thread for client " << client_socket << ": " << e.what() << std::endl;
             // Clean up the client we couldn't create a thread for
              {
                  std::lock_guard<std::mutex> lock(clients_mutex);
                  auto it = std::find(clients.begin(), clients.end(), client_socket);
                  if (it != clients.end()) {
                      clients.erase(it);
                  }
              }
             close_socket(client_socket);
        }
    }

    std::cout << "Shutting down server..." << std::endl;

    // Close all remaining client sockets
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (socket_t client_socket : clients) {
             if (client_socket != INVALID_SOCKET) {
                close_socket(client_socket);
             }
        }
        clients.clear();
    }

    // Close the listening server socket
    if (server_fd != INVALID_SOCKET) {
        close_socket(server_fd);
    }

    cleanup_sockets(); // Cleanup Winsock on Windows

    return 0;
}
