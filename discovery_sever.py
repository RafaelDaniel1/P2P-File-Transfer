# Filename: discovery_server.py
#
# This program implements a simple, stateless discovery server using FastAPI.
# It now includes a heartbeat mechanism to automatically remove inactive peers.
#
# To run:
# 1. Install FastAPI and Uvicorn: pip install "fastapi[all]" uvicorn
# 2. Run with: uvicorn discovery_server:app --host 0.0.0.0 --port 8000
# 3. The server will run on http://localhost:8000.
#
# A background thread periodically checks peer heartbeats to remove inactive peers.

from fastapi import FastAPI, HTTPException
from typing import Dict, List
import uvicorn
import time
import threading

# Configuration
HEARTBEAT_TIMEOUT_SECONDS = 35
CLEANUP_INTERVAL_SECONDS = 30

# In-memory store for peer information and last heartbeat time
# { "peer_id": {"address": "ip:port", "last_heartbeat": timestamp} }
peers: Dict[str, Dict] = {}
peers_lock = threading.Lock()

app = FastAPI(title="P2P Discovery Server")

def cleanup_stale_peers():
    """Background task to remove peers that haven't sent a heartbeat recently."""
    while True:
        with peers_lock:
            now = time.time()
            stale_peers = [peer_id for peer_id, data in peers.items() 
                           if now - data['last_heartbeat'] > HEARTBEAT_TIMEOUT_SECONDS]
            for peer_id in stale_peers:
                del peers[peer_id]
                print(f"Removed stale peer: {peer_id}")
        time.sleep(CLEANUP_INTERVAL_SECONDS)

# Start the cleanup thread on server startup
@app.on_event("startup")
def startup_event():
    thread = threading.Thread(target=cleanup_stale_peers, daemon=True)
    thread.start()

@app.post("/register/{peer_id}")
async def register_peer(peer_id: str, ip_address: str, port: int):
    """
    Registers or updates a peer's information.
    """
    with peers_lock:
        peers[peer_id] = {"address": f"{ip_address}:{port}", "last_heartbeat": time.time()}
    print(f"Peer '{peer_id}' registered/updated at {ip_address}:{port}")
    return {"message": f"Peer {peer_id} registered successfully."}

@app.post("/heartbeat/{peer_id}")
async def heartbeat(peer_id: str):
    """
    Updates the last heartbeat timestamp for a peer.
    """
    with peers_lock:
        if peer_id in peers:
            peers[peer_id]["last_heartbeat"] = time.time()
            return {"message": f"Heartbeat for {peer_id} received."}
        else:
            raise HTTPException(status_code=404, detail="Peer not found. Please register first.")

@app.get("/peers", response_model=List[str])
async def get_peers():
    """
    Returns a list of all currently registered peers (ip:port).
    """
    with peers_lock:
        return [data["address"] for data in peers.values()]

@app.delete("/unregister/{peer_id}")
async def unregister_peer(peer_id: str):
    """
    Unregisters a peer from the discovery server.
    """
    with peers_lock:
        if peer_id in peers:
            del peers[peer_id]
            print(f"Peer '{peer_id}' unregistered.")
            return {"message": f"Peer {peer_id} unregistered successfully."}
        else:
            raise HTTPException(status_code=404, detail="Peer not found.")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
