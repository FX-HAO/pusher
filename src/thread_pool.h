#ifndef __THREAD_POOL_H_
#define __THREAD_POOL_H_

#include <pthread.h>

#include "adlist.h"


typedef struct thread_task_s {
    uint64_t id;
    void *data;
    void (*handler)(void *data);
    void (*free)(void *data);
} thread_task_t;

typedef struct thread_pool_s {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    char *name;
    int thread_count;
    list *tasks;
    unsigned int maxtasks;
} thread_pool_t;

thread_pool_t *thread_pool_create(char *name, int thread_count, int maxtasks);
void thread_pool_destroy(thread_pool_t *tp);
int thread_task_post(thread_pool_t *tp, thread_task_t *task);

#endif /* __THREAD_POOL_H_ */
