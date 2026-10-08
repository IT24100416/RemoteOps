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
#include <time.h>
#include <stdarg.h>
#include <ctype.h>

#define PORT 9410
#define BUFFER_SIZE 1024
#define FILE_BUFFER_SIZE 4096

#define AUTH_TOKEN "OPS-0416"
#define SID "6140"

#define FILE_DIRECTORY "./agentfiles/IT24100416"
#define LOG_FILE "remoteops_IT24100416.log"

#define MONITOR_INTERVAL 5

/* Protocol error codes */
#define ERR_AUTH_FAILED          001
#define ERR_COMMAND_NOT_ALLOWED  002
#define ERR_NOT_AUTHENTICATED    003
#define ERR_FILE_TOO_LARGE       004
#define ERR_FILE_NOT_FOUND       005
#define ERR_INVALID_REQUEST      006
#define ERR_INVALID_SIZE         007
#define ERR_FILE_OPEN_FAILED     008
#define ERR_INTERNAL_ERROR       009
#define ERR_UNKNOWN_COMMAND      010
#define ERR_MONITOR_RUNNING      011
#define ERR_MONITOR_SOCKET       012
#define ERR_INVALID_ADDRESS      013
#define ERR_MONITOR_THREAD       014
#define ERR_MONITOR_NOT_RUNNING  015
#define ERR_INVALID_PORT         016

#define MAX_FILE_SIZE (100L * 1024L * 1024L)

/* ============================================================
 * GLOBAL UDP MONITOR STATE
 * ============================================================ */

static pthread_t monitor_thread;
static int monitor_running = 0;
static int monitor_udp_socket = -1;
static struct sockaddr_in monitor_destination;

static pthread_mutex_t monitor_mutex =
    PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * LOGGING
 * ============================================================ */

static pthread_mutex_t log_mutex =
    PTHREAD_MUTEX_INITIALIZER;

void log_event(const char *format, ...)
{
    FILE *fp;
    time_t now;
    struct tm time_info;
    char timestamp[64];
    va_list args;

    now = time(NULL);

    if (localtime_r(&now, &time_info) == NULL)
    {
        return;
    }

    strftime(timestamp,
             sizeof(timestamp),
             "%Y-%m-%d %H:%M:%S",
             &time_info);

    pthread_mutex_lock(&log_mutex);

    fp = fopen(LOG_FILE, "a");

    if (fp != NULL)
    {
        fprintf(fp, "[%s] ", timestamp);

        va_start(args, format);
        vfprintf(fp, format, args);
        va_end(args);

        fprintf(fp, "\n");

        fclose(fp);
    }

    pthread_mutex_unlock(&log_mutex);
}

/* ============================================================
 * SEND ALL
 * ============================================================ */

int send_all(int fd,
             const void *data,
             size_t length)
{
    size_t total = 0;
    const char *ptr = data;

    while (total < length)
    {
        ssize_t sent = send(fd,
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
 * SEND TEXT MESSAGE
 * ============================================================ */

int send_message(int fd,
                 const char *message)
{
    return send_all(fd,
                    message,
                    strlen(message));
}

/* ============================================================
 * RECEIVE ONE LINE
 * ============================================================ */

int recv_line(int fd,
              char *buffer,
              size_t size)
{
    size_t total = 0;

    if (size == 0)
    {
        return -1;
    }

    while (total < size - 1)
    {
        char c;

        ssize_t received =
            recv(fd, &c, 1, 0);

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
 * RECEIVE EXACT NUMBER OF BYTES
 * ============================================================ */

int recv_all(int fd,
             void *buffer,
             size_t length)
{
    size_t total = 0;
    char *ptr = buffer;

    while (total < length)
    {
        ssize_t received =
            recv(fd,
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
 * SYSINFO
 * ============================================================ */

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

    fp = fopen("/proc/loadavg", "r");

    if (fp != NULL)
    {
        fscanf(fp, "%lf", cpu_load);
        fclose(fp);
    }

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

    if (mem_total_kb >= mem_available_kb)
    {
        *mem_used_mb =
            (mem_total_kb - mem_available_kb) / 1024;
    }

    fp = fopen("/proc/uptime", "r");

    if (fp != NULL)
    {
        fscanf(fp,
               "%lf",
               uptime_seconds);

        fclose(fp);
    }
}

void handle_sysinfo(int client_fd)
{
    double cpu_load;
    unsigned long mem_used_mb;
    double uptime_seconds;

    char response[BUFFER_SIZE];

    collect_sysinfo(&cpu_load,
                    &mem_used_mb,
                    &uptime_seconds);

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

/* ============================================================
 * NUMBER CHECK
 * ============================================================ */

int is_number(const char *text)
{
    size_t i;

    if (text == NULL || *text == '\0')
    {
        return 0;
    }

    for (i = 0; text[i] != '\0'; i++)
    {
        if (!isdigit((unsigned char)text[i]))
        {
            return 0;
        }
    }

    return 1;
}

/* ============================================================
 * LISTPROC
 * ============================================================ */

void handle_listproc(int client_fd)
{
    DIR *dir;
    struct dirent *entry;

    char response[BUFFER_SIZE];
    size_t used = 0;

    int count = 0;

    used += (size_t)snprintf(response + used,
                             sizeof(response) - used,
                             "OK PROCS");

    dir = opendir("/proc");

    if (dir == NULL)
    {
        snprintf(response + used,
                 sizeof(response) - used,
                 " SID:%s\n",
                 SID);

        send_message(client_fd, response);
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        char path[512];
        char name[128];

        FILE *fp;

        if (!is_number(entry->d_name))
        {
            continue;
        }

        snprintf(path,
                 sizeof(path),
                 "/proc/%s/comm",
                 entry->d_name);

        fp = fopen(path, "r");

        if (fp == NULL)
        {
            continue;
        }

        if (fgets(name,
                  sizeof(name),
                  fp) != NULL)
        {
            int written;

            name[strcspn(name, "\r\n")] = '\0';

            written =
                snprintf(response + used,
                          sizeof(response) - used,
                          "%s%s:%s",
                          (count == 0) ? " " : ",",
                          entry->d_name,
                          name);

            if (written < 0 ||
                (size_t)written >=
                    sizeof(response) - used)
            {
                fclose(fp);
                break;
            }

            used += (size_t)written;
            count++;
        }

        fclose(fp);

        if (count >= 20)
        {
            break;
        }
    }

    closedir(dir);

    snprintf(response + used,
             sizeof(response) - used,
             " SID:%s\n",
             SID);

    send_message(client_fd,
                 response);
}

/* ============================================================
 * FILENAME VALIDATION
 * ============================================================ */

int valid_filename(const char *filename)
{
    size_t i;

    if (filename == NULL || *filename == '\0')
        return 0;

    if (strlen(filename) >= 256)
        return 0;

    if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
        return 0;

    for (i = 0; filename[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)filename[i];

        if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
            return 0;
    }

    return 1;
}

/* ============================================================
 * PUT
 * ============================================================ */

void handle_put(int client_fd,
                const char *filename,
                long filesize)
{
    char filepath[512];
    FILE *fp;
    char file_buffer[FILE_BUFFER_SIZE];
    long remaining = filesize;

    if (!valid_filename(filename))
    {
        send_message(client_fd,
                     "ERR 006 INVALID_REQUEST SID:" SID "\n");
        return;
    }

    if (filesize < 0)
    {
        send_message(client_fd,
                     "ERR 007 INVALID_SIZE SID:" SID "\n");
        return;
    }

    if (filesize > MAX_FILE_SIZE)
    {
        send_message(client_fd,
                     "ERR 004 FILE_TOO_LARGE SID:" SID "\n");
        return;
    }

    mkdir("agentfiles", 0755);
    mkdir(FILE_DIRECTORY, 0755);

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    fp = fopen(filepath, "wb");

    if (fp == NULL)
    {
        send_message(client_fd,
                     "ERR 008 FILE_OPEN_FAILED SID:" SID "\n");
        return;
    }

    while (remaining > 0)
    {
        size_t chunk = (remaining > (long)sizeof(file_buffer))
                     ? sizeof(file_buffer)
                     : (size_t)remaining;

        if (recv_all(client_fd, file_buffer, chunk) < 0)
        {
            fclose(fp);
            remove(filepath);
            return;
        }

        if (fwrite(file_buffer, 1, chunk, fp) != chunk)
        {
            fclose(fp);
            remove(filepath);
            send_message(client_fd,
                         "ERR 009 INTERNAL_ERROR SID:" SID "\n");
            return;
        }

        remaining -= (long)chunk;
    }

    if (fclose(fp) != 0)
    {
        remove(filepath);
        send_message(client_fd,
                     "ERR 009 INTERNAL_ERROR SID:" SID "\n");
        return;
    }

    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "OK FILE_RECEIVED %s SID:%s\n",
                 filename,
                 SID);

        send_message(client_fd, response);
    }
}

/* ============================================================
 * GET
 * ============================================================ */

void handle_get(int client_fd,
                const char *filename)
{
    char filepath[512];
    FILE *fp;
    long filesize;
    long remaining;
    char response[BUFFER_SIZE];
    char file_buffer[FILE_BUFFER_SIZE];

    if (!valid_filename(filename))
    {
        send_message(client_fd,
                     "ERR 006 INVALID_REQUEST SID:" SID "\n");
        return;
    }

    snprintf(filepath,
             sizeof(filepath),
             FILE_DIRECTORY "/%s",
             filename);

    fp = fopen(filepath, "rb");

    if (fp == NULL)
    {
        send_message(client_fd,
                     "ERR 005 FILE_NOT_FOUND SID:" SID "\n");
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        send_message(client_fd,
                     "ERR 009 INTERNAL_ERROR SID:" SID "\n");
        return;
    }

    filesize = ftell(fp);

    if (filesize < 0 || filesize > MAX_FILE_SIZE)
    {
        fclose(fp);
        send_message(client_fd,
                     "ERR 009 INTERNAL_ERROR SID:" SID "\n");
        return;
    }

    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        send_message(client_fd,
                     "ERR 009 INTERNAL_ERROR SID:" SID "\n");
        return;
    }

    /*
     * Exact GET protocol:
     *   OK FILE_SEND <filename> <filesize> SID:6140
     * followed immediately by exactly <filesize> raw bytes.
     * There is no trailing FILE_SENT line.
     */
    snprintf(response,
             sizeof(response),
             "OK FILE_SEND %s %ld SID:%s\n",
             filename,
             filesize,
             SID);

    if (send_message(client_fd, response) < 0)
    {
        fclose(fp);
        return;
    }

    remaining = filesize;

    while (remaining > 0)
    {
        size_t chunk = (remaining > (long)sizeof(file_buffer))
                     ? sizeof(file_buffer)
                     : (size_t)remaining;

        if (fread(file_buffer, 1, chunk, fp) != chunk)
        {
            fclose(fp);
            return;
        }

        if (send_all(client_fd, file_buffer, chunk) < 0)
        {
            fclose(fp);
            return;
        }

        remaining -= (long)chunk;
    }

    fclose(fp);
}

/* ============================================================
 * EXEC
 * ============================================================ */

void handle_exec(int client_fd,
                 const char *command)
{
    const char *allowed = NULL;

    if (strcmp(command, "DATE") == 0)
    {
        allowed = "date";
    }
    else if (strcmp(command, "UPTIME") == 0)
    {
        allowed = "uptime";
    }
    else if (strcmp(command, "DISKFREE") == 0)
    {
        allowed = "df -h / | tail -1";
    }
    else if (strcmp(command, "HOSTNAME") == 0)
    {
        allowed = "hostname";
    }
    else if (strcmp(command, "WHOAMI") == 0)
    {
        allowed = "whoami";
    }

    if (allowed == NULL)
    {
        send_message(client_fd,
                     "ERR 002 COMMAND_NOT_ALLOWED SID:" SID "\n");

        return;
    }

    FILE *fp = popen(allowed, "r");

    if (fp == NULL)
    {
        send_message(client_fd,
                     "ERR 009 INTERNAL_ERROR SID:" SID "\n");

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
        strcpy(result, "No output");
    }

    pclose(fp);

    result[strcspn(result, "\r\n")] = '\0';

    {
        char response[BUFFER_SIZE];

        snprintf(response,
                 sizeof(response),
                 "OK EXEC_RESULT %.900s SID:%s\n",
                 result,
                 SID);

        send_message(client_fd,
                     response);
    }
}

/* ============================================================
 * UDP MONITOR THREAD
 * ============================================================ */

void *monitor_thread_function(void *arg)
{
    (void)arg;

    while (1)
    {
        int running;
        int udp_socket;
        struct sockaddr_in destination;

        pthread_mutex_lock(&monitor_mutex);

        running = monitor_running;
        udp_socket = monitor_udp_socket;
        destination = monitor_destination;

        pthread_mutex_unlock(&monitor_mutex);

        if (!running)
        {
            break;
        }

        if (udp_socket < 0)
        {
            break;
        }

        double cpu_load;
        unsigned long mem_used_mb;
        double uptime_seconds;

        char message[BUFFER_SIZE];

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

        sleep(MONITOR_INTERVAL);
    }

    return NULL;
}

/* ============================================================
 * MONITOR START
 * ============================================================ */

void handle_monitor_start(int client_fd,
                          const char *ip_address,
                          int udp_port)
{
    pthread_mutex_lock(&monitor_mutex);

    if (monitor_running)
    {
        pthread_mutex_unlock(&monitor_mutex);

        send_message(client_fd,
                     "ERR 011 MONITOR_ALREADY_RUNNING SID:" SID "\n");

        return;
    }

    monitor_udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);

    if (monitor_udp_socket < 0)
    {
        pthread_mutex_unlock(&monitor_mutex);

        send_message(client_fd,
                     "ERR 012 MONITOR_SOCKET_ERROR SID:" SID "\n");

        return;
    }

    memset(&monitor_destination,
           0,
           sizeof(monitor_destination));

    monitor_destination.sin_family =
        AF_INET;

    monitor_destination.sin_port =
        htons((unsigned short)udp_port);

    if (inet_pton(AF_INET,
                  ip_address,
                  &monitor_destination.sin_addr) <= 0)
    {
        close(monitor_udp_socket);

        monitor_udp_socket = -1;

        pthread_mutex_unlock(&monitor_mutex);

        send_message(client_fd,
                     "ERR 013 INVALID_ADDRESS SID:" SID "\n");

        return;
    }

    monitor_running = 1;

    if (pthread_create(&monitor_thread,
                       NULL,
                       monitor_thread_function,
                       NULL) != 0)
    {
        monitor_running = 0;

        close(monitor_udp_socket);

        monitor_udp_socket = -1;

        pthread_mutex_unlock(&monitor_mutex);

        send_message(client_fd,
                     "ERR 014 MONITOR_THREAD_ERROR SID:" SID "\n");

        return;
    }

    pthread_mutex_unlock(&monitor_mutex);

    send_message(client_fd,
                 "OK MONITOR_STARTED SID:" SID "\n");

    printf("UDP monitoring started on %s:%d\n",
           ip_address,
           udp_port);
}

/* ============================================================
 * MONITOR STOP
 * ============================================================ */

void handle_monitor_stop(int client_fd)
{
    pthread_mutex_lock(&monitor_mutex);

    if (!monitor_running)
    {
        pthread_mutex_unlock(&monitor_mutex);

        send_message(client_fd,
                     "ERR 015 MONITOR_NOT_RUNNING SID:" SID "\n");

        return;
    }

    monitor_running = 0;

    int socket_to_close =
        monitor_udp_socket;

    monitor_udp_socket = -1;

    pthread_mutex_unlock(&monitor_mutex);

    if (socket_to_close >= 0)
    {
        close(socket_to_close);
    }

    pthread_join(monitor_thread,
                 NULL);

    send_message(client_fd,
                 "OK MONITOR_STOPPED SID:" SID "\n");

    printf("UDP monitoring stopped.\n");
}

/* ============================================================
 * STOP MONITOR WHEN CLIENT DISCONNECTS
 * ============================================================ */

void stop_monitor_if_running(void)
{
    pthread_mutex_lock(&monitor_mutex);

    if (!monitor_running)
    {
        pthread_mutex_unlock(&monitor_mutex);
        return;
    }

    monitor_running = 0;

    int socket_to_close =
        monitor_udp_socket;

    monitor_udp_socket = -1;

    pthread_mutex_unlock(&monitor_mutex);

    if (socket_to_close >= 0)
    {
        close(socket_to_close);
    }

    pthread_join(monitor_thread,
                 NULL);

    printf("Active UDP monitoring stopped.\n");
}

/* ============================================================
 * CLIENT THREAD
 * ============================================================ */

void *client_thread(void *arg)
{
    int client_fd =
        *(int *)arg;

    free(arg);

    pthread_t thread_id =
        pthread_self();

    char client_ip[INET_ADDRSTRLEN];

    struct sockaddr_in peer;
    socklen_t peer_len =
        sizeof(peer);

    memset(client_ip,
           0,
           sizeof(client_ip));

    if (getpeername(client_fd,
                    (struct sockaddr *)&peer,
                    &peer_len) == 0)
    {
        if (inet_ntop(AF_INET,
                      &peer.sin_addr,
                      client_ip,
                      sizeof(client_ip)) == NULL)
        {
            strcpy(client_ip, "unknown");
        }
    }
    else
    {
        strcpy(client_ip, "unknown");
    }

    printf("[Thread %lu] Controller connected from %s\n",
           (unsigned long)thread_id,
           client_ip);

    log_event("Thread %lu: Controller connected from %s",
              (unsigned long)thread_id,
              client_ip);

    char buffer[BUFFER_SIZE];

    int authenticated = 0;

    while (1)
    {
        if (recv_line(client_fd,
                      buffer,
                      sizeof(buffer)) < 0)
        {
            printf("[Thread %lu] Controller disconnected.\n",
                   (unsigned long)thread_id);

            log_event("Thread %lu: Controller disconnected",
                      (unsigned long)thread_id);

            break;
        }

        printf("[Thread %lu] Received: %s\n",
               (unsigned long)thread_id,
               buffer);

        log_event("Thread %lu: Command received: %s",
                  (unsigned long)thread_id,
                  buffer);

        /* ====================================================
         * AUTH
         * ==================================================== */

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

                send_message(client_fd,
                             "OK AUTHENTICATED SID:" SID "\n");

                printf("[Thread %lu] Authentication successful.\n",
                       (unsigned long)thread_id);

                log_event("Thread %lu: Authentication successful",
                          (unsigned long)thread_id);
            }
            else
            {
                send_message(client_fd,
                             "ERR 001 AUTH_FAILED SID:" SID "\n");

                printf("[Thread %lu] Authentication failed.\n",
                       (unsigned long)thread_id);

                log_event("Thread %lu: Authentication failed",
                          (unsigned long)thread_id);
            }

            continue;
        }

        /* ====================================================
         * AUTH CHECK
         * ==================================================== */

        if (!authenticated)
        {
            send_message(client_fd,
                         "ERR 003 NOT_AUTHENTICATED SID:" SID "\n");

            log_event("Thread %lu: Rejected unauthenticated command",
                      (unsigned long)thread_id);

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
            long filesize;

            if (sscanf(buffer + 4,
                       "%255s %ld",
                       filename,
                       &filesize) != 2)
            {
                send_message(client_fd,
                             "ERR 006 INVALID_REQUEST SID:" SID "\n");

                continue;
            }

            handle_put(client_fd,
                       filename,
                       filesize);

            log_event("Thread %lu: PUT %s (%ld bytes)",
                      (unsigned long)thread_id,
                      filename,
                      filesize);

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
                send_message(client_fd,
                             "ERR 006 INVALID_REQUEST SID:" SID "\n");

                continue;
            }

            handle_get(client_fd,
                       filename);

            log_event("Thread %lu: GET %s",
                      (unsigned long)thread_id,
                      filename);

            continue;
        }

        /* ====================================================
         * SYSINFO
         * ==================================================== */

        if (strcmp(buffer,
                   "SYSINFO") == 0)
        {
            handle_sysinfo(client_fd);

            log_event("Thread %lu: SYSINFO",
                      (unsigned long)thread_id);

            continue;
        }

        /* ====================================================
         * LISTPROC
         * ==================================================== */

        if (strcmp(buffer,
                   "LISTPROC") == 0)
        {
            handle_listproc(client_fd);

            log_event("Thread %lu: LISTPROC",
                      (unsigned long)thread_id);

            continue;
        }

        /* ====================================================
         * EXEC
         * ==================================================== */

        if (strncmp(buffer,
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

            log_event("Thread %lu: EXEC %s",
                      (unsigned long)thread_id,
                      command);

            continue;
        }

        /* ====================================================
         * MONITOR START
         * ==================================================== */

        if (strncmp(buffer,
                    "MONITOR START ",
                    14) == 0)
        {
            char port_text[32];
            int udp_port;

            memset(port_text,
                   0,
                   sizeof(port_text));

            if (sscanf(buffer + 14,
                       "%31s",
                       port_text) != 1 ||
                !is_number(port_text))
            {
                send_message(client_fd,
                             "ERR 016 INVALID_PORT SID:" SID "\n");

                continue;
            }

            udp_port = atoi(port_text);

            if (udp_port < 1024 ||
                udp_port > 65535)
            {
                send_message(client_fd,
                             "ERR 016 INVALID_PORT SID:" SID "\n");

                continue;
            }

            struct sockaddr_in addr;

            socklen_t addr_len =
                sizeof(addr);

            char controller_ip[INET_ADDRSTRLEN];

            memset(controller_ip,
                   0,
                   sizeof(controller_ip));

            if (getpeername(client_fd,
                            (struct sockaddr *)&addr,
                            &addr_len) < 0)
            {
                strcpy(controller_ip,
                       "127.0.0.1");
            }
            else
            {
                inet_ntop(AF_INET,
                          &addr.sin_addr,
                          controller_ip,
                          sizeof(controller_ip));
            }

            handle_monitor_start(client_fd,
                                  controller_ip,
                                  udp_port);

            log_event("Thread %lu: MONITOR START %s:%d",
                      (unsigned long)thread_id,
                      controller_ip,
                      udp_port);

            continue;
        }

        /* ====================================================
         * MONITOR STOP
         * ==================================================== */

        if (strcmp(buffer,
                   "MONITOR STOP") == 0)
        {
            handle_monitor_stop(client_fd);

            log_event("Thread %lu: MONITOR STOP",
                      (unsigned long)thread_id);

            continue;
        }

        /* ====================================================
         * QUIT
         * ==================================================== */

        if (strcmp(buffer,
                   "QUIT") == 0)
        {
            stop_monitor_if_running();

            send_message(client_fd,
                         "OK BYE SID:" SID "\n");

            printf("[Thread %lu] Controller requested disconnect.\n",
                   (unsigned long)thread_id);

            log_event("Thread %lu: Controller requested disconnect",
                      (unsigned long)thread_id);

            break;
        }

        /* ====================================================
         * UNKNOWN
         * ==================================================== */

        send_message(client_fd,
                     "ERR 010 UNKNOWN_COMMAND SID:" SID "\n");

        log_event("Thread %lu: Unknown command",
                  (unsigned long)thread_id);
    }

    stop_monitor_if_running();

    close(client_fd);

    printf("[Thread %lu] Client thread finished.\n",
           (unsigned long)thread_id);

    log_event("Thread %lu: Client thread finished",
              (unsigned long)thread_id);

    return NULL;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    server_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int option = 1;

    setsockopt(server_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));

    memset(&server_addr,
           0,
           sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    server_addr.sin_port =
        htons(PORT);

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("RemoteOps Agent started.\n");
    printf("Listening on TCP port %d...\n",
           PORT);

    log_event("RemoteOps Agent started on TCP port %d",
              PORT);

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

        pthread_detach(thread);

        printf("New client thread created.\n");
    }

    close(server_fd);

    return 0;
}
