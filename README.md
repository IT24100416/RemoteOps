# RemoteOps - Network Programming Assignment

## Student Details

- Student ID: IT24100416
- Student Name: Sesath Rathnayaka
- Module: Network Programming
- Project: RemoteOps

## Project Description

RemoteOps is a client-server remote operations system implemented using TCP sockets in C.

The system consists of:

- Agent - TCP server
- Controller - TCP client

The Controller connects to the Agent and authenticates using a predefined token.

## Current Implementation

### Core TCP Communication

- TCP connection
- Authentication
- Session ID
- Command processing
- SYSINFO
- LISTPROC
- EXEC command whitelist
- QUIT command

### Allowed EXEC Commands

- DATE
- UPTIME
- DISKFREE
- HOSTNAME
- WHOAMI

Other EXEC commands are rejected.

## Environment

Operating System : Linux / CentOS
Programming Language : C
Compiler : GCC
Build Tool : Make
Networking : TCP/IP and UDP
Concurrency : POSIX pthreads
TCP Port : 9410
UDP Port : 9411

##Development History
The project was developed incrementally using Git.
Major development stages included:
1. Implement core TCP RemoteOps functionality
2. Implement PUT file upload
3. Implement GET file download
4. Implement TCP message framing
5. Add pthread-based multiple client support
6. Implement UDP monitoring
7. Complete UDP monitoring and logging
8. Fix GET file transfer protocol
9. Finalize project documentation

##Project Completion
The RemoteOps project was completed as an implementation of a remote system monitoring and management tool over TCP/IP.
The final implementation demonstrates socket programming, TCP communication, concurrent client handling, authentication, system monitoring, file transfer, UDP monitoring, message framing, and graceful connection management.
