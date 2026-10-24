/*
 * ptdemo — fixture workload for ProsperoTrace.
 *
 * Writes its pid, load base and _DYNAMIC to /data/ptdemo.pid so the agent can find it,
 * waits PTDEMO_DELAY seconds for the agent to arm, then for PTDEMO_SECONDS
 * runs a TCP echo exchange with the host (PTDEMO_HOST:PTDEMO_PORT), file
 * I/O under /data, heap churn and a worker thread. Deterministic per
 * iteration, so two captures of it are directly comparable.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PTDEMO_HOST
#define PTDEMO_HOST "192.168.10.1"
#endif
#define PTDEMO_PORT 9030
#define PTDEMO_DELAY 8
#define PTDEMO_SECONDS 5

extern char __ehdr_start[];
extern char _DYNAMIC[];

static void *worker(void *arg)
{
    volatile int *n = arg;
    for (int i = 0; i < 100; i++)
        (*n)++;
    return 0;
}

static void echo_once(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PTDEMO_PORT);
    inet_pton(AF_INET, PTDEMO_HOST, &sa.sin_addr);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        char buf[64];
        send(s, "prospero-echo\n", 14, 0);
        recv(s, buf, sizeof(buf), 0);
    }
    close(s);
}

static void file_once(void)
{
    char buf[128];
    int fd = open("/data/ptdemo.tmp", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return;
    write(fd, "prosperotrace fixture\n", 22);
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, sizeof(buf));
    close(fd);
}

int main(void)
{
    FILE *log = fopen("/data/ptdemo.log", "w");
    if (log) {
        fprintf(log, "ptdemo pid %d base %p\n", getpid(), (void *)__ehdr_start);
        fclose(log);
    }
    FILE *f = fopen("/data/ptdemo.pid", "w");
    if (f) {
        fprintf(f, "%d 0x%llx 0x%llx\n", getpid(), (unsigned long long)(uintptr_t)__ehdr_start,
                (unsigned long long)(uintptr_t)_DYNAMIC);
        fclose(f);
    }
    sleep(PTDEMO_DELAY);

    time_t end = time(0) + PTDEMO_SECONDS;
    volatile int counter = 0;
    while (time(0) < end) {
        echo_once();
        file_once();
        char *volatile p = malloc(4096);
        if (p)
            p[0] = 1;
        p = realloc(p, 8192);
        if (p)
            p[8191] = 2;
        free(p);
        pthread_t t;
        if (pthread_create(&t, 0, worker, (void *)&counter) == 0)
            pthread_join(t, 0);
        usleep(100000);
    }
    unlink("/data/ptdemo.tmp");
    unlink("/data/ptdemo.pid");
    log = fopen("/data/ptdemo.log", "a");
    if (log) {
        fprintf(log, "ptdemo done\n");
        fclose(log);
    }
    sleep(1);       /* let the tracer drain its ring before the process goes */
    return 0;
}
