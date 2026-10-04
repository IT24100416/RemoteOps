#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9410
#define BUFFER_SIZE 1024

#define AUTH_TOKEN "OPS-0416"
#define SID "6140"


/*
 * Send the complete message to the Controller.
 */
int send_message(int client_fd, const char *message)
{
    size_t total = 0;
    size_t length = strlen(message);

    while (total < length)
    {
        ssize_t sent = send(client_fd,
                            message + total,
                            length - total,
                            0);

        if (sent <= 0)
        {
            return -1;
        }

        total += sent;
    }

    return 0;
}


/*
 * SYSINFO
 *
 * CPU load  -> /proc/loadavg
 * Memory    -> /proc/meminfo
 * Uptime    -> /proc/uptime
 */
void handle_sysinfo(int client_fd)
{
    FILE *fp;

    double cpu_load = 0.0;
    double uptime_seconds = 0.0;

    unsigned long mem_total_kb = 0;
    unsigned long mem_available_kb = 0;

    char line[256];


    /* Read CPU load */
    fp = fopen("/proc/loadavg", "r");

    if (fp != NULL)
    {
        fscanf(fp, "%lf", &cpu_load);
        fclose(fp);
    }


    /* Read memory information */
    fp = fopen("/proc/meminfo", "r");

    if (fp != NULL)
    {
        while (fgets(line, sizeof(line), fp) != NULL)
        {
            if (sscanf(line,
                       "MemTotal: %lu kB",
                       &mem_total_kb) == 1)
            {
                continue;
            }

            if (sscanf(line,
                       "MemAvailable: %lu kB",
                       &mem_available_kb) == 1)
            {
                continue;
            }
        }

        fclose(fp);
    }


    /* Calculate used memory in MB */
    unsigned long mem_used_mb = 0;

    if (mem_total_kb >= mem_available_kb)
    {
        mem_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }


    /* Read uptime */
    fp = fopen("/proc/uptime", "r");

    if (fp != NULL)
    {
        fscanf(fp, "%lf", &uptime_seconds);
        fclose(fp);
    }


    /* Create SYSINFO response */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK SYSINFO %.2f %lu %.0f SID:%s\n",
             cpu_load,
             mem_used_mb,
             uptime_seconds,
             SID);

    send_message(client_fd, response);
}


/*
 * Check whether a string contains only numbers.
 */
int is_number(const char *text)
{
    if (text == NULL || *text == '\0')
    {
        return 0;
    }

    for (size_t i = 0; text[i] != '\0'; i++)
    {
        if (!isdigit((unsigned char)text[i]))
        {
            return 0;
        }
    }

    return 1;
}


/*
 * LISTPROC
 *
 * Read process information from /proc.
 */
void handle_listproc(int client_fd)
{
    DIR *proc_dir;

    struct dirent *entry;

    char response[BUFFER_SIZE];

    size_t used = 0;


    /* Start response */
    used += snprintf(response + used,
                     sizeof(response) - used,
                     "OK PROCS");


    /* Open /proc */
    proc_dir = opendir("/proc");

    if (proc_dir == NULL)
    {
        snprintf(response + used,
                 sizeof(response) - used,
                 " SID:%s\n",
                 SID);

        send_message(client_fd, response);

        return;
    }


    int process_count = 0;


    /* Read /proc entries */
    while ((entry = readdir(proc_dir)) != NULL)
    {
        /* Only numeric directory names are PIDs */
        if (!is_number(entry->d_name))
        {
            continue;
        }


        /*
         * Build /proc/PID/comm path.
         */
        char comm_path[512];

        snprintf(comm_path,
                 sizeof(comm_path),
                 "/proc/%s/comm",
                 entry->d_name);


        /* Open process name */
        FILE *fp = fopen(comm_path, "r");

        if (fp == NULL)
        {
            continue;
        }


        char process_name[128];


        /* Read process name */
        if (fgets(process_name,
                  sizeof(process_name),
                  fp) != NULL)
        {
            /* Remove newline */
            process_name[strcspn(process_name,
                                 "\r\n")] = '\0';


            /* Add process to response */
            int written = snprintf(
                response + used,
                sizeof(response) - used,
                " %s:%s",
                entry->d_name,
                process_name
            );


            if (written < 0 ||
                (size_t)written >=
                sizeof(response) - used)
            {
                fclose(fp);
                break;
            }


            used += (size_t)written;

            process_count++;


            /*
             * Limit response to 20 processes
             * so it stays inside one buffer.
             */
            if (process_count >= 20)
            {
                fclose(fp);
                break;
            }
        }

        fclose(fp);
    }


    closedir(proc_dir);


    /* Add SID */
    snprintf(response + used,
             sizeof(response) - used,
             " SID:%s\n",
             SID);


    send_message(client_fd, response);
}


/*
 * PUT
 *
 * Receives a file from the Controller.
 *
 * Protocol:
 *
 * PUT <filename> <filesize>
 * followed by exactly <filesize> raw bytes.
 */
void handle_put(int client_fd,
                const char *filename,
                long filesize)
{
    char filepath[512];


    /*
     * Store uploaded files inside:
     * ./agentfiles/IT24100416/
     */
    snprintf(filepath,
             sizeof(filepath),
             "./agentfiles/IT24100416/%s",
             filename);


    /*
     * Open file in binary write mode.
     */
    FILE *fp = fopen(filepath, "wb");

    if (fp == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR FILE_OPEN SID:%s\n",
                 SID);

        send_message(client_fd, response);

        return;
    }


    /*
     * Receive file data in chunks.
     */
    char file_buffer[4096];

    long remaining = filesize;


    while (remaining > 0)
    {
        size_t to_receive;


        /*
         * Receive either 4096 bytes
         * or whatever remains.
         */
        if (remaining > (long)sizeof(file_buffer))
        {
            to_receive = sizeof(file_buffer);
        }
        else
        {
            to_receive = (size_t)remaining;
        }


        ssize_t received = recv(client_fd,
                                file_buffer,
                                to_receive,
                                0);


        /*
         * Connection/error during transfer.
         */
        if (received <= 0)
        {
            fclose(fp);

            remove(filepath);

            return;
        }


        /*
         * Write received bytes to file.
         */
        size_t written = fwrite(file_buffer,
                                1,
                                (size_t)received,
                                fp);


        /*
         * File writing error.
         */
        if (written != (size_t)received)
        {
            fclose(fp);

            remove(filepath);

            return;
        }


        remaining -= received;
    }


    fclose(fp);


    /*
     * Tell Controller that upload
     * completed successfully.
     */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s SID:%s\n",
             filename,
             SID);


    send_message(client_fd, response);
}


/*
 * EXEC
 *
 * Allowed commands:
 *
 * DATE
 * UPTIME
 * DISKFREE
 * HOSTNAME
 * WHOAMI
 */
void handle_exec(int client_fd, const char *command)
{
    const char *allowed_command = NULL;


    /*
     * Command whitelist.
     */
    if (strcmp(command, "DATE") == 0)
    {
        allowed_command = "date";
    }
    else if (strcmp(command, "UPTIME") == 0)
    {
        allowed_command = "uptime";
    }
    else if (strcmp(command, "DISKFREE") == 0)
    {
        /*
         * tail -1 removes the df header
         * and returns the actual filesystem row.
         */
        allowed_command = "df -h / | tail -1";
    }
    else if (strcmp(command, "HOSTNAME") == 0)
    {
        allowed_command = "hostname";
    }
    else if (strcmp(command, "WHOAMI") == 0)
    {
        allowed_command = "whoami";
    }


    /*
     * Reject commands outside the whitelist.
     */
    if (allowed_command == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR COMMAND_NOT_ALLOWED SID:%s\n",
                 SID);

        send_message(client_fd, response);

        return;
    }


    /*
     * Execute permitted command.
     */
    FILE *fp = popen(allowed_command, "r");

    if (fp == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR EXEC_FAILED SID:%s\n",
                 SID);

        send_message(client_fd, response);

        return;
    }


    char result[512];

    memset(result, 0, sizeof(result));


    /*
     * Read command output.
     */
    if (fgets(result,
              sizeof(result),
              fp) == NULL)
    {
        strcpy(result, "No output");
    }


    pclose(fp);


    /*
     * Remove newline.
     */
    result[strcspn(result,
                   "\r\n")] = '\0';


    /*
     * Create response.
     */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK EXEC_RESULT %s SID:%s\n",
             result,
             SID);


    send_message(client_fd, response);
}


int main(void)
{
    int server_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    socklen_t client_len =
        sizeof(client_addr);

    char buffer[BUFFER_SIZE];


    /*
     * Create TCP socket.
     */
    server_fd = socket(AF_INET,
                       SOCK_STREAM,
                       0);

    if (server_fd < 0)
    {
        perror("socket");

        return 1;
    }


    /*
     * Allow port reuse.
     */
    int option = 1;

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &option,
                   sizeof(option)) < 0)
    {
        perror("setsockopt");

        close(server_fd);

        return 1;
    }


    /*
     * Configure server address.
     */
    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    server_addr.sin_port =
        htons(PORT);


    /*
     * Bind.
     */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");

        close(server_fd);

        return 1;
    }


    /*
     * Listen.
     */
    if (listen(server_fd, 5) < 0)
    {
        perror("listen");

        close(server_fd);

        return 1;
    }


    printf("RemoteOps Agent started.\n");

    printf("Listening on TCP port %d...\n",
           PORT);


    /*
     * Accept Controller.
     */
    client_fd = accept(server_fd,
                       (struct sockaddr *)&client_addr,
                       &client_len);

    if (client_fd < 0)
    {
        perror("accept");

        close(server_fd);

        return 1;
    }


    printf("Controller connected.\n");


    int authenticated = 0;


    /*
     * Main command loop.
     */
    while (1)
    {
        memset(buffer,
               0,
               sizeof(buffer));


        /*
         * Receive command.
         */
        int bytes_received =
            recv(client_fd,
                 buffer,
                 sizeof(buffer) - 1,
                 0);


        if (bytes_received <= 0)
        {
            printf("Controller disconnected.\n");

            break;
        }


        buffer[bytes_received] = '\0';


        /*
         * Remove newline.
         */
        buffer[strcspn(buffer,
                       "\r\n")] = '\0';


        printf("Received: %s\n",
               buffer);


        /*
         * AUTH
         */
        if (strncmp(buffer,
                    "AUTH ",
                    5) == 0)
        {
            char token[100];

            memset(token,
                   0,
                   sizeof(token));


            sscanf(buffer + 5,
                   "%99s",
                   token);


            if (strcmp(token,
                       AUTH_TOKEN) == 0)
            {
                authenticated = 1;


                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "OK AUTHENTICATED SID:%s\n",
                         SID);


                send_message(client_fd,
                             response);


                printf("Authentication successful.\n");
            }
            else
            {
                send_message(client_fd,
                             "ERR AUTH\n");


                printf("Authentication failed.\n");
            }


            continue;
        }


        /*
         * Commands require authentication.
         */
        if (!authenticated)
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "ERR NOT_AUTHENTICATED SID:%s\n",
                     SID);


            send_message(client_fd,
                         response);

            continue;
        }


        /*
         * PUT
         */
        if (strncmp(buffer,
                    "PUT ",
                    4) == 0)
        {
            char filename[256];

            long filesize;


            memset(filename,
                   0,
                   sizeof(filename));


            /*
             * Read filename and file size.
             */
            if (sscanf(buffer + 4,
                       "%255s %ld",
                       filename,
                       &filesize) != 2)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR INVALID_PUT SID:%s\n",
                         SID);

                send_message(client_fd,
                             response);

                continue;
            }


            /*
             * Reject invalid file size.
             */
            if (filesize < 0)
            {
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR INVALID_SIZE SID:%s\n",
                         SID);

                send_message(client_fd,
                             response);

                continue;
            }


            /*
             * Receive the file.
             */
            handle_put(client_fd,
                       filename,
                       filesize);
        }


        /*
         * SYSINFO
         */
        else if (strcmp(buffer,
                        "SYSINFO") == 0)
        {
            handle_sysinfo(client_fd);
        }


        /*
         * LISTPROC
         */
        else if (strcmp(buffer,
                        "LISTPROC") == 0)
        {
            handle_listproc(client_fd);
        }


        /*
         * EXEC
         */
        else if (strncmp(buffer,
                         "EXEC ",
                         5) == 0)
        {
            char command[100];

            memset(command,
                   0,
                   sizeof(command));


            sscanf(buffer + 5,
                   "%99s",
                   command);


            handle_exec(client_fd,
                        command);
        }


        /*
         * QUIT
         */
        else if (strcmp(buffer,
                        "QUIT") == 0)
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "OK BYE SID:%s\n",
                     SID);


            send_message(client_fd,
                         response);


            printf("Controller requested disconnect.\n");

            break;
        }


        /*
         * Unknown command.
         */
        else
        {
            char response[BUFFER_SIZE];

            snprintf(response,
                     sizeof(response),
                     "ERR UNKNOWN_COMMAND SID:%s\n",
                     SID);


            send_message(client_fd,
                         response);
        }
    }


    /*
     * Close sockets.
     */
    close(client_fd);

    close(server_fd);


    printf("Agent stopped.\n");

    return 0;
}
