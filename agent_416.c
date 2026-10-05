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
#define FILE_BUFFER_SIZE 4096

#define AUTH_TOKEN "OPS-0416"
#define SID "6140"

#define FILE_DIRECTORY "./agentfiles/IT24100416"


/*
 * ============================================================
 * TCP FRAMING HELPERS
 * ============================================================
 */


/*
 * Send exactly 'length' bytes.
 *
 * TCP is a byte stream, so one send() is not guaranteed
 * to send the complete buffer.
 */
int send_all(int socket_fd,
             const void *buffer,
             size_t length)
{
    size_t total_sent = 0;

    const char *data = (const char *)buffer;

    while (total_sent < length)
    {
        ssize_t sent =
            send(socket_fd,
                 data + total_sent,
                 length - total_sent,
                 0);

        if (sent <= 0)
        {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}


/*
 * Send a complete null-terminated protocol message.
 */
int send_message(int client_fd,
                 const char *message)
{
    return send_all(client_fd,
                    message,
                    strlen(message));
}


/*
 * Receive one complete newline-terminated message.
 *
 * Example:
 *
 * AUTH OPS-0416\n
 * SYSINFO\n
 * GET test.txt\n
 *
 * TCP does not preserve message boundaries, therefore
 * we read until '\n' is received.
 */
int recv_line(int client_fd,
              char *buffer,
              size_t size)
{
    size_t index = 0;

    if (buffer == NULL || size < 2)
    {
        return -1;
    }

    while (index < size - 1)
    {
        char character;

        ssize_t received =
            recv(client_fd,
                 &character,
                 1,
                 0);

        if (received <= 0)
        {
            return -1;
        }

        buffer[index++] = character;

        if (character == '\n')
        {
            break;
        }
    }

    buffer[index] = '\0';

    return (int)index;
}


/*
 * Receive exactly 'length' bytes.
 *
 * This is used for file transfers.
 */
int recv_all(int client_fd,
             void *buffer,
             size_t length)
{
    size_t total_received = 0;

    char *data = (char *)buffer;

    while (total_received < length)
    {
        ssize_t received =
            recv(client_fd,
                 data + total_received,
                 length - total_received,
                 0);

        if (received <= 0)
        {
            return -1;
        }

        total_received += (size_t)received;
    }

    return 0;
}


/*
 * ============================================================
 * UTILITY
 * ============================================================
 */


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
 * Remove newline characters from a string.
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
 * Check filename for basic path traversal.
 *
 * We only allow a simple filename, not:
 *
 * ../file
 * /etc/passwd
 * directory/file
 */
int valid_filename(const char *filename)
{
    if (filename == NULL ||
        filename[0] == '\0')
    {
        return 0;
    }

    if (strcmp(filename, ".") == 0 ||
        strcmp(filename, "..") == 0)
    {
        return 0;
    }

    if (strstr(filename, "..") != NULL)
    {
        return 0;
    }

    if (strchr(filename, '/') != NULL ||
        strchr(filename, '\\') != NULL)
    {
        return 0;
    }

    return 1;
}


/*
 * ============================================================
 * SYSINFO
 * ============================================================
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

    /*
     * Read CPU load.
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
     * Read memory information.
     */
    fp = fopen("/proc/meminfo", "r");

    if (fp != NULL)
    {
        while (fgets(line,
                     sizeof(line),
                     fp) != NULL)
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

    /*
     * Calculate used memory in MB.
     */
    unsigned long mem_used_mb = 0;

    if (mem_total_kb >= mem_available_kb)
    {
        mem_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }

    /*
     * Read uptime.
     */
    fp = fopen("/proc/uptime", "r");

    if (fp != NULL)
    {
        fscanf(fp,
               "%lf",
               &uptime_seconds);

        fclose(fp);
    }

    /*
     * Create response.
     */
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
 * LISTPROC
 * ============================================================
 */
void handle_listproc(int client_fd)
{
    DIR *proc_dir;

    struct dirent *entry;

    char response[BUFFER_SIZE];

    size_t used = 0;

    /*
     * Start response.
     */
    used += (size_t)snprintf(
        response + used,
        sizeof(response) - used,
        "OK PROCS");

    /*
     * Open /proc.
     */
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

    /*
     * Read process entries.
     */
    while ((entry = readdir(proc_dir)) != NULL)
    {
        /*
         * Only numeric directory names are PIDs.
         */
        if (!is_number(entry->d_name))
        {
            continue;
        }

        char comm_path[512];

        snprintf(comm_path,
                 sizeof(comm_path),
                 "/proc/%s/comm",
                 entry->d_name);

        FILE *fp =
            fopen(comm_path, "r");

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

            int written =
                snprintf(response + used,
                          sizeof(response) - used,
                          " %s:%s",
                          entry->d_name,
                          process_name);

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
             * Keep response inside one buffer.
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

    /*
     * Add SID and newline.
     */
    snprintf(response + used,
             sizeof(response) - used,
             " SID:%s\n",
             SID);

    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * PUT
 * ============================================================
 *
 * Controller:
 *
 * PUT filename filesize\n
 *
 * followed by exactly filesize raw bytes.
 */
void handle_put(int client_fd,
                const char *filename,
                long filesize)
{
    /*
     * Validate filename.
     */
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

    /*
     * Validate size.
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

        return;
    }

    /*
     * Build destination path.
     */
    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    /*
     * Open destination file.
     */
    FILE *fp =
        fopen(filepath,
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

    /*
     * Receive exactly the declared number of bytes.
     */
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
         *
         * Use recv_all() instead of one recv().
         */
        if (recv_all(client_fd,
                     file_buffer,
                     to_receive) < 0)
        {
            fclose(fp);

            remove(filepath);

            return;
        }

        /*
         * Write exact chunk.
         */
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

    /*
     * Confirm successful upload.
     */
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
 * followed by exactly filesize raw bytes.
 *
 * Agent finally sends:
 *
 * OK FILE_SENT filename SID:sid\n
 */
void handle_get(int client_fd,
                const char *filename)
{
    /*
     * Validate filename.
     */
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

    /*
     * Build source path.
     */
    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    /*
     * Open file.
     */
    FILE *fp =
        fopen(filepath,
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

    /*
     * Find file size.
     */
    if (fseek(fp,
              0,
              SEEK_END) != 0)
    {
        fclose(fp);

        send_message(
            client_fd,
            "ERR FILE_ERROR\n");

        return;
    }

    long filesize =
        ftell(fp);

    if (filesize < 0)
    {
        fclose(fp);

        send_message(
            client_fd,
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
     * Send exact file contents.
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

        /*
         * IMPORTANT:
         *
         * Use send_all() to guarantee
         * the complete chunk is transmitted.
         */
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

    /*
     * Final confirmation.
     */
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
 * EXEC
 * ============================================================
 *
 * Only whitelisted commands are allowed.
 */
void handle_exec(int client_fd,
                 const char *command)
{
    const char *allowed_command = NULL;

    /*
     * Command whitelist.
     */
    if (strcmp(command,
               "DATE") == 0)
    {
        allowed_command = "date";
    }
    else if (strcmp(command,
                    "UPTIME") == 0)
    {
        allowed_command = "uptime";
    }
    else if (strcmp(command,
                    "DISKFREE") == 0)
    {
        allowed_command =
            "df -h / | tail -1";
    }
    else if (strcmp(command,
                    "HOSTNAME") == 0)
    {
        allowed_command =
            "hostname";
    }
    else if (strcmp(command,
                    "WHOAMI") == 0)
    {
        allowed_command =
            "whoami";
    }

    /*
     * Reject non-whitelisted commands.
     */
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

    /*
     * Execute permitted command.
     */
    FILE *fp =
        popen(allowed_command,
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

    /*
     * Remove newline.
     */
    remove_newline(result);

    /*
     * Send result.
     */
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
 * MAIN
 * ============================================================
 */
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
    if (listen(server_fd,
               5) < 0)
    {
        perror("listen");

        close(server_fd);

        return 1;
    }

    printf("RemoteOps Agent started.\n");
    printf("Listening on TCP port %d...\n",
           PORT);

    /*
     * Accept one Controller.
     *
     * pthread/multiple clients will be added
     * in the next Day 2 step.
     */
    client_fd =
        accept(server_fd,
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
         * IMPORTANT TCP FRAMING:
         *
         * Receive one complete command line.
         */
        int bytes_received =
            recv_line(client_fd,
                      buffer,
                      sizeof(buffer));

        if (bytes_received <= 0)
        {
            printf("Controller disconnected.\n");

            break;
        }

        /*
         * Remove newline.
         */
        remove_newline(buffer);

        printf("Received: %s\n",
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

            printf("Controller requested disconnect.\n");

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

    /*
     * Close sockets.
     */
    close(client_fd);

    close(server_fd);

    printf("Agent stopped.\n");

    return 0;
}
