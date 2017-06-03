#include <ev.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/shm.h>
#define __USE_GNU
#include <sched.h>


#define err(fmt,...) fprintf(stderr, "[pid:%d, line: %3d] "fmt" %s\n", getpid(), __LINE__, ##__VA_ARGS__, strerror(errno))

#ifndef container_of
# define container_of(ptr, type, field) ((type*)((char*)(ptr) - ((char*)&((type*)0)->field)))
#endif


struct worker_s {
    struct ev_child cwatcher;
    int worker_id;
    int listen_fd;
    pid_t pid;
    struct listener_s* listener;
    int starttime;
};

struct listener_s {
    int argc;
    char** argv;
    int stop_moniter;
    int worker_count;
    struct worker_s* workers;
    int shm_id;
    int shm_size;
};

int start_listen(char* ip, uint16_t port) {
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd<0) {
        return -1;
    }
    struct sockaddr_in srvaddr;
    memset(&srvaddr, 0, sizeof(struct sockaddr_in));
    srvaddr.sin_family = PF_INET;
    srvaddr.sin_addr.s_addr = inet_addr(ip);
    srvaddr.sin_port = htons(port);

    int one = 1;
#ifdef SO_REUSEPORT
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    fcntl(listenfd, F_SETFL, fcntl(listenfd, F_GETFL) | O_NONBLOCK);

    if (bind(listenfd, (struct sockaddr*)&srvaddr, sizeof(struct sockaddr)) < 0) {
        err("bind");
        return -1;
    }
    if (listen(listenfd, 511) < 0) {
        err("listen");
        return -1;
    }

    return listenfd;
}

pid_t spawn_worker(struct worker_s* worker) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    worker->starttime = (int)tv.tv_sec;
    struct listener_s* listener = worker->listener;
    pid_t pid = fork();
    if (pid < 0) {
        err("fork");
        return -1;
    } else if (pid == 0) { // child
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(worker->worker_id, &mask);
        sched_setaffinity(getpid(), sizeof(mask), &mask);

        struct worker_s* wks= listener->workers;
        int wkid = worker->worker_id;
        while (wkid--) {
            close(wks[wkid].listen_fd);
        }

        char* arg[] = {listener->argv[3], NULL};

        char envlistenfd[64];
        sprintf(envlistenfd, "LISTEN_FD=%d", worker->listen_fd);
        char envworkerid[64];
        sprintf(envworkerid, "WKR_ID=%d", worker->worker_id);
        char envworkercount[64];
        sprintf(envworkercount, "WKR_COUNT=%d", listener->worker_count);
        char envshmid[64];
        sprintf(envshmid, "SHM_ID=%d", listener->shm_id);
        char envshmsize[64];
        sprintf(envshmsize, "SHM_SIZE=%d", listener->shm_size);
        char* env[] = {envlistenfd, envworkerid, envworkercount, envshmid, envshmsize, NULL};

        execve(listener->argv[3], arg, env);

        exit(EXIT_FAILURE); // will never reach
    } else {
        return pid;
    }
}

void exit_cb(struct ev_loop* loop, struct ev_child* cwatcher, int status) {
    struct worker_s* worker = container_of(cwatcher, struct worker_s, cwatcher);
    ev_child_stop(loop, cwatcher);

    err("worker[pid:%d] exit with status:%d, stop_moniter:%d", worker->pid, cwatcher->rstatus, worker->listener->stop_moniter);

    struct timeval tv;
    gettimeofday(&tv, 0);

    if (worker->listener->stop_moniter || 2 > ((int)tv.tv_sec - worker->starttime)) {
        return;
    }

    worker->pid = spawn_worker(worker);
    if (-1 == worker->pid) {
        err("spawn worker failed, worker_id:%d", worker->worker_id);
        exit(EXIT_FAILURE);
    }

    err("worker %d restart, new pid: %d", worker->worker_id, worker->pid);

    ev_child_set(cwatcher, worker->pid, 0);
    ev_child_start(loop, cwatcher);
}

int setup_workers(struct ev_loop* loop, struct listener_s* listener) {
    char* ip = listener->argv[1];
    uint16_t port = (uint16_t)atoi(listener->argv[2]);

    int listen_fd;
#ifndef SO_REUSEPORT
    listen_fd = start_listen(ip, port);
#endif

    pid_t pid;
    int i;
    struct worker_s* workers = listener->workers;
    for (i = 0; i < listener->worker_count; i++) {
#ifdef SO_REUSEPORT
        listen_fd = start_listen(ip, port);
#endif
        workers[i].listen_fd = listen_fd;
        workers[i].worker_id = i;
        workers[i].listener = listener;
        pid = spawn_worker(&workers[i]);
        if (pid < 0) {
            return -1;
        }
        workers[i].pid = pid;
        ev_child_init(&workers[i].cwatcher, exit_cb, pid, 0);
        ev_child_start(loop, &workers[i].cwatcher);
    }

    return 0;
}

void signal_workers(struct listener_s* listener, int sig) {
    int worker_count = listener->worker_count;
    for (;worker_count--;) {
        err("pid:%d",listener->workers[worker_count].pid);
        kill(listener->workers[worker_count].pid, sig);
    }
}

void listener_stop(struct ev_loop* loop, struct ev_signal* quitwatcher, int status) {
    struct listener_s* listener = (struct listener_s*)ev_userdata(loop);
    err("listener_stop:%d", listener->stop_moniter);
    listener->stop_moniter = 1;
    signal_workers(listener, SIGQUIT);
}

void restart_workers(struct ev_loop* loop, struct ev_signal* hupwatcher, int status) {
    struct listener_s* listener = (struct listener_s*)ev_userdata(loop);
    listener->stop_moniter = 0;
    signal_workers(listener, SIGQUIT);
}

int shm_alloc(key_t key, size_t size) {
    if (!key) {
        key = IPC_PRIVATE;
    }
    int id = shmget(key, size, (SHM_R|SHM_W|IPC_CREAT));
    if (id == -1) {
        perror("shmget");
    }
    return id;
}
void* shm_attach(int shmid) {
    void* addr = shmat(shmid, NULL, 0);
    if (addr == (void*)-1) {
        return NULL;
    }
    return addr;
}
int shm_detach(void* addr) {
    return shmdt(addr);
}
void shm_free(int shmid) {
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl(IPC_RMID)");
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("Usage: %s ip port path2worker [shmsize]\n", argv[0]);
        return EXIT_FAILURE;
    }


    struct listener_s listener;
    listener.argc = argc;
    listener.argv = argv;
    listener.stop_moniter = 0;
    listener.shm_id = -1;
    listener.shm_size = -1;

    if (argc > 4) {
        listener.shm_size = atoi(argv[4]);
        if (listener.shm_size > 0) {
            listener.shm_id = shm_alloc(0, listener.shm_size);
        }
        if (listener.shm_id != -1) {
            shm_free(listener.shm_id); // lazy free
        }
    }


    // get loop
    struct ev_loop* loop = ev_default_loop(EVBACKEND_EPOLL);
    ev_set_userdata(loop, &listener);


    // setup signal handler
    struct ev_signal quitwatcher;
    struct ev_signal hupwatcher;
    ev_signal_init(&quitwatcher, listener_stop, SIGQUIT);
    ev_signal_init(&hupwatcher, restart_workers, SIGHUP);
    ev_signal_start(loop, &quitwatcher);
    ev_unref(loop); // 将loop中的watchercnt--,保证不停止此watcher的情况下loop也能正常退出
    ev_signal_start(loop, &hupwatcher);
    ev_unref(loop); // 将loop中的watchercnt--,保证不停止此watcher的情况下loop也能正常退出


    // get cpu number
    listener.worker_count = (int)sysconf(_SC_NPROCESSORS_CONF);


    // init workers
    struct worker_s workers[listener.worker_count];
    listener.workers = &workers[0];

    if (0 != setup_workers(loop, &listener)) {
        return EXIT_FAILURE;
    }


    int r = ev_run(loop, 0);


    ev_ref(loop);
    ev_signal_stop(loop, &quitwatcher);
    ev_ref(loop);
    ev_signal_stop(loop, &hupwatcher);


    return r;
}
