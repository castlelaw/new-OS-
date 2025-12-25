#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <stdbool.h>
#include "threads/synch.h"

/* [P2-1 FIX] Circular dependency 방지용 전방 선언 */
struct semaphore;
struct file; /* 실행 파일 포인터 저장을 위해 필요 */

enum thread_status
  {
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_BLOCKED,
    THREAD_DYING
  };

typedef int tid_t;
#define TID_ERROR ((tid_t) -1)

#define PRI_MIN 0
#define PRI_DEFAULT 31
#define PRI_MAX 63

struct thread
  {
    tid_t tid;
    enum thread_status status;
    char name[16];
    uint8_t *stack;
    int priority;
    struct list_elem allelem;

    int64_t wakeup_tick;

    struct list_elem elem;

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;          /* Page directory. */

    /* [P2-1 FIX] Process Control Fields */
    int exit_status;            /* exit(status)로 전달된 값 */
    bool exited;                /* 정상적으로 exit 호출되었는지 여부 */
    bool load_success;          /* 프로그램 로드 성공 여부 (실패 시 종료 메시지 출력 안 함) */

    /* [P2-1 FIX] Executable file pointer for deny_write */
    struct file *bin_file;      /* 현재 실행 중인 파일 (종료 시 close 필요) */
    struct semaphore wait_sema;
#endif

    /* Owned by thread.c. */
    unsigned magic;             /* Detects stack overflow. */
  };

extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

struct thread *get_thread (tid_t tid);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */