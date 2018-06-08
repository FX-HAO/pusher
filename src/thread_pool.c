#include "server.h"
#include "thread_pool.h"


static void *thread_pool_cycle(void *data);
static int thread_pool_init(thread_pool_t *tp);
static void thread_pool_exit_handler(void *data);

static uint64_t thread_pool_task_id;

static int thread_pool_init(thread_pool_t *tp) {
    int             err;
    pthread_t       tid;
    pthread_attr_t  attr;
    int j;

    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->cond, NULL);
    pthread_attr_init(&attr);

    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (err) {
        serverLog(LL_WARNING, "pthread_attr_setdetachstate() failed");
        return C_ERR;
    }

#if 0
    err = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
    if (err) {
        serverLog(LL_WARNING, "pthread_attr_setstacksize() failed");
        return C_ERR;
    }
#endif

    for (j = 0; j < tp->thread_count; j++) {
        err = pthread_create(&tid, &attr, thread_pool_cycle, tp);
        if (err) {
            serverLog(LL_WARNING, "pthread_create() failed");
            return C_ERR;
        }
    }

    (void) pthread_attr_destroy(&attr);

    return C_OK;
}

thread_pool_t *thread_pool_create(char *name, int thread_count, int maxtasks) {
    thread_pool_t *tp;

    if ((tp = zmalloc(sizeof(*tp))) == NULL) return NULL;
    tp->tasks = listCreate();
    tp->name = name;
    tp->thread_count = thread_count;
    tp->maxtasks = maxtasks;
    thread_pool_init(tp);
    return tp;
}

void thread_pool_destroy(thread_pool_t *tp) {
    thread_task_t    task;
    volatile int  lock;

    memset(&task, 0, sizeof(thread_task_t));

    task.handler = thread_pool_exit_handler;
    task.data = (void *) &lock;

    lock = 1;

    if (thread_task_post(tp, &task) != C_OK) {
        return;
    }

    while (lock) {
        sched_yield();
    }

    pthread_cond_destroy(&tp->cond);
    pthread_mutex_destroy(&tp->mtx);
    zfree(tp);
}

static void thread_pool_exit_handler(void *data) {
    int *lock = data;

    *lock = 0;

    pthread_exit(0);
}

static void *thread_pool_cycle(void *data) {
    thread_pool_t *tp = data;

    int                 err;
    sigset_t            set;
    listNode            *head;
    thread_task_t       *task;

    serverLog(LL_DEBUG, "thread in pool \"%s\" started", tp->name);

    sigfillset(&set);

    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGFPE);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);

    err = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (err) {
        serverLog(LL_WARNING, "pthread_sigmask() failed");
        return NULL;
    }

    for ( ;; ) {
        pthread_mutex_lock(&tp->mtx);

        while (listLength(tp->tasks) == 0) {
            pthread_cond_wait(&tp->cond, &tp->mtx);
        }

        head = listFirst(tp->tasks);
        listDelNode(tp->tasks, head);
        task = listNodeValue(head);
        
        pthread_mutex_unlock(&tp->mtx);

        serverLog(LL_DEBUG, "run task #%u in thread pool \"%s\"",
            task->id, tp->name);

        task->handler(task->data);

        if (task->free) task->free(task->data);
        zfree(task);

        serverLog(LL_DEBUG, "complete task #%u in thread pool \"%s\"",
            task->id, tp->name);
    }
}

int thread_task_post(thread_pool_t *tp, thread_task_t *task) {
    pthread_mutex_lock(&tp->mtx);

    if (listLength(tp->tasks) >= tp->maxtasks) {
        pthread_mutex_unlock(&tp->mtx);

        serverLog(LL_WARNING, 
            "thread pool \"%s\" queue overflow", 
            tp->name);
        return C_ERR;
    }

    task->id = thread_pool_task_id++;

    pthread_cond_signal(&tp->cond);

    listAddNodeTail(tp->tasks, task);

    pthread_mutex_unlock(&tp->mtx);

    serverLog(LL_WARNING, 
        "task #%u added to thread pool \"%s\"",
        task->id, tp->name);

    return C_OK;
}
