#include <iostream>
#include <string>
#include <thread>
#include <atomic>     // <<< Include for std::atomic
#include <cstring>    // For memset, strerror
#include <cerrno>     // For errno
#include <vector>     // <<< Added for clearing line buffer

// Platform specific includes and definitions
#ifdef _WIN32
    // Define minimum Windows version (_WIN32_WINNT) for modern Winsock functions like inet_pton
    // Needs to be defined before including winsock2.h or ws2tcpip.h
    #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600 // Target Windows Vista / Server 2008 or later
    #endif

    #define _WINSOCK_DEPRECATED_NO_WARNINGS // Suppress warnings for older Winsock functions
    #include <winsock2.h>
    #include <ws2tcpip.h> // Include for inet_pton and other TCP/IP functions
    #pragma comment(lib, "ws2_32.lib") // Link with Winsock library
    using socket_t = SOCKET;
    using socklen_t = int;
    #define close_socket(s) closesocket(s)
    #define socket_error() WSAGetLastError()
    #define SHUT_WR SD_SEND // Define POSIX value for Windows shutdown()
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
    #include <arpa/inet.h>  // Include for inet_pton
    #include <unistd.h>     // For close(), shutdown()
    #include <netdb.h>      // For gethostbyname (though not used directly here)
    #include <sys/ioctl.h>  // For FIONREAD (optional, for smarter buffer handling)
    using socket_t = int;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    #define close_socket(s) close(s)
    #define socket_error() errno
    // SHUT_WR is defined in sys/socket.h on POSIX
    inline void init_sockets() {} // No-op on POSIX
    inline void cleanup_sockets() {} // No-op on POSIX
#endif

const int BUFFER_SIZE = 4096;
std::atomic<bool> should_exit(false); // Atomic flag to signal threads to exit gracefully

// Function to clear the current input line in the console
void clear_current_line() {
    // Use carriage return (\r) to move cursor to beginning, then overwrite with spaces
    // Adjust the number of spaces if your prompt/messages are wider
    std::cout << "\r" << std::string(80, ' ') << "\r";
}

// Function to display the input prompt
void display_prompt() {
    std::cout << "Enter message (/quit to exit): " << std::flush;
}


// Function to receive messages from the server
void receive_messages(socket_t sock) {
    char buffer[BUFFER_SIZE];
    int bytes_received;

    while (!should_exit.load()) { // Check the atomic flag
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0'; // Null-terminate

            clear_current_line(); // Clear the prompt line
            std::cout << buffer << std::endl; // Print the received message
            display_prompt(); // Display the prompt again

        } else if (bytes_received == 0) {
            // Server closed the connection gracefully
            clear_current_line();
            std::cout << "Server disconnected." << std::endl;
            should_exit = true; // Signal the sender thread to exit
            break; // Exit the loop
        } else { // bytes_received == SOCKET_ERROR
            // Check if the error is due to the socket being closed intentionally by this client
            if (should_exit.load()) {
                break; // Exit gracefully if we initiated the shutdown
            }

            // Otherwise, it's a real receive error
            clear_current_line();
            std::cerr << "Receive failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
            should_exit = true; // Signal the sender thread to exit
            break; // Exit the loop
        }
    }
     // Optional: Indicate thread exit
     // std::cout << "[Receive thread exiting]" << std::endl;
}

// Function to read user input and send it to the server
void send_messages(socket_t sock) {
    std::string message;
    while (!should_exit.load()) { // Check the atomic flag
        display_prompt(); // Display prompt before waiting for input

        // Read a whole line from standard input
        if (!std::getline(std::cin, message)) {
             // This can happen on EOF (e.g., Ctrl+D on Linux, Ctrl+Z then Enter on Windows)
             // or other input errors. Treat as wanting to quit.
             if (std::cin.eof()) {
                 std::cout << "\nInput stream closed (EOF). Quitting..." << std::endl;
             } else {
                  std::cerr << "\nInput error. Quitting..." << std::endl;
             }
             should_exit = true; // Signal receiver thread
             break;
        }


        if (should_exit.load()) break; // Check again after blocking getline

        if (message == "/quit") {
            should_exit = true; // Signal receiver thread
            break; // Exit sender loop
        }

        if (message.empty()) {
            continue; // Don't send empty messages
        }

        // Send the message to the server
        if (send(sock, message.c_str(), message.length(), 0) == SOCKET_ERROR) {
            // Check if the error happened because the receiver thread detected a disconnect
             if (should_exit.load()) {
                 break; // Exit gracefully
             }
             // Otherwise, it's a real send error
            std::cerr << "\nSend failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
            should_exit = true; // Signal receiver thread
            break; // Exit sender loop
        }
    }

    // --- Signal that we are done sending ---
    // This helps the receiver thread wake up if it's blocked in recv()
    // when the user types /quit or input fails.
    // It tells the server we won't send more data, potentially causing server's recv to return 0.
    if (sock != INVALID_SOCKET) {
        shutdown(sock, SHUT_WR); // Shut down the sending side of the socket
    }

     // Optional: Indicate thread exit
     // std::cout << "[Send thread exiting]" << std::endl;
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
    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid port number: " << argv[2] << std::endl;
        return EXIT_FAILURE;
    } catch (const std::out_of_range& e) {
         std::cerr << "Port number out of range: " << argv[2] << std::endl;
        return EXIT_FAILURE;
    }

    if (server_port <= 0 || server_port > 65535) {
         std::cerr << "Port number out of valid range (1-65535): " << server_port << std::endl;
         return EXIT_FAILURE;
    }


    init_sockets(); // Initialize Winsock on Windows

    socket_t sock = INVALID_SOCKET;
    struct sockaddr_in serv_addr;

    // 1. Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        cleanup_sockets();
        return EXIT_FAILURE;
    }
     std::cout << "Socket created." << std::endl;


    // 2. Prepare the server address structure
    memset(&serv_addr, 0, sizeof(serv_addr)); // Zero out structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port); // Convert port to network byte order

    // 3. Convert IP address from text to binary form (using inet_pton for compatibility)
    int pton_ret = inet_pton(AF_INET, server_ip_str, &serv_addr.sin_addr);
    if (pton_ret <= 0) {
        if (pton_ret == 0) {
             std::cerr << "Invalid IP address format: " << server_ip_str << std::endl;
        } else { // < 0
             std::cerr << "inet_pton failed. Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        }
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }


    // 4. Connect to server
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        std::cerr << "Connection Failed to " << server_ip_str << ":" << server_port
                  << ". Error: " << socket_error() << " (" << strerror(socket_error()) << ")" << std::endl;
        close_socket(sock);
        cleanup_sockets();
        return EXIT_FAILURE;
    }

    std::cout << "Successfully connected to server " << server_ip_str << ":" << server_port << std::endl;

    // --- Start communication threads ---
    std::thread receiver_thread;
    std::thread sender_thread;

    try {
        // Start receiver thread first
        receiver_thread = std::thread(receive_messages, sock);

        // Start sender thread
        sender_thread = std::thread(send_messages, sock);
    } catch (const std::system_error& e) {
         std::cerr << "Failed to create communication threads: " << e.what() << std::endl;
         should_exit = true; // Signal any potentially created thread to exit
         // Attempt to join any threads that might have been created before the exception
         if (sender_thread.joinable()) sender_thread.join();
         if (receiver_thread.joinable()) receiver_thread.join();
         close_socket(sock);
         cleanup_sockets();
         return EXIT_FAILURE;
    }


    // --- Wait for threads to finish ---
    // The sender thread will finish when the user types /quit or input fails.
    // It will then signal the receiver thread via should_exit and shutdown(SHUT_WR).
    if (sender_thread.joinable()) {
        sender_thread.join();
    }

    // The receiver thread will finish when should_exit is true (signaled by sender or network error)
    // or when recv returns 0 (server disconnect) or SOCKET_ERROR.
    if (receiver_thread.joinable()) {
         receiver_thread.join();
    }

    // --- Cleanup ---
    std::cout << "Closing socket..." << std::endl;
    if (sock != INVALID_SOCKET) {
        close_socket(sock);
        sock = INVALID_SOCKET; // Mark as closed
    }
    cleanup_sockets(); // Cleanup Winsock on Windows

    std::cout << "Exiting application." << std::endl;
    return 0;
}
