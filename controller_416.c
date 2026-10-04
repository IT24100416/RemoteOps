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
 * Receive one complete newline-terminated message.
 */
int recv_line(int sock_fd, char *buffer, size_t size)
{
    size_t index = 0;

    while (index < size - 1)
    {
        char ch;

        ssize_t received = recv(sock_fd,
                                &ch,
                                1,
                                0);

        if (received <= 0)
        {
            return -1;
        }

        buffer[index++] = ch;

        if (ch == '\n')
        {
            break;
        }
    }

    buffer[index] = '\0';

    return (int)index;
}


/*
 * Receive exactly the requested number of bytes.
 */
int recv_all(int sock_fd,
             void *buffer,
             size_t length)
{
    size_t total = 0;

    char *ptr = (char *)buffer;

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


/*
 * Send all requested bytes.
 */
int send_all(int sock_fd,
             const void *buffer,
             size_t length)
{
    size_t total = 0;

    const char *ptr = (const char *)buffer;

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


/*
 * GET FILE
 *
 * Sends:
 *
 *     GET filename
 *
 * Agent responds:
 *
 *     OK FILE_SIZE <size> SID:<sid>
 *
 * followed by raw file data.
 */
int download_file(int sock_fd,
                  const char *filename)
{
    char message[BUFFER_SIZE];

    /*
     * Send GET request.
     */
    snprintf(message,
             sizeof(message),
             "GET %s\n",
             filename);

    if (send_all(sock_fd,
                 message,
                 strlen(message)) < 0)
    {
        perror("send");

        return -1;
    }


    /*
     * Receive file-size response.
     */
    char response[BUFFER_SIZE];

    memset(response,
           0,
           sizeof(response));

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");

        return -1;
    }


    printf("Agent: %s",
           response);


    /*
     * Check whether Agent reported an error.
     */
    if (strncmp(response,
                "OK FILE_SIZE ",
                13) != 0)
    {
        return 0;
    }


    /*
     * Extract file size.
     *
     * Example:
     *
     * OK FILE_SIZE 34 SID:6140
     */
    long filesize = 0;

    if (sscanf(response + 13,
               "%ld",
               &filesize) != 1 ||
        filesize < 0)
    {
        printf("Invalid file size received.\n");

        return -1;
    }


    printf("Downloading %s (%ld bytes)...\n",
           filename,
           filesize);


    /*
     * Create local output file.
     */
    char output_filename[512];

    snprintf(output_filename,
             sizeof(output_filename),
             "downloaded_%s",
             filename);


    FILE *fp = fopen(output_filename,
                     "wb");

    if (fp == NULL)
    {
        perror("fopen");

        return -1;
    }


    /*
     * Receive exact file data.
     */
    char file_buffer[4096];

    long remaining = filesize;


    while (remaining > 0)
    {
        size_t chunk_size;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            chunk_size =
                sizeof(file_buffer);
        }
        else
        {
            chunk_size =
                (size_t)remaining;
        }


        if (recv_all(sock_fd,
                     file_buffer,
                     chunk_size) < 0)
        {
            printf("File transfer interrupted.\n");

            fclose(fp);

            remove(output_filename);

            return -1;
        }


        size_t written =
            fwrite(file_buffer,
                   1,
                   chunk_size,
                   fp);


        if (written != chunk_size)
        {
            printf("File write error.\n");

            fclose(fp);

            remove(output_filename);

            return -1;
        }


        remaining -=
            (long)chunk_size;
    }


    fclose(fp);


    /*
     * Receive final FILE_SENT message.
     */
    memset(response,
           0,
           sizeof(response));

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");

        return -1;
    }


    printf("Agent: %s",
           response);


    printf("Download complete: %s\n",
           output_filename);


    return 0;
}


/*
 * PUT FILE
 *
 * Sends:
 *
 *     PUT filename filesize
 *
 * followed by the raw file data.
 */
int upload_file(int sock_fd,
                const char *filename)
{
    FILE *fp = fopen(filename,
                     "rb");

    if (fp == NULL)
    {
        perror("fopen");

        return 0;
    }


    /*
     * Find file size.
     */
    if (fseek(fp,
              0,
              SEEK_END) != 0)
    {
        perror("fseek");

        fclose(fp);

        return 0;
    }


    long filesize = ftell(fp);

    if (filesize < 0)
    {
        perror("ftell");

        fclose(fp);

        return 0;
    }


    rewind(fp);


    /*
     * Send PUT header.
     */
    char message[BUFFER_SIZE];

    snprintf(message,
             sizeof(message),
             "PUT %s %ld\n",
             filename,
             filesize);


    if (send_all(sock_fd,
                 message,
                 strlen(message)) < 0)
    {
        perror("send");

        fclose(fp);

        return -1;
    }


    /*
     * Send file contents.
     */
    char file_buffer[4096];

    long remaining = filesize;


    while (remaining > 0)
    {
        size_t chunk_size;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            chunk_size =
                sizeof(file_buffer);
        }
        else
        {
            chunk_size =
                (size_t)remaining;
        }


        size_t bytes_read =
            fread(file_buffer,
                  1,
                  chunk_size,
                  fp);


        if (bytes_read == 0)
        {
            printf("File read error.\n");

            fclose(fp);

            return -1;
        }


        if (send_all(sock_fd,
                     file_buffer,
                     bytes_read) < 0)
        {
            perror("send");

            fclose(fp);

            return -1;
        }


        remaining -=
            (long)bytes_read;
    }


    fclose(fp);


    /*
     * Receive Agent response.
     */
    char response[BUFFER_SIZE];

    memset(response,
           0,
           sizeof(response));

    if (recv_line(sock_fd,
                  response,
                  sizeof(response)) < 0)
    {
        printf("Agent disconnected.\n");

        return -1;
    }


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
     * Automatic authentication.
     */
    char auth_message[BUFFER_SIZE];

    snprintf(auth_message,
             sizeof(auth_message),
             "AUTH %s\n",
             AUTH_TOKEN);


    if (send_all(sock_fd,
                 auth_message,
                 strlen(auth_message)) < 0)
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


    if (recv_line(sock_fd,
                  buffer,
                  sizeof(buffer)) < 0)
    {
        printf("Agent disconnected.\n");

        close(sock_fd);

        return 1;
    }


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
         * Remove newline.
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
         * ----------------------------------------
         * GET
         * ----------------------------------------
         */
        if (strncmp(buffer,
                    "GET ",
                    4) == 0)
        {
            char filename[256];

            memset(filename,
                   0,
                   sizeof(filename));


            if (sscanf(buffer + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: GET <filename>\n");

                continue;
            }


            download_file(sock_fd,
                          filename);

            continue;
        }


        /*
         * ----------------------------------------
         * PUT
         * ----------------------------------------
         */
        if (strncmp(buffer,
                    "PUT ",
                    4) == 0)
        {
            char filename[256];

            memset(filename,
                   0,
                   sizeof(filename));


            if (sscanf(buffer + 4,
                       "%255s",
                       filename) != 1)
            {
                printf("Usage: PUT <filename>\n");

                continue;
            }


            upload_file(sock_fd,
                        filename);

            continue;
        }


        /*
         * ----------------------------------------
         * Normal commands
         * ----------------------------------------
         */

        char message[BUFFER_SIZE + 1];

        snprintf(message,
                 sizeof(message),
                 "%s\n",
                 buffer);


        /*
         * Send command.
         */
        if (send_all(sock_fd,
                     message,
                     strlen(message)) < 0)
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


            if (recv_line(sock_fd,
                          buffer,
                          sizeof(buffer)) > 0)
            {
                printf("Agent: %s",
                       buffer);
            }


            break;
        }


        /*
         * Receive normal Agent response.
         */
        memset(buffer,
               0,
               sizeof(buffer));


        if (recv_line(sock_fd,
                      buffer,
                      sizeof(buffer)) < 0)
        {
            printf("Agent disconnected.\n");

            break;
        }


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
