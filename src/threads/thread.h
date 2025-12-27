#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <stdbool.h>
#include "threads/synch.h" /* [P2-1 FIX] Semaphore */

struct file; 

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

/* [P2-1 FIX] 자식 프로세스 정보를 담는 구조체
   - 부모 thread의 children 리스트에 매달려 있음
   - wait()는 "자식 1개당 딱 1번"만 가능해야 하므로 waited 플래그 필요
   - 자식이 이미 종료했는지 여부(exited)를 기록해두면 process_wait 구현이 더 안전해짐 */
struct child_process {
    tid_t tid;                  /* 자식 스레드 ID */
    int exit_status;            /* 자식 종료 코드 (exit(status) 또는 -1) */

    bool waited;                /* 부모가 이미 wait() 했는지 (1회 제한용) */
    bool exited;                /* 자식이 종료했는지 여부 (자식 exit 시 true) */

    struct semaphore wait_sema; /* 자식 종료 대기용 세마포어 (자식 exit에서 up) */
    struct list_elem elem;      /* 부모의 children 리스트 연결용 */
};

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
    uint32_t *pagedir;
    int exit_status;           /* 나의 종료 코드 */
    bool load_success;
    struct file *bin_file;     /* 실행 중인 파일 */

    /* [P2-1 FIX] 자식 관리 필드 */
    struct list children;      /* 자식 프로세스 목록 (struct child_process) */
    struct child_process *cp;  /* 나 자신의 메타데이터 포인터 (부모가 만들어준 것) */
    struct file *fd_table[128]; /* syscall.c에서 2~127을 사용 *
#endif

    unsigned magic;
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
