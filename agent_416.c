#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

#define PORT 9410
#define BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 4096

#define AUTH_TOKEN "OPS-0416"
#define SID "6140"

#define FILE_DIRECTORY "./agentfiles/IT24100416"

#define MONITOR_INTERVAL 5


/*
 * ============================================================
 * UDP MONITOR GLOBAL VARIABLES
 * ============================================================
 */

static pthread_t monitor_thread;

static int monitor_running = 0;

static int monitor_udp_socket = -1;

static struct sockaddr_in monitor_destination;

static pthread_mutex_t monitor_mutex =
    PTHREAD_MUTEX_INITIALIZER;


/*
 * ============================================================
 * SEND MESSAGE
 * ============================================================
 *
 * Sends the complete TCP text response.
 */
int send_message(int client_fd,
                 const char *message)
{
    size_t total = 0;

    size_t length = strlen(message);

    while (total < length)
    {
        ssize_t sent =
            send(client_fd,
                 message + total,
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
 * SYSINFO DATA COLLECTION
 * ============================================================
 */

void collect_sysinfo(double *cpu_load,
                     unsigned long *mem_used_mb,
                     double *uptime_seconds)
{
    FILE *fp;

    char line[256];

    unsigned long mem_total_kb = 0;
    unsigned long mem_available_kb = 0;


    *cpu_load = 0.0;
    *mem_used_mb = 0;
    *uptime_seconds = 0.0;


    /*
     * CPU load
     */
    fp = fopen("/proc/loadavg", "r");

    if (fp != NULL)
    {
        fscanf(fp,
               "%lf",
               cpu_load);

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


    if (mem_total_kb >= mem_available_kb)
    {
        *mem_used_mb =
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
               uptime_seconds);

        fclose(fp);
    }
}


/*
 * ============================================================
 * SYSINFO
 * ============================================================
 */

void handle_sysinfo(int client_fd)
{
    double cpu_load;

    unsigned long mem_used_mb;

    double uptime_seconds;


    collect_sysinfo(&cpu_load,
                    &mem_used_mb,
                    &uptime_seconds);


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
 * NUMBER CHECK
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


    used +=
        (size_t)snprintf(response + used,
                         sizeof(response) - used,
                         "OK PROCS");


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


        FILE *fp =
            fopen(comm_path,
                  "r");


        if (fp == NULL)
        {
            continue;
        }


        char process_name[128];


        if (fgets(process_name,
                  sizeof(process_name),
                  fp) != NULL)
        {
            process_name[
                strcspn(process_name,
                        "\r\n")
            ] = '\0';


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


            used +=
                (size_t)written;


            process_count++;


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
 * PUT
 * ============================================================
 */

void handle_put(int client_fd,
                const char *filename,
                long filesize)
{
    /*
     * Create storage directory if necessary.
     */
    mkdir("agentfiles", 0755);

    mkdir(FILE_DIRECTORY, 0755);


    char filepath[512];


    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);


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
         * Receive exactly the required
         * amount, even if recv() returns
         * a partial chunk.
         */
        size_t total_received = 0;


        while (total_received < to_receive)
        {
            ssize_t received =
                recv(client_fd,
                     file_buffer + total_received,
                     to_receive - total_received,
                     0);


            if (received <= 0)
            {
                fclose(fp);

                remove(filepath);

                return;
            }


            total_received +=
                (size_t)received;
        }


        size_t written =
            fwrite(file_buffer,
                   1,
                   total_received,
                   fp);


        if (written != total_received)
        {
            fclose(fp);

            remove(filepath);

            return;
        }


        remaining -=
            (long)total_received;
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
 */

void handle_get(int client_fd,
                const char *filename)
{
    char filepath[512];


    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);


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


    if (fseek(fp,
              0,
              SEEK_END) != 0)
    {
        fclose(fp);

        send_message(client_fd,
                     "ERR FILE_ERROR SID:" SID "\n");

        return;
    }


    long filesize =
        ftell(fp);


    if (filesize < 0)
    {
        fclose(fp);

        send_message(client_fd,
                     "ERR FILE_ERROR SID:" SID "\n");

        return;
    }


    rewind(fp);


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


        size_t total_sent = 0;


        while (total_sent < bytes_read)
        {
            ssize_t sent =
                send(client_fd,
                     file_buffer + total_sent,
                     bytes_read - total_sent,
                     0);


            if (sent <= 0)
            {
                fclose(fp);

                return;
            }


            total_sent +=
                (size_t)sent;
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
 * EXEC
 * ============================================================
 */

void handle_exec(int client_fd,
                 const char *command)
{
    const char *allowed_command = NULL;


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
        allowed_command = "hostname";
    }
    else if (strcmp(command,
                    "WHOAMI") == 0)
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


    result[
        strcspn(result,
                "\r\n")
    ] = '\0';


    char response[BUFFER_SIZE];


    /*
     * Limit result length to prevent
     * snprintf truncation warnings.
     */
    snprintf(response,
             sizeof(response),
             "OK EXEC_RESULT %.900s SID:%s\n",
             result,
             SID);


    send_message(client_fd,
                 response);
}


/*
 * ============================================================
 * UDP MONITOR THREAD
 * ============================================================
 *
 * Sends periodic system information
 * to the Controller using UDP.
 *
 * Format:
 *
 * SYSINFO <cpu_load> <mem_used_mb>
 * <uptime_sec> SID:<sid>
 *
 * Interval: 5 seconds.
 */

void *monitor_thread_function(void *arg)
{
    (void)arg;


    char message[BUFFER_SIZE];


    while (1)
    {
        pthread_mutex_lock(&monitor_mutex);


        if (!monitor_running)
        {
            pthread_mutex_unlock(
                &monitor_mutex);

            break;
        }


        int udp_socket =
            monitor_udp_socket;


        struct sockaddr_in destination =
            monitor_destination;


        pthread_mutex_unlock(
            &monitor_mutex);


        double cpu_load;

        unsigned long mem_used_mb;

        double uptime_seconds;


        collect_sysinfo(&cpu_load,
                        &mem_used_mb,
                        &uptime_seconds);


        snprintf(message,
                 sizeof(message),
                 "SYSINFO %.2f %lu %.0f SID:%s\n",
                 cpu_load,
                 mem_used_mb,
                 uptime_seconds,
                 SID);


        sendto(udp_socket,
               message,
               strlen(message),
               0,
               (struct sockaddr *)&destination,
               sizeof(destination));


        /*
         * Wait for the next monitoring
         * interval.
         */
        sleep(MONITOR_INTERVAL);
    }


    return NULL;
}


/*
 * ============================================================
 * MONITOR START
 * ============================================================
 */

void handle_monitor_start(int client_fd,
                          const char *ip_address,
                          int udp_port)
{
    pthread_mutex_lock(&monitor_mutex);


    /*
     * Prevent two monitoring streams
     * from running simultaneously.
     */
    if (monitor_running)
    {
        pthread_mutex_unlock(
            &monitor_mutex);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR MONITOR_ALREADY_RUNNING SID:%s\n",
                 SID);


        send_message(client_fd,
                     response);

        return;
    }


    /*
     * Create UDP socket.
     */
    monitor_udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);


    if (monitor_udp_socket < 0)
    {
        pthread_mutex_unlock(
            &monitor_mutex);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR MONITOR_SOCKET SID:%s\n",
                 SID);


        send_message(client_fd,
                     response);

        return;
    }


    /*
     * Configure Controller destination.
     */
    memset(&monitor_destination,
           0,
           sizeof(monitor_destination));


    monitor_destination.sin_family =
        AF_INET;


    monitor_destination.sin_port =
        htons((unsigned short)udp_port);


    if (inet_pton(AF_INET,
                  ip_address,
                  &monitor_destination.sin_addr)
        <= 0)
    {
        close(monitor_udp_socket);

        monitor_udp_socket = -1;

        pthread_mutex_unlock(
            &monitor_mutex);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR INVALID_ADDRESS SID:%s\n",
                 SID);


        send_message(client_fd,
                     response);

        return;
    }


    monitor_running = 1;


    /*
     * Create monitoring thread.
     */
    if (pthread_create(&monitor_thread,
                       NULL,
                       monitor_thread_function,
                       NULL) != 0)
    {
        monitor_running = 0;

        close(monitor_udp_socket);

        monitor_udp_socket = -1;

        pthread_mutex_unlock(
            &monitor_mutex);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR MONITOR_THREAD SID:%s\n",
                 SID);


        send_message(client_fd,
                     response);

        return;
    }


    pthread_mutex_unlock(
        &monitor_mutex);


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK MONITOR_STARTED SID:%s\n",
             SID);


    send_message(client_fd,
                 response);


    printf("UDP monitoring started on %s:%d\n",
           ip_address,
           udp_port);
}


/*
 * ============================================================
 * MONITOR STOP
 * ============================================================
 */

void handle_monitor_stop(int client_fd)
{
    pthread_mutex_lock(&monitor_mutex);


    if (!monitor_running)
    {
        pthread_mutex_unlock(
            &monitor_mutex);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR MONITOR_NOT_RUNNING SID:%s\n",
                 SID);


        send_message(client_fd,
                     response);

        return;
    }


    /*
     * Tell monitor thread to stop.
     */
    monitor_running = 0;


    int socket_to_close =
        monitor_udp_socket;


    monitor_udp_socket = -1;


    pthread_mutex_unlock(
        &monitor_mutex);


    /*
     * Closing the UDP socket is safe here
     * because the monitor thread only uses
     * it for sendto().
     */
    if (socket_to_close >= 0)
    {
        close(socket_to_close);
    }


    /*
     * Wait for monitor thread.
     */
    pthread_join(monitor_thread,
                 NULL);


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK MONITOR_STOPPED SID:%s\n",
             SID);


    send_message(client_fd,
                 response);


    printf("UDP monitoring stopped.\n");
}


/*
 * ============================================================
 * STOP ACTIVE MONITOR
 * ============================================================
 *
 * Used when:
 *
 * - QUIT
 * - Controller disconnects
 */

void stop_monitor_if_running(void)
{
    pthread_mutex_lock(&monitor_mutex);


    if (!monitor_running)
    {
        pthread_mutex_unlock(
            &monitor_mutex);

        return;
    }


    monitor_running = 0;


    int socket_to_close =
        monitor_udp_socket;


    monitor_udp_socket = -1;


    pthread_mutex_unlock(
        &monitor_mutex);


    if (socket_to_close >= 0)
    {
        close(socket_to_close);
    }


    pthread_join(monitor_thread,
                 NULL);


    printf("Active UDP monitoring stopped.\n");
}


/*
 * ============================================================
 * CLIENT THREAD
 * ============================================================
 */

void *client_thread(void *arg)
{
    int client_fd =
        *(int *)arg;


    free(arg);


    pthread_t thread_id =
        pthread_self();


    struct sockaddr_in client_addr;

    socklen_t client_len =
        sizeof(client_addr);


    /*
     * Get client address.
     */
    if (getpeername(client_fd,
                    (struct sockaddr *)&client_addr,
                    &client_len) == 0)
    {
        char client_ip[INET_ADDRSTRLEN];


        if (inet_ntop(AF_INET,
                      &client_addr.sin_addr,
                      client_ip,
                      sizeof(client_ip)) == NULL)
        {
            strcpy(client_ip,
                   "127.0.0.1");
        }


        printf("[Thread %lu] Controller connected from %s\n",
               (unsigned long)thread_id,
               client_ip);
    }
    else
    {
        printf("[Thread %lu] Controller connected.\n",
               (unsigned long)thread_id);
    }


    char buffer[BUFFER_SIZE];

    int authenticated = 0;


    while (1)
    {
        memset(buffer,
               0,
               sizeof(buffer));


        ssize_t bytes_received =
            recv(client_fd,
                 buffer,
                 sizeof(buffer) - 1,
                 0);


        if (bytes_received <= 0)
        {
            printf("[Thread %lu] Controller disconnected.\n",
                   (unsigned long)thread_id);

            break;
        }


        buffer[bytes_received] =
            '\0';


        buffer[
            strcspn(buffer,
                    "\r\n")
        ] = '\0';


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
                             "ERR AUTH SID:" SID "\n");


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
         * MONITOR START
         * ====================================================
         *
         * Format:
         *
         * MONITOR START <udp_port>
         *
         * The Controller's TCP source IP is used
         * as the UDP destination IP.
         */

        else if (strncmp(buffer,
                         "MONITOR START ",
                         14) == 0)
        {
            char port_text[32];


            memset(port_text,
                   0,
                   sizeof(port_text));


            if (sscanf(buffer + 14,
                       "%31s",
                       port_text) != 1 ||
                !is_number(port_text))
            {
                char response[BUFFER_SIZE];


                snprintf(response,
                         sizeof(response),
                         "ERR INVALID_PORT SID:%s\n",
                         SID);


                send_message(client_fd,
                             response);


                continue;
            }


            int udp_port =
                atoi(port_text);


            if (udp_port < 1024 ||
                udp_port > 65535)
            {
                char response[BUFFER_SIZE];


                snprintf(response,
                         sizeof(response),
                         "ERR INVALID_PORT SID:%s\n",
                         SID);


                send_message(client_fd,
                             response);


                continue;
            }


            /*
             * Determine Controller IP.
             */
            struct sockaddr_in peer_addr;

            socklen_t peer_len =
                sizeof(peer_addr);


            char controller_ip[INET_ADDRSTRLEN];


            memset(controller_ip,
                   0,
                   sizeof(controller_ip));


            if (getpeername(client_fd,
                            (struct sockaddr *)&peer_addr,
                            &peer_len) < 0)
            {
                strcpy(controller_ip,
                       "127.0.0.1");
            }
            else
            {
                if (inet_ntop(AF_INET,
                              &peer_addr.sin_addr,
                              controller_ip,
                              sizeof(controller_ip))
                    == NULL)
                {
                    strcpy(controller_ip,
                           "127.0.0.1");
                }
            }


            handle_monitor_start(
                client_fd,
                controller_ip,
                udp_port);
        }


        /*
         * ====================================================
         * MONITOR STOP
         * ====================================================
         */

        else if (strcmp(buffer,
                        "MONITOR STOP") == 0)
        {
            handle_monitor_stop(client_fd);
        }


        /*
         * ====================================================
         * QUIT
         * ====================================================
         */

        else if (strcmp(buffer,
                        "QUIT") == 0)
        {
            /*
             * Stop any active monitor first.
             */
            stop_monitor_if_running();


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


    /*
     * Stop monitoring if Controller
     * disconnects unexpectedly.
     */
    stop_monitor_if_running();


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
 * Main thread:
 *
 * 1. Creates TCP server.
 * 2. Listens on port 9410.
 * 3. Accepts multiple Controllers.
 * 4. Creates one pthread per Controller.
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
         * Allocate socket descriptor
         * for the new thread.
         */
        int *client_socket =
            malloc(sizeof(int));


        if (client_socket == NULL)
        {
            perror("malloc");

            close(client_fd);

            continue;
        }


        *client_socket =
            client_fd;


        pthread_t thread;


        if (pthread_create(&thread,
                           NULL,
                           client_thread,
                           client_socket) != 0)
        {
            perror("pthread_create");

            free(client_socket);

            close(client_fd);

            continue;
        }


        /*
         * Detach client thread because
         * the main thread does not need
         * to join individual clients.
         */
        pthread_detach(thread);


        printf("New client thread created.\n");
    }


    close(server_fd);

    return 0;
}
