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

int main(void)
{
    int sock_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    /* Create TCP socket */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (sock_fd < 0)
    {
        perror("socket");
        return 1;
    }

    /* Configure Agent address */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(sock_fd);
        return 1;
    }

    /* Connect to Agent */
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
     * Authenticate automatically
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

    /* Receive authentication response */
    memset(buffer, 0, sizeof(buffer));

    int bytes_received = recv(sock_fd,
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

    printf("Agent: %s", buffer);

    /*
     * Command loop
     */
    while (1)
    {
        printf("\nRemoteOps> ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        {
            break;
        }

        /* Remove newline from user input */
        buffer[strcspn(buffer, "\r\n")] = '\0';

        /* Ignore empty input */
        if (strlen(buffer) == 0)
        {
            continue;
        }

        /*
         * Add newline required by the RemoteOps protocol.
         *
         * BUFFER_SIZE + 1 prevents the warning caused by
         * adding '\n' to a maximum-length input.
         */
        char message[BUFFER_SIZE + 1];

        snprintf(message,
                 sizeof(message),
                 "%s\n",
                 buffer);

        /* Send command */
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
        if (strcmp(buffer, "QUIT") == 0)
        {
            memset(buffer, 0, sizeof(buffer));

            bytes_received = recv(sock_fd,
                                  buffer,
                                  sizeof(buffer) - 1,
                                  0);

            if (bytes_received > 0)
            {
                buffer[bytes_received] = '\0';
                printf("Agent: %s", buffer);
            }

            break;
        }

        /*
         * Receive Agent response
         */
        memset(buffer, 0, sizeof(buffer));

        bytes_received = recv(sock_fd,
                              buffer,
                              sizeof(buffer) - 1,
                              0);

        if (bytes_received <= 0)
        {
            printf("Agent disconnected.\n");
            break;
        }

        buffer[bytes_received] = '\0';

        printf("Agent: %s", buffer);
    }

    /* Close TCP socket */
    close(sock_fd);

    printf("Controller stopped.\n");

    return 0;
}
