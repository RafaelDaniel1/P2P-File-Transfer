# P2P File Transfer System

## Overview

The P2P File Transfer System is a peer-to-peer application that enables reliable file sharing between clients over UDP. It combines a high-performance C++ core engine with a Python-based server for peer discovery and coordination.

## Purpose

UDP provides fast communication but does not guarantee reliability. This project was developed to implement a custom reliability layer on top of UDP using a sliding window protocol. The goal is to achieve efficient and controlled file transfer while handling packet loss, ordering, and retransmission manually.

## Technologies Used

* C++
* Python
* UDP Sockets
* Multithreading
* Custom Sliding Window Protocol

## Features

* Reliable file transfer over UDP
* Sliding window protocol with acknowledgments and retransmissions
* Peer discovery using a centralized Python server
* Heartbeat mechanism to track active peers
* Concurrent send and receive operations
* File chunking and reassembly

## Setup and Installation

### Prerequisites

* g++ (C++ compiler)
* Python 3.0

### Clone the Repository

```bash
git clone https://github.com/RafaelDaniel1/p2p-file-transfer.git
cd p2p-file-transfer
```

### Compile the Client

```bash
g++ client/core_engine.cpp -o client/client -pthread
```

### Run the Server

```bash
python server/discovery_server.py
```

## Usage

1. Start the server:

```bash
python server/discovery_server.py
```

2. Run the client (on multiple terminals or machines):

```bash
./client/client
```

3. Follow prompts in the CLI to:

* Register with the server
* Discover peers
* Select a peer to send a file
* Provide a file path 

4. The system will:

* Break the file into chunks
* Send data using a sliding window protocol
* Handle retransmissions if packets are lost
* Reassemble the file on the receiving side

## Example Workflow

* Client A connects to the server and registers
* Client B connects and retrieves the list of active peers
* Client B selects Client A and initiates file transfer
* File is transmitted reliably over UDP and reconstructed on Client A

## My Contribution

* Designed and implemented the reliable UDP protocol using a sliding window mechanism
* Developed packet acknowledgment and retransmission logic
* Built multithreaded client-side networking in C++
* Integrated the client with the Python-based discovery server

## Challenges and Lessons Learned

* Implementing reliability over UDP required handling packet loss, duplication, and ordering
* Debugging multithreaded networking introduced synchronization challenges
* Gained a deeper understanding of how transport-layer protocols like TCP work internally
* Learned to balance performance with reliability in distributed systems

## Future Improvements

* Add encryption for secure file transfer
* Implement congestion control mechanisms
* Develop a graphical user interface
* Improve scalability for larger peer networks


(Add screenshots or terminal output here showing file transfer in action)
