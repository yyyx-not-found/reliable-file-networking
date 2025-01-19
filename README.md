# reliable-file-networking

## Overview
This is a course project of CS 341 at University of Illinois Urbana Champaign that implements basic client-server networking operations, such as file transfer and management, using custom-defined communication protocols.

## Features
- Client-Server Communication: Implements core networking functionalities using send and recv system calls.
- File Operations:
  - PUT: Upload files from the client to the server.
  - GET: Download files from the server to the client.
  - DELETE: Remove files on the server.
  - LIST: Retrieve a list of available files on the server.
- Error Handling: Provides robust handling for various edge cases such as malformed requests, incorrect file sizes, and invalid responses.
- Usage Guidance: Includes detailed usage and help information for both the client and server components.

## How to Run
- Run client with
  ```
  ./client <host>:<port> <method> [remote] [local]
  ```
- Run server with
  ```
  ./server <port>
  ```

## Disclaimer
This project is for educational and demonstration purposes only. Do not use this project for assignments or academic submissions.
