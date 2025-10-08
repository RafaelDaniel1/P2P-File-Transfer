// Filename: core_engine.cpp
//
// This program implements the core peer engine for a P2P file transfer application.
// It now supports a two-way file transfer, with full receiving logic.
//
// Features:
// - Sliding window protocol for efficient and reliable file transfer over UDP.
// - A local TCP socket for inter-process communication (IPC) with a Python CLI.
// - Multithreading to handle both network traffic and IPC concurrently.
// - Complete receiving logic to reassemble file chunks and save to disk.
//
// To compile and run:
// 1. Install a C++ compiler (e.g., g++).
// 2. Compile with: g++ core_engine.cpp -o core_engine -std=c++17 -pthread -lws2_32
// 3. Run with: ./core_engine <udp_port> <ipc_port>
// 4. The program will listen on the specified ports for both UDP and local IPC commands.

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>
#include <map>
#include <queue>
#include <chrono>
#include <fstream>
#include <sstream>

// Platform-specific headers for sockets
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    // We will use closesocket() directly on Windows to avoid name clashes
    #define SHUT_WR SD_SEND
    using socklen_t = int;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    // We will use close() directly on non-Windows
#endif

// --- Constants and Configuration ---
const int CHUNK_SIZE = 1024;
const int HEADER_SIZE = 16;
const int MAX_PACKET_SIZE = CHUNK_SIZE + HEADER_SIZE;
const int TIMEOUT_MS = 1000;
const int WINDOW_SIZE = 10;

// Mutex for thread-safe access to shared data
std::mutex g_data_mutex;
bool g_is_running = true;

// --- Custom Protocol Structures ---
enum PacketType {
    DATA,
    ACK,
    SYN,
    FIN
};

struct PacketHeader {
    uint32_t transfer_id;
    uint32_t sequence_num;
    uint32_t chunk_index; // Used for various purposes, e.g., total chunks in SYN
    PacketType type;
};

struct FileTransfer {
    uint32_t transfer_id;
    std::string filename;
    std::string peer_address;
    int peer_port;
    bool is_sending;
    size_t total_chunks;

    // Sender state
    size_t send_window_base;
    size_t next_seq_num;
    std::chrono::steady_clock::time_point last_ack_time;
    std::map<size_t, std::vector<char>> sent_packets;

    // Receiver state
    size_t receive_window_base;
    std::vector<char> received_chunks_buffer; // A simple buffer for incoming chunks
    std::vector<bool> received_status;
};

// Map to store active file transfers
std::map<uint32_t, FileTransfer> g_active_transfers;
uint32_t g_next_transfer_id = 1;

// --- Utility Functions ---
void start_winsock() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        exit(1);
    }
#endif
}

void cleanup_winsock() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// Function to set a socket to non-blocking mode
int set_non_blocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#endif
}

// Sends a single data packet to the peer
void send_data_packet(int udp_socket, const FileTransfer& transfer, size_t chunk_index) {
    std::ifstream file(transfer.filename, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << transfer.filename << std::endl;
        return;
    }

    file.seekg(chunk_index * CHUNK_SIZE);
    std::vector<char> payload(CHUNK_SIZE);
    file.read(payload.data(), CHUNK_SIZE);
    size_t bytes_read = file.gcount();
    
    // Create the packet header
    PacketHeader header;
    header.transfer_id = transfer.transfer_id;
    header.sequence_num = chunk_index; // Use chunk index as sequence number
    header.chunk_index = chunk_index; 
    header.type = DATA;

    std::vector<char> buffer(HEADER_SIZE + bytes_read);
    memcpy(buffer.data(), &header, HEADER_SIZE);
    memcpy(buffer.data() + HEADER_SIZE, payload.data(), bytes_read);

    sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(transfer.peer_port);
    inet_pton(AF_INET, transfer.peer_address.c_str(), &peer_addr.sin_addr);

    sendto(udp_socket, buffer.data(), buffer.size(), 0, (sockaddr*)&peer_addr, sizeof(peer_addr));
}

// IPC handler function
void handle_ipc_connection(int client_socket, int udp_socket) {
    char buffer[1024];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        std::string command(buffer);
        std::cout << "Received IPC command: " << command << std::endl;
        
        if (command.rfind("start_transfer", 0) == 0) {
            std::unique_lock<std::mutex> lock(g_data_mutex);
            std::string args = command.substr(15);
            size_t first_space = args.find(' ');
            std::string peer_addr_port = args.substr(0, first_space);
            std::string filename = args.substr(first_space + 1);

            size_t colon_pos = peer_addr_port.find(':');
            std::string peer_addr = peer_addr_port.substr(0, colon_pos);
            int peer_port = std::stoi(peer_addr_port.substr(colon_pos + 1));
            
            std::ifstream file(filename, std::ios::binary | std::ios::ate);
            if (!file) {
                std::cerr << "File not found: " << filename << std::endl;
                std::string response = "ERROR: File not found.";
                send(client_socket, response.c_str(), response.size(), 0);
                return;
            }
            size_t file_size = file.tellg();
            file.close();

            FileTransfer new_transfer;
            new_transfer.transfer_id = g_next_transfer_id++;
            new_transfer.filename = filename;
            new_transfer.peer_address = peer_addr;
            new_transfer.peer_port = peer_port;
            new_transfer.is_sending = true;
            new_transfer.total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
            new_transfer.send_window_base = 0;
            new_transfer.next_seq_num = 0;
            new_transfer.last_ack_time = std::chrono::steady_clock::now();
            
            g_active_transfers[new_transfer.transfer_id] = new_transfer;
            lock.unlock();

            std::cout << "Starting new transfer with ID: " << new_transfer.transfer_id << std::endl;
            std::string response = "OK: Transfer started with ID " + std::to_string(new_transfer.transfer_id);
            send(client_socket, response.c_str(), response.size(), 0);

        } else if (command == "shutdown") {
            std::unique_lock<std::mutex> lock(g_data_mutex);
            g_is_running = false;
            std::cout << "Shutting down..." << std::endl;
            lock.unlock();
            std::string response = "OK: Shutting down.";
            send(client_socket, response.c_str(), response.size(), 0);
        } else {
            std::string response = "ERROR: Unknown command.";
            send(client_socket, response.c_str(), response.size(), 0);
        }
    }
#ifdef _WIN32
    closesocket(client_socket);
#else
    close(client_socket);
#endif
}

void ipc_thread(int ipc_port, int udp_socket) {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::cerr << "IPC socket creation failed." << std::endl;
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(ipc_port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "IPC bind failed." << std::endl;
        return;
    }

    listen(server_socket, 5);
    set_non_blocking(server_socket);
    
    while (g_is_running) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket > 0) {
            std::thread handler_thread(handle_ipc_connection, client_socket, udp_socket);
            handler_thread.detach();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
#ifdef _WIN32
    closesocket(server_socket);
#else
    close(server_socket);
#endif
}

void udp_thread(int udp_socket, int port) {
    struct sockaddr_in server_addr, peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(udp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "UDP bind failed." << std::endl;
        return;
    }

    set_non_blocking(udp_socket);

    std::vector<char> buffer(MAX_PACKET_SIZE);

    while (g_is_running) {
        int bytes_received = recvfrom(udp_socket, buffer.data(), buffer.size(), 0, (struct sockaddr*)&peer_addr, &peer_addr_len);
        if (bytes_received > 0) {
            PacketHeader* header = (PacketHeader*)buffer.data();
            
            std::string peer_addr_str = inet_ntoa(peer_addr.sin_addr);
            int peer_port = ntohs(peer_addr.sin_port);

            std::unique_lock<std::mutex> lock(g_data_mutex);
            if (header->type == DATA) {
                // Check if this transfer already exists
                if (g_active_transfers.find(header->transfer_id) == g_active_transfers.end()) {
                    // Create a new file transfer entry for this receiving peer
                    FileTransfer new_transfer;
                    new_transfer.transfer_id = header->transfer_id;
                    new_transfer.filename = "received_file_" + std::to_string(header->transfer_id) + ".dat";
                    new_transfer.peer_address = peer_addr_str;
                    new_transfer.peer_port = peer_port;
                    new_transfer.is_sending = false;
                    new_transfer.total_chunks = 0; // Will be updated by SYN
                    new_transfer.receive_window_base = 0;
                    g_active_transfers[new_transfer.transfer_id] = new_transfer;
                    std::cout << "Starting new transfer for receiving: " << new_transfer.transfer_id << std::endl;
                }

                FileTransfer& transfer = g_active_transfers[header->transfer_id];
                
                // Store the received chunk in the buffer
                if (header->sequence_num >= transfer.received_status.size()) {
                    transfer.received_status.resize(header->sequence_num + 1, false);
                    transfer.received_chunks_buffer.resize((header->sequence_num + 1) * CHUNK_SIZE);
                }
                
                if (!transfer.received_status[header->sequence_num]) {
                    memcpy(transfer.received_chunks_buffer.data() + header->sequence_num * CHUNK_SIZE, buffer.data() + HEADER_SIZE, bytes_received - HEADER_SIZE);
                    transfer.received_status[header->sequence_num] = true;
                    std::cout << "Received chunk " << header->sequence_num << " for transfer ID " << header->transfer_id << std::endl;
                }

                // Send cumulative ACK for all chunks up to receive_window_base
                while (transfer.receive_window_base < transfer.received_status.size() && transfer.received_status[transfer.receive_window_base]) {
                    transfer.receive_window_base++;
                }

                PacketHeader ack_header;
                ack_header.transfer_id = header->transfer_id;
                ack_header.sequence_num = transfer.receive_window_base - 1;
                ack_header.type = ACK;

                sendto(udp_socket, (const char*)&ack_header, HEADER_SIZE, 0, (struct sockaddr*)&peer_addr, peer_addr_len);

                // Check for completion
                if (transfer.receive_window_base >= transfer.total_chunks && transfer.total_chunks != 0) {
                     std::cout << "All chunks received for transfer ID " << transfer.transfer_id << ". Assembling file..." << std::endl;
                     
                     std::ofstream outfile(transfer.filename, std::ios::binary);
                     if (outfile) {
                         outfile.write(transfer.received_chunks_buffer.data(), transfer.total_chunks * CHUNK_SIZE);
                         outfile.close();
                         std::cout << "File saved as " << transfer.filename << std::endl;
                     }
                     g_active_transfers.erase(transfer.transfer_id);
                }
            
            } else if (header->type == ACK) {
                // Handle ACK for sent data
                if (g_active_transfers.count(header->transfer_id)) {
                    FileTransfer& transfer = g_active_transfers[header->transfer_id];
                    
                    // This is a cumulative ACK, so we update the window base
                    if (header->sequence_num >= transfer.send_window_base) {
                        transfer.send_window_base = header->sequence_num + 1;
                        transfer.last_ack_time = std::chrono::steady_clock::now();
                    }
                }
            } else if (header->type == SYN) {
                // Handle SYN packet to initiate transfer on receiver side
                if (g_active_transfers.find(header->transfer_id) == g_active_transfers.end()) {
                    FileTransfer new_transfer;
                    new_transfer.transfer_id = header->transfer_id;
                    new_transfer.filename = "received_file_" + std::to_string(header->transfer_id) + ".dat";
                    new_transfer.peer_address = peer_addr_str;
                    new_transfer.peer_port = peer_port;
                    new_transfer.is_sending = false;
                    new_transfer.total_chunks = header->chunk_index; // Total chunks are in the SYN packet
                    new_transfer.receive_window_base = 0;
                    new_transfer.received_status.resize(new_transfer.total_chunks, false);
                    g_active_transfers[new_transfer.transfer_id] = new_transfer;
                    std::cout << "SYN received. Starting new transfer for receiving: " << new_transfer.transfer_id << std::endl;

                    // Send SYN_ACK back to the sender
                    PacketHeader syn_ack_header;
                    syn_ack_header.transfer_id = header->transfer_id;
                    syn_ack_header.type = ACK;
                    sendto(udp_socket, (const char*)&syn_ack_header, HEADER_SIZE, 0, (struct sockaddr*)&peer_addr, peer_addr_len);
                }
            }

            lock.unlock();
        }

        // Check for timeouts and send new packets within the window
        std::unique_lock<std::mutex> lock(g_data_mutex);
        for (auto& pair : g_active_transfers) {
            FileTransfer& transfer = pair.second;
            if (transfer.is_sending) {
                // Send new packets as long as the window is open
                while (transfer.next_seq_num < transfer.total_chunks && 
                       transfer.next_seq_num < transfer.send_window_base + WINDOW_SIZE) {
                    send_data_packet(udp_socket, transfer, transfer.next_seq_num);
                    transfer.next_seq_num++;
                    // Update last ack time to act as a timeout for the entire window
                    transfer.last_ack_time = std::chrono::steady_clock::now();
                }

                // Check for timeout on the window base
                auto now = std::chrono::steady_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - transfer.last_ack_time).count();
                if (elapsed_ms > TIMEOUT_MS) {
                    std::cout << "Timeout for transfer ID " << transfer.transfer_id << ". Retransmitting window from chunk " << transfer.send_window_base << std::endl;
                    // Retransmit all un-acked packets in the window
                    for (size_t i = transfer.send_window_base; i < transfer.next_seq_num; ++i) {
                        send_data_packet(udp_socket, transfer, i);
                    }
                    transfer.last_ack_time = now;
                }
            }
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#ifdef _WIN32
    closesocket(udp_socket);
#else
    close(udp_socket);
#endif
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <udp_port> <ipc_port>" << std::endl;
        return 1;
    }

    start_winsock();

    int udp_port = std::stoi(argv[1]);
    int ipc_port = std::stoi(argv[2]);

    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        std::cerr << "UDP socket creation failed." << std::endl;
        cleanup_winsock();
        return 1;
    }

    std::cout << "Core Peer Engine started. Listening for UDP on port " << udp_port << " and IPC on port " << ipc_port << std::endl;

    std::thread ipc_comm(ipc_thread, ipc_port, udp_socket);
    std::thread udp_comm(udp_thread, udp_socket, udp_port);

    ipc_comm.join();
    udp_comm.join();

#ifdef _WIN32
    closesocket(udp_socket);
#else
    close(udp_socket);
#endif
    cleanup_winsock();
    return 0;
}
