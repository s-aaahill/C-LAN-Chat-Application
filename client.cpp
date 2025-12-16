#include <iostream>
#include <string>
#include <thread>
#include <atomic>     
#include <cstring>    
#include <cerrno>     
#include <vector>     

// Platform specific includes and definitions
#ifdef _WIN32
    #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600 
    #endif

    #define _WINSOCK_DEPRECATED_NO_WARNINGS 
    #include <winsock2.h>
    #include <ws2tcpip.h> 
    #pragma comment(lib, "ws2_32.lib") 
    using socket_t = SOCKET;
    using socklen_t = int;
    #define close_socket(s) closesocket(s)
    #define socket_error() WSAGetLastError()
    #define SHUT_WR SD_SEND 
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
    #include <sys/ioctl.h>  
    using socket_t = int;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    #define close_socket(s) close(s)
    #define socket_error() errno
    inline void init_sockets() {} 
    inline void cleanup_sockets() {} 
#endif

const int BUFFER_SIZE = 4096;
std::atomic<bool> should_exit(false); 

void clear_current_line() {
    std::cout << "\r" << std::string(80, ' ') << "\r";
}

void display_prompt() {
    std::cout << "Enter message (/quit to exit): " << std::flush;
}

void receive_messages(socket_t sock) {
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (!should_exit.load()) {
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0'; 

            clear_current_line(); 
            std::cout << buffer << std::endl; 
            display_prompt(); 

        } else if (bytes_received == 0) {
            clear_current_line();
            std::cout << "Server disconnected." << std::endl;
            should_exit = true; 
            break; 
        } else { 
            if (should_exit.load()) {
                break; 
            }
            clear_current_line();
            std::cerr << "Receive failed." << std::endl;
            should_exit = true; 
            break; 
        }
    }
}

void send_messages(socket_t sock) {
    std::string message;
    while (!should_exit.load()) { 
        display_prompt(); 

        if (!std::getline(std::cin, message)) {
             if (std::cin.eof()) {
                 std::cout << "\nInput stream closed (EOF). Quitting..." << std::endl;
             } else {
                  std::cerr << "\nInput error. Quitting..." << std::endl;
             }
             should_exit = true; 
             break;
        }

        if (should_exit.load()) break; 

        if (message == "/quit") {
            should_exit = true; 
            break; 
        }

        if (message.empty()) {
            continue; 
        }

        if (send(sock, message.c_str(), message.length(), 0) == SOCKET_ERROR) {
             if (should_exit.load()) {
                 break; 
             }
            std::cerr << "\nSend failed." << std::endl;
            should_exit = true; 
            break; 
        }
    }

    if (sock != INVALID_SOCKET) {
        shutdown(sock, SHUT_WR); 
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <server_port>" << std::endl;
        return EXIT_FAILURE;
    }

    const char* server_ip_str = argv[1];
    int server_port;
    try {
        server_port = std::stoi(argv[2]);
    } catch (...) {
        std::cerr << "Invalid port number: " << argv[2] << std::endl;
        return EXIT_FAILURE;
    }

    if (server_port <= 0 || server_port > 65535) {
         std::cerr << "Port number out of valid range (1-65535)." << std::endl;
         return EXIT_FAILURE;
    }

    init_sockets(); 

    socket_t sock = INVALID_SOCKET;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    memset(&serv_addr, 0, sizeof(serv_addr)); 
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port); 

    int pton_ret = inet_pton(AF_INET, server_ip_str, &serv_addr.sin_addr);
    if (pton_ret <= 0) {
        std::cerr << "Invalid IP address or inet_pton failed." << std::endl;
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        std::cerr << "Connection Failed." << std::endl;
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    std::cout << "Successfully connected to server." << std::endl;

    // --- Username Step ---
    std::cout << "Enter your username: ";
    std::string username;
    std::getline(std::cin, username);
    if (username.empty()) username = "Guest";
    send(sock, username.c_str(), username.length(), 0);
    // ---------------------

    std::thread receiver_thread;
    std::thread sender_thread;

    try {
        receiver_thread = std::thread(receive_messages, sock);
        sender_thread = std::thread(send_messages, sock);
    } catch (...) {
         std::cerr << "Failed to create threads." << std::endl;
         should_exit = true; 
         if (sender_thread.joinable()) sender_thread.join();
         if (receiver_thread.joinable()) receiver_thread.join();
         close_socket(sock);
         cleanup_sockets();
         return EXIT_FAILURE;
    }

    if (sender_thread.joinable()) sender_thread.join();
    if (receiver_thread.joinable()) receiver_thread.join();

    if (sock != INVALID_SOCKET) {
        close_socket(sock);
    }
    cleanup_sockets(); 

    return 0;
}
