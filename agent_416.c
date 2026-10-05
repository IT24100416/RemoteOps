#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>

#define PORT 9410
#define BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 4096

#define AUTH_TOKEN "OPS-0416"
#define SID "6140"

#define FILE_DIRECTORY "./agentfiles/IT24100416"


/*
 * ============================================================
 * SEND ALL
 * ============================================================
 *
 * Sends all requested bytes.
 */
int send_all(int socket_fd, const void *data, size_t length)
{
    size_t total = 0;

    const char *buffer = (const char *)data;

    while (total < length)
    {
        ssize_t sent = send(socket_fd,
                            buffer + total,
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
 * ============================================================
 * SEND MESSAGE
 * ============================================================
 */
int send_message(int client_fd, const char *message)
{
    return send_all(client_fd,
                    message,
                    strlen(message));
}


/*
 * ============================================================
 * RECEIVE ALL
 * ============================================================
 *
 * Receives exactly length bytes.
 */
int recv_all(int socket_fd, void *data, size_t length)
{
    size_t total = 0;

    char *buffer = (char *)data;

    while (total < length)
    {
        ssize_t received = recv(socket_fd,
                                buffer + total,
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
 * ============================================================
 * RECEIVE ONE LINE
 * ============================================================
 *
 * TCP does not preserve message boundaries.
 * This function reads until '\n'.
 */
int recv_line(int socket_fd,
              char *buffer,
              size_t size)
{
    if (buffer == NULL || size < 2)
    {
        return -1;
    }

    size_t index = 0;

    while (index < size - 1)
    {
        char character;

        ssize_t received = recv(socket_fd,
                                &character,
                                1,
                                0);

        if (received <= 0)
        {
            return -1;
        }

        if (character == '\n')
        {
            break;
        }

        buffer[index++] = character;
    }

    buffer[index] = '\0';

    return (int)index;
}


/*
 * ============================================================
 * REMOVE NEWLINE
 * ============================================================
 */
void remove_newline(char *text)
{
    if (text == NULL)
    {
        return;
    }

    text[strcspn(text, "\r\n")] = '\0';
}


/*
 * ============================================================
 * VALIDATE FILENAME
 * ============================================================
 *
 * Prevent directory traversal such as:
 *
 * ../file
 * ../../file
 */
int valid_filename(const char *filename)
{
    if (filename == NULL ||
        filename[0] == '\0')
    {
        return 0;
    }

    if (strstr(filename, "..") != NULL)
    {
        return 0;
    }

    if (strchr(filename, '/') != NULL)
    {
        return 0;
    }

    if (strchr(filename, '\\') != NULL)
    {
        return 0;
    }

    return 1;
}


/*
 * ============================================================
 * SYSINFO
 * ============================================================
 */
void handle_sysinfo(int client_fd)
{
    FILE *fp;

    double cpu_load = 0.0;
    double uptime_seconds = 0.0;

    unsigned long mem_total_kb = 0;
    unsigned long mem_available_kb = 0;

    char line[256];

    /*
     * CPU load
     */
    fp = fopen("/proc/loadavg", "r");

    if (fp != NULL)
    {
        fscanf(fp,
               "%lf",
               &cpu_load);

        fclose(fp);
    }

    /*
     * Memory
     */
    fp = fopen("/proc/meminfo", "r");

    if (fp != NULL)
    {
        while (fgets(line,
                      sizeof(line),
                      fp) != NULL)
        {
            sscanf(line,
                   "MemTotal: %lu kB",
                   &mem_total_kb);

            sscanf(line,
                   "MemAvailable: %lu kB",
                   &mem_available_kb);
        }

        fclose(fp);
    }

    unsigned long mem_used_mb = 0;

    if (mem_total_kb >= mem_available_kb)
    {
        mem_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }

    /*
     * Uptime
     */
    fp = fopen("/proc/uptime", "r");

    if (fp != NULL)
    {
        fscanf(fp,
               "%lf",
               &uptime_seconds);

        fclose(fp);
    }

    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK SYSINFO %.2f %lu %.0f SID:%s\n",
             cpu_load,
             mem_used_mb,
             uptime_seconds,
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * IS NUMBER
 * ============================================================
 */
int is_number(const char *text)
{
    if (text == NULL ||
        *text == '\0')
    {
        return 0;
    }

    for (size_t i = 0;
         text[i] != '\0';
         i++)
    {
        if (!isdigit((unsigned char)text[i]))
        {
            return 0;
        }
    }

    return 1;
}


/*
 * ============================================================
 * LISTPROC
 * ============================================================
 */
void handle_listproc(int client_fd)
{
    DIR *proc_dir;

    struct dirent *entry;

    char response[BUFFER_SIZE];

    size_t used = 0;

    used += (size_t)snprintf(
        response + used,
        sizeof(response) - used,
        "OK PROCS"
    );

    proc_dir = opendir("/proc");

    if (proc_dir == NULL)
    {
        snprintf(response + used,
                 sizeof(response) - used,
                 " SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    int process_count = 0;

    while ((entry = readdir(proc_dir)) != NULL)
    {
        if (!is_number(entry->d_name))
        {
            continue;
        }

        char comm_path[512];

        snprintf(comm_path,
                 sizeof(comm_path),
                 "/proc/%s/comm",
                 entry->d_name);

        FILE *fp = fopen(comm_path, "r");

        if (fp == NULL)
        {
            continue;
        }

        char process_name[128];

        if (fgets(process_name,
                  sizeof(process_name),
                  fp) != NULL)
        {
            remove_newline(process_name);

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
             * Keep response within buffer.
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

    snprintf(response + used,
             sizeof(response) - used,
             " SID:%s\n",
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * EXEC
 * ============================================================
 *
 * Only these commands are permitted:
 *
 * DATE
 * UPTIME
 * DISKFREE
 * HOSTNAME
 * WHOAMI
 */
void handle_exec(int client_fd,
                 const char *command)
{
    const char *allowed_command = NULL;

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

    if (allowed_command == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR COMMAND_NOT_ALLOWED SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    FILE *fp = popen(allowed_command,
                     "r");

    if (fp == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR EXEC_FAILED SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    char result[512];

    memset(result,
           0,
           sizeof(result));

    if (fgets(result,
              sizeof(result),
              fp) == NULL)
    {
        strcpy(result,
               "No output");
    }

    pclose(fp);

    remove_newline(result);

    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK EXEC_RESULT %s SID:%s\n",
             result,
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * PUT
 * ============================================================
 *
 * Controller sends:
 *
 * PUT filename filesize\n
 *
 * followed by exactly filesize bytes.
 */
void handle_put(int client_fd,
                const char *filename,
                long filesize)
{
    if (!valid_filename(filename))
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR INVALID_FILENAME SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    if (filesize < 0)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR INVALID_SIZE SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    FILE *fp = fopen(filepath,
                     "wb");

    if (fp == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR FILE_OPEN SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    char file_buffer[FILE_BUFFER_SIZE];

    long remaining = filesize;

    while (remaining > 0)
    {
        size_t to_receive;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            to_receive =
                sizeof(file_buffer);
        }
        else
        {
            to_receive =
                (size_t)remaining;
        }

        /*
         * IMPORTANT:
         * Receive exactly the required
         * number of bytes.
         */
        if (recv_all(client_fd,
                     file_buffer,
                     to_receive) < 0)
        {
            fclose(fp);

            remove(filepath);

            return;
        }

        size_t written =
            fwrite(file_buffer,
                   1,
                   to_receive,
                   fp);

        if (written != to_receive)
        {
            fclose(fp);

            remove(filepath);

            return;
        }

        remaining -=
            (long)to_receive;
    }

    fclose(fp);

    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s SID:%s\n",
             filename,
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * GET
 * ============================================================
 *
 * Controller:
 *
 * GET filename\n
 *
 * Agent:
 *
 * OK FILE_SIZE filesize SID:sid\n
 *
 * followed by exactly filesize bytes.
 *
 * Finally:
 *
 * OK FILE_SENT filename SID:sid\n
 */
void handle_get(int client_fd,
                const char *filename)
{
    if (!valid_filename(filename))
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR INVALID_FILENAME SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    FILE *fp = fopen(filepath,
                     "rb");

    if (fp == NULL)
    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "ERR FILE_NOT_FOUND SID:%s\n",
                 SID);

        send_message(client_fd,
                     response);

        return;
    }

    if (fseek(fp,
              0,
              SEEK_END) != 0)
    {
        fclose(fp);

        send_message(client_fd,
                     "ERR FILE_ERROR\n");

        return;
    }

    long filesize =
        ftell(fp);

    if (filesize < 0)
    {
        fclose(fp);

        send_message(client_fd,
                     "ERR FILE_ERROR\n");

        return;
    }

    rewind(fp);

    /*
     * Send file size first.
     */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK FILE_SIZE %ld SID:%s\n",
             filesize,
             SID);

    if (send_message(client_fd,
                     response) < 0)
    {
        fclose(fp);
        return;
    }

    /*
     * Send file data.
     */
    char file_buffer[FILE_BUFFER_SIZE];

    long remaining = filesize;

    while (remaining > 0)
    {
        size_t to_read;

        if (remaining >
            (long)sizeof(file_buffer))
        {
            to_read =
                sizeof(file_buffer);
        }
        else
        {
            to_read =
                (size_t)remaining;
        }

        size_t bytes_read =
            fread(file_buffer,
                  1,
                  to_read,
                  fp);

        if (bytes_read == 0)
        {
            fclose(fp);
            return;
        }

        if (send_all(client_fd,
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

    snprintf(response,
             sizeof(response),
             "OK FILE_SENT %s SID:%s\n",
             filename,
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * CLIENT THREAD
 * ============================================================
 *
 * Every connected Controller gets its own thread.
 */
void *handle_client(void *arg)
{
    int client_fd =
        *((int *)arg);

    /*
     * The main thread allocated this.
     * Free it after copying the descriptor.
     */
    free(arg);

    pthread_t thread_id =
        pthread_self();

    printf("[Thread %lu] Controller connected.\n",
           (unsigned long)thread_id);

    int authenticated = 0;

    char buffer[BUFFER_SIZE];

    while (1)
    {
        memset(buffer,
               0,
               sizeof(buffer));

        /*
         * TCP framing:
         *
         * Receive exactly one command
         * line ending with '\n'.
         */
        int bytes_received =
            recv_line(client_fd,
                      buffer,
                      sizeof(buffer));

        if (bytes_received <= 0)
        {
            printf("[Thread %lu] Controller disconnected.\n",
                   (unsigned long)thread_id);

            break;
        }

        remove_newline(buffer);

        printf("[Thread %lu] Received: %s\n",
               (unsigned long)thread_id,
               buffer);

        /*
         * ====================================================
         * AUTH
         * ====================================================
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

                printf("[Thread %lu] Authentication successful.\n",
                       (unsigned long)thread_id);
            }
            else
            {
                send_message(client_fd,
                             "ERR AUTH\n");

                printf("[Thread %lu] Authentication failed.\n",
                       (unsigned long)thread_id);
            }

            continue;
        }

        /*
         * ====================================================
         * AUTHENTICATION CHECK
         * ====================================================
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
         * ====================================================
         * PUT
         * ====================================================
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

            handle_put(client_fd,
                       filename,
                       filesize);
        }

        /*
         * ====================================================
         * GET
         * ====================================================
         */
        else if (strncmp(buffer,
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
                char response[BUFFER_SIZE];

                snprintf(response,
                         sizeof(response),
                         "ERR INVALID_GET SID:%s\n",
                         SID);

                send_message(client_fd,
                             response);

                continue;
            }

            handle_get(client_fd,
                       filename);
        }

        /*
         * ====================================================
         * SYSINFO
         * ====================================================
         */
        else if (strcmp(buffer,
                        "SYSINFO") == 0)
        {
            handle_sysinfo(client_fd);
        }

        /*
         * ====================================================
         * LISTPROC
         * ====================================================
         */
        else if (strcmp(buffer,
                        "LISTPROC") == 0)
        {
            handle_listproc(client_fd);
        }

        /*
         * ====================================================
         * EXEC
         * ====================================================
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
         * ====================================================
         * QUIT
         * ====================================================
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

            printf("[Thread %lu] Controller requested disconnect.\n",
                   (unsigned long)thread_id);

            break;
        }

        /*
         * ====================================================
         * UNKNOWN COMMAND
         * ====================================================
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

    close(client_fd);

    printf("[Thread %lu] Client thread finished.\n",
           (unsigned long)thread_id);

    return NULL;
}


/*
 * ============================================================
 * MAIN
 * ============================================================
 *
 * The main thread only:
 *
 * 1. Creates the server socket.
 * 2. Listens on TCP 9410.
 * 3. Accepts clients.
 * 4. Creates a pthread for each client.
 *
 * Each client is handled independently.
 */
int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    /*
     * Create TCP socket.
     */
    server_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Allow address reuse.
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
    if (listen(server_fd,
               10) < 0)
    {
        perror("listen");

        close(server_fd);

        return 1;
    }

    printf("RemoteOps Agent started.\n");
    printf("Listening on TCP port %d...\n",
           PORT);

    /*
     * ========================================================
     * MULTIPLE CLIENT LOOP
     * ========================================================
     */
    while (1)
    {
        struct sockaddr_in client_addr;

        socklen_t client_len =
            sizeof(client_addr);

        int client_fd =
            accept(server_fd,
                   (struct sockaddr *)&client_addr,
                   &client_len);

        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("accept");

            continue;
        }

        /*
         * Allocate socket descriptor for
         * the new thread.
         */
        int *client_ptr =
            malloc(sizeof(int));

        if (client_ptr == NULL)
        {
            perror("malloc");

            close(client_fd);

            continue;
        }

        *client_ptr = client_fd;

        pthread_t thread;

        /*
         * Create a separate thread
         * for this Controller.
         */
        if (pthread_create(&thread,
                           NULL,
                           handle_client,
                           client_ptr) != 0)
        {
            perror("pthread_create");

            close(client_fd);

            free(client_ptr);

            continue;
        }

        /*
         * Detached thread:
         * resources are automatically
         * released when the thread exits.
         */
        if (pthread_detach(thread) != 0)
        {
            perror("pthread_detach");
        }

        printf("New client thread created.\n");
    }

    /*
     * Normally unreachable because
     * the Agent continuously accepts clients.
     */
    close(server_fd);

    return 0;
}
