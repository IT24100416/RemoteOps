#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9410
#define BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 4096

#define UDP_DEFAULT_PORT 9411

#define AUTH_TOKEN "OPS-0416"

/* ============================================================
 * TCP SEND ALL
 * ============================================================ */

int send_all(int sock_fd,
             const void *data,
             size_t length)
{
    size_t total = 0;
    const char *ptr = data;

    while (total < length)
    {
        ssize_t sent =
            send(sock_fd,
                 ptr + total,
                 length - total,
                 0);

        if (sent <= 0)
        {
            return -1;
        }

        total += (size_t)sent;
    }

    return 0;
}

/* ============================================================
 * TCP RECEIVE LINE
 * ============================================================ */

int recv_line(int sock_fd,
              char *buffer,
              size_t size)
{
    size_t total = 0;

    while (total < size - 1)
    {
        char c;

        ssize_t received =
            recv(sock_fd,
                 &c,
                 1,
                 0);

        if (received <= 0)
        {
            return -1;
        }

        if (c == '\n')
        {
            break;
        }

        if (c != '\r')
        {
            buffer[total++] = c;
        }
    }

    buffer[total] = '\0';

    return 0;
}

/* ============================================================
 * RECEIVE EXACT DATA
 * ============================================================ */

int recv_all(int sock_fd,
             void *buffer,
             size_t length)
{
    size_t total = 0;
    char *ptr = buffer;

    while (total < length)
    {
        ssize_t received =
            recv(sock_fd,
                 ptr + total,
                 length - total,
                 0);

        if (received <= 0)
        {
            return -1;
        }

        total += (size_t)received;
    }

    return 0;
}

/* ============================================================
 * UDP MONITOR
 * ============================================================ */

static int udp_monitor_running = 0;
static int udp_socket_fd = -1;
static pthread_t udp_thread;

void *udp_monitor_receiver(void *arg)
{
    (void)arg;

    char buffer[BUFFER_SIZE];

    while (udp_monitor_running)
    {
        struct sockaddr_in sender_addr;

        socklen_t sender_len =
            sizeof(sender_addr);

        ssize_t received =
            recvfrom(udp_socket_fd,
                     buffer,
                     sizeof(buffer) - 1,
                     0,
                     (struct sockaddr *)&sender_addr,
                     &sender_len);

        if (received < 0)
        {
            if (!udp_monitor_running)
            {
                break;
            }

            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        buffer[received] = '\0';

        printf("\nUDP Monitor: %s\n",
               buffer);

        printf("RemoteOps> ");
        fflush(stdout);
    }

    return NULL;
}

int start_udp_monitor(int port)
{
    if (udp_monitor_running)
    {
        printf("UDP monitor is already running.\n");
        return 0;
    }

    udp_socket_fd =
        socket(AF_INET,
               SOCK_DGRAM,
               0);

    if (udp_socket_fd < 0)
    {
        perror("UDP socket");
        return -1;
    }

    int option = 1;

    setsockopt(udp_socket_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));

    struct sockaddr_in local_addr;

    memset(&local_addr,
           0,
           sizeof(local_addr));

    local_addr.sin_family =
        AF_INET;

    local_addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    local_addr.sin_port =
        htons((uint16_t)port);

    if (bind(udp_socket_fd,
             (struct sockaddr *)&local_addr,
             sizeof(local_addr)) < 0)
    {
        perror("UDP bind");

        close(udp_socket_fd);
        udp_socket_fd = -1;

        return -1;
    }

    udp_monitor_running = 1;

    if (pthread_create(&udp_thread,
                       NULL,
                       udp_monitor_receiver,
                       NULL) != 0)
    {
        perror("pthread_create");

        udp_monitor_running = 0;

        close(udp_socket_fd);
        udp_socket_fd = -1;

        return -1;
    }

    return 0;
}

void stop_udp_monitor(void)
{
    if (!udp_monitor_running)
    {
        return;
    }

    udp_monitor_running = 0;

    if (udp_socket_fd >= 0)
    {
        shutdown(udp_socket_fd,
                 SHUT_RDWR);

        close(udp_socket_fd);

        udp_socket_fd = -1;
    }

    pthread_join(udp_thread,
                 NULL);
}

/* ============================================================
 * PUT
 * ============================================================ */

void handle_put(int sock_fd,
                const char *filename)
{
    FILE *fp =
        fopen(filename, "rb");

    if (fp == NULL)
    {
        perror("Cannot open file");
        return;
    }

    if (fseek(fp,
              0,
              SEEK_END) != 0)
    {
        fclose(fp);
        return;
    }

    long filesize =
        ftell(fp);

    rewind(fp);

    if (filesize < 0)
    {
        fclose(fp);
        return;
    }

    char command[BUFFER_SIZE];

    snprintf(command,
             sizeof(command),
             "PUT %s %ld\n",
             filename,
             filesize);

    if (send_all(sock_fd,
                 command,
                 strlen(command)) < 0)
    {
        fclose(fp);
        return;
    }

    char file_buffer[FILE_BUFFER_SIZE];

    long remaining =
        filesize;

    while (remaining > 0)
    {
        size_t chunk;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            chunk = sizeof(file_buffer);
        }
        else
        {
            chunk = (size_t)remaining;
        }

        size_t bytes_read =
            fread(file_buffer,
                  1,
                  chunk,
                  fp);

        if (bytes_read != chunk)
        {
            fclose(fp);
            return;
        }

        if (send_all(sock_fd,
                     file_buffer,
                     bytes_read) < 0)
        {
            fclose(fp);
            return;
        }

        remaining -=
            (long)bytes_read;
    }

    fclose(fp);

    char response[BUFFER_SIZE];

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        return;
    }

    printf("Agent: %s\n",
           response);
}

/* ============================================================
 * GET
 * ============================================================ */

void handle_get(int sock_fd,
                const char *filename)
{
    char command[BUFFER_SIZE];

    snprintf(command,
             sizeof(command),
             "GET %s\n",
             filename);

    if (send_all(sock_fd,
                 command,
                 strlen(command)) < 0)
    {
        return;
    }

    char response[BUFFER_SIZE];

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        return;
    }

    printf("Agent: %s\n",
           response);

    long filesize;

    if (sscanf(response,
               "OK FILE_SIZE %ld",
               &filesize) != 1)
    {
        return;
    }

    if (filesize < 0)
    {
        printf("Invalid file size.\n");
        return;
    }

    char output_filename[300];

    snprintf(output_filename,
             sizeof(output_filename),
             "downloaded_%s",
             filename);

    FILE *fp =
        fopen(output_filename,
              "wb");

    if (fp == NULL)
    {
        perror("fopen");
        return;
    }

    printf("Downloading %s (%ld bytes)...\n",
           filename,
           filesize);

    char file_buffer[FILE_BUFFER_SIZE];

    long remaining =
        filesize;

    while (remaining > 0)
    {
        size_t chunk;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            chunk = sizeof(file_buffer);
        }
        else
        {
            chunk = (size_t)remaining;
        }

        if (recv_all(sock_fd,
                     file_buffer,
                     chunk) < 0)
        {
            fclose(fp);
            return;
        }

        if (fwrite(file_buffer,
                   1,
                   chunk,
                   fp) != chunk)
        {
            fclose(fp);
            return;
        }

        remaining -=
            (long)chunk;
    }

    fclose(fp);

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        return;
    }

    printf("Agent: %s\n",
           response);

    printf("Download complete: %s\n",
           output_filename);
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    int sock_fd;

    struct sockaddr_in server_addr;

    sock_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (sock_fd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");

        close(sock_fd);

        return 1;
    }

    if (connect(sock_fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");

        close(sock_fd);

        return 1;
    }

    printf("Connected to RemoteOps Agent.\n");

    /* ========================================================
     * AUTH
     * ======================================================== */

    char buffer[BUFFER_SIZE];

    char auth_message[BUFFER_SIZE];

    snprintf(auth_message,
             sizeof(auth_message),
             "AUTH %s\n",
             AUTH_TOKEN);

    if (send_all(sock_fd,
                 auth_message,
                 strlen(auth_message)) < 0)
    {
        close(sock_fd);
        return 1;
    }

    if (recv_line(sock_fd,
                  buffer,
                  sizeof(buffer)) < 0)
    {
        printf("Agent disconnected.\n");

        close(sock_fd);
        return 1;
    }

    printf("Agent: %s\n",
           buffer);

    /* ========================================================
     * COMMAND LOOP
     * ======================================================== */

    while (1)
    {
        printf("\nRemoteOps> ");
        fflush(stdout);

        if (fgets(buffer,
                  sizeof(buffer),
                  stdin) == NULL)
        {
            break;
        }

        buffer[strcspn(buffer,
                       "\r\n")] = '\0';

        if (strlen(buffer) == 0)
        {
            continue;
        }

        /* ====================================================
         * PUT
         * ==================================================== */

        if (strncmp(buffer,
                    "PUT ",
                    4) == 0)
        {
            char filename[256];

            if (sscanf(buffer + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: PUT <filename>\n");
                continue;
            }

            handle_put(sock_fd,
                       filename);

            continue;
        }

        /* ====================================================
         * GET
         * ==================================================== */

        if (strncmp(buffer,
                    "GET ",
                    4) == 0)
        {
            char filename[256];

            if (sscanf(buffer + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: GET <filename>\n");
                continue;
            }

            handle_get(sock_fd,
                       filename);

            continue;
        }

        /* ====================================================
         * MONITOR START
         * ==================================================== */

        if (strncmp(buffer,
                    "MONITOR START",
                    13) == 0)
        {
            int port =
                UDP_DEFAULT_PORT;

            int parsed_port;

            if (sscanf(buffer,
                       "MONITOR START %d",
                       &parsed_port) == 1)
            {
                port = parsed_port;
            }

            if (start_udp_monitor(port) < 0)
            {
                printf("Failed to start UDP receiver.\n");
                continue;
            }

            char command[BUFFER_SIZE];

            snprintf(command,
                     sizeof(command),
                     "MONITOR START %d\n",
                     port);

            if (send_all(sock_fd,
                         command,
                         strlen(command)) < 0)
            {
                stop_udp_monitor();
                break;
            }

            if (recv_line(sock_fd,
                          buffer,
                          sizeof(buffer)) < 0)
            {
                stop_udp_monitor();
                break;
            }

            printf("Agent: %s\n",
                   buffer);

            continue;
        }

        /* ====================================================
         * MONITOR STOP
         * ==================================================== */

        if (strcmp(buffer,
                   "MONITOR STOP") == 0)
        {
            const char *command =
                "MONITOR STOP\n";

            if (send_all(sock_fd,
                         command,
                         strlen(command)) < 0)
            {
                break;
            }

            if (recv_line(sock_fd,
                          buffer,
                          sizeof(buffer)) < 0)
            {
                break;
            }

            printf("Agent: %s\n",
                   buffer);

            stop_udp_monitor();

            continue;
        }

        /* ====================================================
         * QUIT
         * ==================================================== */

        if (strcmp(buffer,
                   "QUIT") == 0)
        {
            const char *command =
                "QUIT\n";

            send_all(sock_fd,
                     command,
                     strlen(command));

            if (recv_line(sock_fd,
                          buffer,
                          sizeof(buffer)) == 0)
            {
                printf("Agent: %s\n",
                       buffer);
            }

            stop_udp_monitor();

            break;
        }

        /* ====================================================
         * NORMAL COMMAND
         * ==================================================== */

        char message[BUFFER_SIZE + 1];

        snprintf(message,
                 sizeof(message),
                 "%s\n",
                 buffer);

        if (send_all(sock_fd,
                     message,
                     strlen(message)) < 0)
        {
            printf("Failed to send command.\n");
            break;
        }

        if (recv_line(sock_fd,
                      buffer,
                      sizeof(buffer)) < 0)
        {
            printf("Agent disconnected.\n");
            break;
        }

        printf("Agent: %s\n",
               buffer);
    }

    stop_udp_monitor();

    close(sock_fd);

    printf("Controller stopped.\n");

    return 0;
}
