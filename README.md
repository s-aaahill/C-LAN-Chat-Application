# Dockerized TCP Chat Server

A robust, real-time chat application written in C++ using TCP sockets. This project supports multiple clients, tracks usernames, logs events, and is designed to be easily deployed via Docker.

## Architecture
The application follows a classic **Client-Server** model:
- **Server**: Listens on a specified TCP port (default 8080). It manages connected clients, handles username registration, and broadcasts messages to all other connected users. It uses `std::map` to track sessions and a mutex for thread-safe operations.
- **Client**: Connects to the server IP and port. It runs two threads: one for sending user input and another for receiving messages. Usernames are sent as the initial handshake message.

## Features
- **Custom Usernames**: Clients identify themselves upon connection.
- **Environment Configuration**: Server port configurable via `PORT` environment variable.
- **Server Logging**: Connection, disconnection, and message events are logged to stdout with timestamps.
- **Docker Ready**: Includes Dockerfile for containerized deployment.
- **Cross-Platform Code**: Source compatible with Linux and Windows (using Winsock).

## Building and Running Locally

### Prerequisites
- GCC/G++ Compiler
- Make

### Build
To build both server and client:
```bash
make
```

### Run Server
```bash
./server
# Or specify a custom port (non-Docker way, if modified to take args, but defaults to env/8080)
# To use env var locally on Linux:
PORT=9000 ./server
```

### Run Client
```bash
./client 127.0.0.1 8080
```

## Docker Instructions

### 1. Build the Docker Image
Build the container image using the provided Dockerfile.
```bash
docker build -t cpp-chat-server .
```

### 2. Run the Server Container
Run the server, mapping the port and optionally setting the `PORT` environment variable.

**Default (Port 8080):**
```bash
docker run -d -p 8080:8080 --name chat-server cpp-chat-server
```

**Custom Port (e.g., 5000):**
Notice we map host 5000 to container 5000 and tell the app to listen on 5000.
```bash
docker run -d -p 5000:5000 -e PORT=5000 --name chat-server cpp-chat-server
```

### 3. Connect Clients
Clients connect to the server's IP address and exposed port.
```bash
# Assuming server is on localhost:8080
./client 127.0.0.1 8080
```

## Design Decisions
- **Protocol**: Simple line-based text protocol. First message is username, subsequent messages are chat text.
- **Threading**: One thread per client on the server to ensure blocking I/O doesn't freeze the server. Detached threads are used for simplicity.
- **State Management**: A `std::map` protects client socket-to-username mappings with a `std::mutex` to prevent race conditions during broadcasting and disconnection.
- **Logging**: Standard output logging is used to align with Docker best practices (logs collection drivers).

---
*Author: Systems Programmer*
