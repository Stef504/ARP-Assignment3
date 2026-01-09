# ARP-Assignment3: Multi-Drone Networked Simulation

## Overview

This assignment extends the ARP-Assignment2 drone simulation to support **networked multi-drone communication**. The system now operates in three distinct modes:
1. **Standalone Mode** - Original single-drone simulation (Assignment 2 behavior)
2. **Server Mode** - Hosts a networked game, broadcasting drone position to connected clients
3. **Client Mode** - Connects to a server, receiving and displaying the server's drone position

The networked communication implements a **virtual coordinate system** with socket-based TCP/IP communication between server and client instances.

---

## How does it work?

- Two assignments are connected in a socket TCP/IP communication network
- Upon startup, the application asks the user to input a number in order to choose the mode to operation in, 1 for standalone, 2 for server and 3 for client.
- If running server mode, the application will prompt the user to insert the port number; if running client mode, the server's IP address will also be asked as well as a port number.
- Both networked modes will turn off the watchdog and the obstacle and target generators, then perform a TCP handshake and connect

### Server mode 
- Communicates to the client its windows'sizes, which the client reproduces through a virtual coordinate system; this proves useful in cases where server and client have different window sizes in their parameter files.
- Sends its drone's position to the client. Drone dynamics remain unchanged in both networked modes.
- Receives the clients drone position as an obstacle, with the already seen law of repulsion.
- Close connection on termination.

### Client mode
- Receives window sizes and drone position from the server and displays them.
- Moves its drone and sends its position to the server. This drone will be the only obstacle interpreted by the server.
- Terminates when receiving the connection closure.

---

## New Active Components

### 🌐 Communication Server (`Communication_Server.c`)

This process handles the server-side network communication using TCP sockets.

**Responsibilities:**
- Accepts incoming client connections on the listening socket
- Implements a handshake protocol with acknowledgments (`ok`/`ook`, `w,h`/`sok`)
- Reads local drone position from BlackBoard via `fdComm_FromBB`
- Converts local coordinates to **virtual coordinates** using transformation formulas
- Sends virtual drone position to the client
- Receives client's virtual drone position
- Converts virtual coordinates back to local coordinates
- Writes client position to BlackBoard via `fdComm_ToBB` (treated as obstacle)

**Virtual Coordinate Transformation:**
```
Local to Virtual:
x₁ = x₀ + x·cos(α) - y·sin(α)
y₁ = y₀ + x·sin(α) + y·cos(α)

Virtual to Local:
x = (x₁ - x₀)·cos(α) + (y₁ - y₀)·sin(α)
y = -(x₁ - x₀)·sin(α) + (y₁ - y₀)·cos(α)
```

**Communication Protocol (Main Loop):**
1. Send `drone` command → Send drone virtual position → Wait for `dok`
2. Send `obst` command → Wait for client position → Send `pok`

**Safety Features:**
- Socket timeout (5 seconds) for internet reliability
- Graceful handling of `SIGTERM` for clean shutdown
- Non-blocking select loop to allow for termination during waiting for client
- Sends `q` signal to client before closing
- Does not wait for `qok` as to allows the server to quit while waiting for client and in cases where the client dies unexpectedly

---

### 🌐 Communication Client (`Communication_Client.c`)

This process handles the client-side network communication using TCP sockets.

**Responsibilities:**
- Connects to the server using hostname and port
- Implements connection retry logic (5 retries, 3 seconds between attempts)
- Participates in handshake protocol
- Reads local drone position from BlackBoard via `fdComm_FromBB`
- Converts local coordinates to virtual coordinates
- Sends virtual drone position to the server
- Receives server's virtual drone position
- Converts virtual coordinates back to local coordinates
- Writes server position to BlackBoard via `fdComm_ToBB` (display only, no repulsion)

**Connection Handling:**
- Automatic retry on connection failure
- If connection cannot be established after max retries, signals parent process (`SIGTERM`) to initiate system shutdown
- Handles `q` signal from server for graceful termination

**Safety Features:**
- Socket timeout for read operations
- Graceful handling of `SIGTERM` for clean shutdown
- Proper cleanup of file descriptors and sockets on exit

---

## Communication Flow Diagrams

### Mode 1: Standalone
```
Drone → fdToBB → BlackBoard (reads drone position)
BlackBoard → fdFromBB → Drone (sends position back)
BlackBoard → fdRepul → Drone (sends repulsion forces)
Obstacle Generator → fdOb → BlackBoard (obstacle positions)
Target Generator → fdTa → BlackBoard (target positions)
```

### Mode 2: Server
```
Drone → fdToBB → BlackBoard (reads drone position)
BlackBoard → fdComm_FromBB → Communication Server (sends MY drone position)
Communication Server ←→ [TCP Socket] ←→ Client (virtual coordinates)
Communication Server → fdComm_ToBB → BlackBoard (receives CLIENT's position as obstacle)
BlackBoard → fdRepul → Drone (applies repulsion from client's position)
```

### Mode 3: Client
```
Drone → fdToBB → BlackBoard (reads MY drone position)
BlackBoard → fdComm_FromBB → Communication Client (sends MY drone position)
Communication Client ←→ [TCP Socket] ←→ Server (virtual coordinates)
Communication Client → fdComm_ToBB → BlackBoard (receives SERVER's position for display only)
```
---

## 🛠️ Installation and Running

### Prerequisites
- Linux operating system
- GCC compiler
- ncurses library (`libncurses-dev`)
- Konsole terminal emulator

### Running

**Standalone Mode (Single Drone):**
```bash
make clean
make
./main
# Select option 1
```

**Server Mode:**
```bash
make clean
make
./main
# Select option 2
# Enter port number (e.g., 5000)
```

**Client Mode:**
```bash
make clean
make
./main
# Select option 3
# Enter server hostname (e.g., localhost or IP address)
# Enter port number (same as server)
```

### Multi-Machine Setup
To run server and client on different machines:

1. **On Server Machine:**
```bash
   ./main
   # Select option 2
   # Enter port (e.g., 5000)
```

2. **On Client Machine:**
```bash
   ./main
   # Select option 3
   # Enter server's IP address
   # Enter port (5000)
   ```
---

## New Features in Assignment 3

1. **Multi-Mode Operation** - Choose between standalone, server, or client mode at startup
2. **TCP Socket Communication** - Real-time drone position sharing between networked instances
3. **Virtual Coordinate System** - Coordinate transformation for consistent positioning across different window sizes
4. **Drone-as-Obstacle** - In server mode, client's drone creates repulsion forces
5. **Graceful Connection Handling** - Retry logic, timeout handling, and clean shutdown procedures
6. **Protocol-Based Communication** - Structured handshake and data exchange with acknowledgments

---

## ⚠️ Disclaimers

> **Network Latency:** Network communication adds latency. Allow time for position updates to propagate.

> **Same Port:** Ensure server and client use the same port number.

> **Firewall:** If connecting across machines, ensure the port is not blocked by firewall.

> **Wait for Repulsion:** Please wait for the drone to stop moving after a repulsion force is detected before pressing another key.

>**Press one key at a time:** While running the game do not hold down the desired key, one press is sufficient, all special key features are enabled

>**WSL and Linux Network Configuration:** While being the server on WSL you might experience issues when the client tries to connect as windows may block the Linux ip address. If so it is better to test on two Linux Machines.

---

## Repository

All commits are accessible for evaluation on GitHub:
- **Assignment 3:** *https://github.com/Stef504/ARP-Assignment3.git*
