# Rock-Paper-Scissors UDP Multiplayer Game

A lightweight multiplayer implementation of the Rock-Paper-Scissors game using UDP sockets for communication.

## Overview

This project consists of a server and client components that communicate via UDP protocol. The server manages the game logic, tracks active clients, and facilitates the Rock-Paper-Scissors tournament. Clients can register, maintain their status through periodic pings, and participate in game rounds.

## Components

### Server
- Manages client registrations and tracks active status
- Handles the game logic for Rock-Paper-Scissors tournaments
- Provides administrative commands for server management
- Broadcasts game results to all connected clients
- Maintains client hardware information

### Client
- Auto-registers with server on startup
- Maintains connection with periodic ping messages
- Responds to server commands automatically
- Makes random choices (rock, paper, scissors) when prompted
- Gracefully handles disconnection

## Features

- **Automatic client discovery**: Clients can discover and connect to the server by hostname
- **Active client tracking**: Server monitors client activity through ping messages
- **Tournament system**: Multi-round elimination tournament until a single winner remains
- **Client hardware reporting**: Clients report CPU and RAM information
- **Administrator interface**: Server provides commands to monitor and control the system

## Building and Running

The project uses Docker for containerization:

```bash
# Build and run the server and multiple clients
docker-compose up -d --build
docker attach game_server
```

## Network Protocol

The system uses a simple text-based protocol over UDP:
- `REGISTER:<name>:<hardware>` - Client registration
- `PING` - Keep-alive message
- `CHOOSE` - Server request for client choice
- `ROCK`, `PAPER`, `SCISSORS` - Client choices
- `SHUTDOWN` - Server command to terminate clients
