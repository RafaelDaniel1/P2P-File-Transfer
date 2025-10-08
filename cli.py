# Filename: cli.py
#
# This program serves as the command-line interface for the P2P file transfer application.
# It now includes a new 'run_peer' command that handles peer registration and heartbeat
# sending, making the system more robust and self-healing.
#
# To run:
# 1. Install the requests library: pip install requests
# 2. To start your peer, run: python cli.py run_peer <my_peer_id> <my_ip> <my_udp_port> <my_ipc_port>
# 3. From a different terminal, you can send commands:
#    - Example: python cli.py list
#    - Example: python cli.py send <peer_ip:udp_port> <file_path> <my_ipc_port>
#    - Example: python cli.py shutdown <my_ipc_port>

import argparse
import requests
import sys 
import socket
import threading
import time

# Configuration
DISCOVERY_SERVER_URL = "http://localhost:8000"
CORE_ENGINE_HOST = "localhost"
HEARTBEAT_INTERVAL_SECONDS = 30

def send_command_to_core(ipc_port: int, command: str):
    """Sends a command to the C++ Core Peer Engine via a local TCP socket."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.connect((CORE_ENGINE_HOST, ipc_port))
            sock.sendall(command.encode())
            response = sock.recv(1024).decode()
            print("Engine Response:", response)
    except ConnectionRefusedError:
        print(f"ERROR: Could not connect to Core Peer Engine on port {ipc_port}. Is it running?")
    except Exception as e:
        print(f"An error occurred: {e}")

def heartbeat_task(peer_id: str, ip_address: str, udp_port: int, ipc_port: int):
    """Periodically sends a heartbeat to the discovery server."""
    while True:
        try:
            url = f"{DISCOVERY_SERVER_URL}/heartbeat/{peer_id}"
            response = requests.post(url)
            response.raise_for_status()
            # print(f"Heartbeat sent for {peer_id}.")
        except requests.exceptions.RequestException as e:
            print(f"ERROR: Failed to send heartbeat: {e}. Attempting to re-register.")
            try:
                url = f"{DISCOVERY_SERVER_URL}/register/{peer_id}"
                response = requests.post(url, params={"ip_address": ip_address, "port": udp_port})
                response.raise_for_status()
                print(f"Re-registered {peer_id} successfully.")
            except requests.exceptions.RequestException as e:
                print(f"ERROR: Failed to re-register: {e}")
                
        time.sleep(HEARTBEAT_INTERVAL_SECONDS)

def main():
    parser = argparse.ArgumentParser(description="P2P File Transfer CLI")
    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Run Peer command - the new main entry point
    parser_run = subparsers.add_parser("run_peer", help="Start the peer and send heartbeats.")
    parser_run.add_argument("peer_id", help="A unique ID for this peer.")
    parser_run.add_argument("ip_address", help="The public IP address of this peer.")
    parser_run.add_argument("udp_port", type=int, help="The port this peer's engine is listening on for UDP.")
    parser_run.add_argument("ipc_port", type=int, help="The port this peer's engine is listening on for IPC.")
    
    # List command
    subparsers.add_parser("list", help="List all registered peers from the discovery server.")

    # Send command
    parser_send = subparsers.add_parser("send", help="Send a file to another peer.")
    parser_send.add_argument("peer_address", help="The IP:UDP_Port of the peer to send to.")
    parser_send.add_argument("file_path", help="The path to the file to send.")
    parser_send.add_argument("ipc_port", type=int, help="The IPC port of this local peer's engine.")

    # Shutdown command
    parser_shutdown = subparsers.add_parser("shutdown", help="Shut down the C++ Core Peer Engine.")
    parser_shutdown.add_argument("ipc_port", type=int, help="The IPC port of this local peer's engine.")

    args = parser.parse_args()

    if args.command == "run_peer":
        try:
            # First, register the peer
            url = f"{DISCOVERY_SERVER_URL}/register/{args.peer_id}"
            response = requests.post(url, params={"ip_address": args.ip_address, "port": args.udp_port})
            response.raise_for_status()
            print(f"Peer '{args.peer_id}' registered successfully. Now sending heartbeats...")
            
            # Start the heartbeat thread
            heartbeat_thread = threading.Thread(target=heartbeat_task, args=(args.peer_id, args.ip_address, args.udp_port, args.ipc_port), daemon=True)
            heartbeat_thread.start()
            
            print("Peer is now running. Press Ctrl+C to stop.")
            while True:
                time.sleep(1)
        except requests.exceptions.RequestException as e:
            print(f"ERROR: Failed to register with discovery server: {e}")
        except KeyboardInterrupt:
            print("\nShutting down...")
            # Optionally unregister on shutdown, but heartbeat timeout will handle it
            # requests.delete(f"{DISCOVERY_SERVER_URL}/unregister/{args.peer_id}")

    elif args.command == "list":
        try:
            response = requests.get(f"{DISCOVERY_SERVER_URL}/peers")
            response.raise_for_status()
            peers = response.json()
            print("Registered Peers:")
            for peer in peers:
                print(f"  - {peer}")
        except requests.exceptions.RequestException as e:
            print(f"ERROR: Failed to get peer list from discovery server: {e}")

    elif args.command == "send":
        command = f"start_transfer {args.peer_address} {args.file_path}"
        send_command_to_core(args.ipc_port, command)

    elif args.command == "shutdown":
        send_command_to_core(args.ipc_port, "shutdown")
        
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
