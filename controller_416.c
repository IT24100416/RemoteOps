#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9410
#define BUFFER_SIZE 1024

#define AUTH_TOKEN "OPS-0416"


/*
 * PUT
 *
 * Upload a file from the Controller to the Agent.
 *
 * Protocol:
 *
 * PUT <filename> <filesize>\n
 * followed by exactly <filesize> raw bytes.
 */
int handle_put(int sock_fd, const char *filename)
{
    FILE *fp = fopen(filename, "rb");

    if (fp == NULL)
    {
        perror("fopen");
        return -1;
    }


    /*
     * Find the file size.
     */
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        perror("fseek");
        fclose(fp);
        return -1;
    }

    long filesize = ftell(fp);

    if (filesize < 0)
    {
        perror("ftell");
        fclose(fp);
        return -1;
    }

    rewind(fp);


    /*
     * Send PUT command.
     */
    char request[BUFFER_SIZE];

    snprintf(request,
             sizeof(request),
             "PUT %s %ld\n",
             filename,
             filesize);


    if (send(sock_fd,
             request,
             strlen(request),
             0) < 0)
    {
        perror("send");
        fclose(fp);
        return -1;
    }


    /*
     * Send file data.
     */
    char file_buffer[4096];

    long remaining = filesize;

    while (remaining > 0)
    {
        size_t to_read;

        if (remaining > (long)sizeof(file_buffer))
        {
            to_read = sizeof(file_buffer);
        }
        else
        {
            to_read = (size_t)remaining;
        }


        size_t bytes_read =
            fread(file_buffer,
                  1,
                  to_read,
                  fp);


        if (bytes_read == 0)
        {
            if (ferror(fp))
            {
                perror("fread");
                fclose(fp);
                return -1;
            }

            break;
        }


        /*
         * TCP send may send fewer bytes than requested.
         * Therefore, continue sending until the
         * complete chunk has been transmitted.
         */
        size_t total_sent = 0;

        while (total_sent < bytes_read)
        {
            ssize_t sent =
                send(sock_fd,
                     file_buffer + total_sent,
                     bytes_read - total_sent,
                     0);


            if (sent <= 0)
            {
                perror("send");
                fclose(fp);
                return -1;
            }

            total_sent += (size_t)sent;
        }


        remaining -= (long)bytes_read;
    }


    fclose(fp);


    /*
     * Receive Agent response.
     */
    char response[BUFFER_SIZE];

    memset(response,
           0,
           sizeof(response));


    ssize_t received =
        recv(sock_fd,
             response,
             sizeof(response) - 1,
             0);


    if (received <= 0)
    {
        printf("Agent disconnected.\n");
        return -1;
    }


    response[received] = '\0';


    printf("Agent: %s",
           response);


    return 0;
}


int main(void)
{
    int sock_fd;

    struct sockaddr_in server_addr;

    char buffer[BUFFER_SIZE];


    /*
     * Create TCP socket.
     */
    sock_fd = socket(AF_INET,
                     SOCK_STREAM,
                     0);

    if (sock_fd < 0)
    {
        perror("socket");
        return 1;
    }


    /*
     * Configure Agent address.
     */
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


    /*
     * Connect to Agent.
     */
    if (connect(sock_fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");

        close(sock_fd);

        return 1;
    }


    printf("Connected to RemoteOps Agent.\n");


    /*
     * Authenticate automatically.
     */
    char auth_message[BUFFER_SIZE];

    snprintf(auth_message,
             sizeof(auth_message),
             "AUTH %s\n",
             AUTH_TOKEN);


    if (send(sock_fd,
             auth_message,
             strlen(auth_message),
             0) < 0)
    {
        perror("send");

        close(sock_fd);

        return 1;
    }


    /*
     * Receive authentication response.
     */
    memset(buffer,
           0,
           sizeof(buffer));


    int bytes_received =
        recv(sock_fd,
             buffer,
             sizeof(buffer) - 1,
             0);


    if (bytes_received <= 0)
    {
        printf("Agent disconnected.\n");

        close(sock_fd);

        return 1;
    }


    buffer[bytes_received] = '\0';


    printf("Agent: %s",
           buffer);


    /*
     * Command loop.
     */
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


        /*
         * Remove newline from user input.
         */
        buffer[strcspn(buffer,
                       "\r\n")] = '\0';


        /*
         * Ignore empty input.
         */
        if (strlen(buffer) == 0)
        {
            continue;
        }


        /*
         * PUT
         *
         * User enters:
         *
         * PUT test.txt
         *
         * The Controller automatically calculates
         * the file size and sends the file.
         */
        if (strncmp(buffer,
                    "PUT ",
                    4) == 0)
        {
            char filename[256];

            memset(filename,
                   0,
                   sizeof(filename));


            /*
             * Extract filename.
             */
            if (sscanf(buffer + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: PUT <filename>\n");

                continue;
            }


            /*
             * Upload file.
             */
            handle_put(sock_fd,
                       filename);

            continue;
        }


        /*
         * Add newline required by
         * the RemoteOps protocol.
         */
        char message[BUFFER_SIZE + 1];

        snprintf(message,
                 sizeof(message),
                 "%s\n",
                 buffer);


        /*
         * Send normal command.
         */
        if (send(sock_fd,
                 message,
                 strlen(message),
                 0) < 0)
        {
            perror("send");
            break;
        }


        /*
         * QUIT
         */
        if (strcmp(buffer,
                   "QUIT") == 0)
        {
            memset(buffer,
                   0,
                   sizeof(buffer));


            bytes_received =
                recv(sock_fd,
                     buffer,
                     sizeof(buffer) - 1,
                     0);


            if (bytes_received > 0)
            {
                buffer[bytes_received] = '\0';

                printf("Agent: %s",
                       buffer);
            }


            break;
        }


        /*
         * Receive Agent response.
         */
        memset(buffer,
               0,
               sizeof(buffer));


        bytes_received =
            recv(sock_fd,
                 buffer,
                 sizeof(buffer) - 1,
                 0);


        if (bytes_received <= 0)
        {
            printf("Agent disconnected.\n");
            break;
        }


        buffer[bytes_received] = '\0';


        printf("Agent: %s",
               buffer);
    }


    /*
     * Close TCP socket.
     */
    close(sock_fd);


    printf("Controller stopped.\n");


    return 0;
}
