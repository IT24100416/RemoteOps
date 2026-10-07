# RemoteOps - Network Programming Assignment

## Student Details

- Student ID: IT24100416
- Student Name: Sesath
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

- Operating System: CentOS 10
- Programming Language: C
- Protocol: TCP
- TCP Port: 9410
- Compiler: GCC

## Current Status

Day 1 - Core TCP implementation completed.

