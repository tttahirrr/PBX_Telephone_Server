README.md

# PBX Telephone Server (Multithreaded Networking)
# Overview
This project implements a multithreaded network server that simulates a Private Branch Exchange (PBX) telephone system. It handles registration of virtual telephone units (TUs), supports call handling (pickup, hangup, dial, chat), and manages concurrent client communication using POSIX threads, sockets, and synchronization primitives.

# Features
Registers TUs as extensions.
Supports TU actions: pickup, hangup, dial, chat.
Handles TU state transitions: on-hook, ringing, dial tone, busy, connected, error.
Concurrent client handling via thread-per-client model.
Graceful server shutdown via SIGHUP signal handling.

# Modules Implemented
main.c: Server initialization, socket setup, signal handling, thread spawning.
server.c: Handles client connection, parses commands, dispatches to PBX.
pbx.c: Manages TU registry, extension mapping, call routing, synchronization.
tu.c: TU object logic, state transitions, network message handling.

# Build Instructions
make          # Build the server and test binaries
make clean    # Clean build artifacts

Executables:
Server: bin/pbx
Tests: bin/pbx_tests

Running the Server
bin/pbx -p <PORT>

Example:
bin/pbx -p 3333

Then connect using:
telnet localhost 3333


# Supported Commands
Sent from client to server (via telnet or test client):
pickup
hangup
dial <extension>
chat <message>
Server responses include:
ON HOOK #, DIAL TONE, RINGING, CONNECTED #, BUSY SIGNAL, ERROR, CHAT <msg>

# Testing
Run tests with:
bin/pbx_tests -j1

Includes unit tests for pbx.c, tu.c, and integration tests via script-based inputs.

