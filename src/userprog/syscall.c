#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h" 
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "console.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

static void check_user_vaddr (const void *vaddr);

/* 파일 시스템 접근을 위한 락. (filesys 디렉터리가 스레드 안전하지 않으므로 필요) */
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

/* 공통 종료 헬퍼: exit_status 설정 + thread_exit */
static void
exit_process (int status)
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;
  thread_exit ();
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock); // 파일 시스템 락 초기화
}

/* * 유효하지 않은 사용자 가상 주소에 접근 시 현재 프로세스를 종료하는 헬퍼 함수*/
static void
check_user_vaddr (const void *vaddr)
{
  struct thread *cur = thread_current();
  // 널 포인터이거나, 커널 가상 주소 공간에 있거나, 유효한 사용자 주소가 아닌 경우
  if (vaddr == NULL || !is_user_vaddr(vaddr))
    exit_process(-1);
  if(pagedir_get_page (cur->pagedir, vaddr) == NULL)
    exit_process(-1);

}

static void
syscall_handler (struct intr_frame *f) 
{
  // 1. 스택 포인터(f->esp) 및 시스템 콜 번호의 주소 유효성 검사
  check_user_vaddr(f->esp);

  // 시스템 콜 번호 읽기
  int syscall_no = *(int *)f->esp;
  int status;
  const char *cmd_line;
  
  switch (syscall_no)
    {
    case SYS_HALT: //SYS_HALT와 일치하면, 이 시스템 콜 처리 (추가 인자 없음)
      shutdown_power_off(); //시뮬레이션된 Pintos 시스템을 종료
      break;

    case SYS_EXIT: //SYS_EXIT와 일치하면, 이 시스템 콜 처리
      // 2. 인자 1 (status)의 주소 유효성 검사 (f->esp + 4)
      check_user_vaddr(f->esp + 4); 
      status = *(int *)(f->esp + 4);
      exit_process (status); //프로세스 종료료
      break;

    case SYS_WRITE:
    {
      check_user_vaddr(f->esp + 4); //fd 주소 유효성 검사
      check_user_vaddr(f->esp + 8); //buffer 주소 유효성 검사
      check_user_vaddr(f->esp + 12); //size 주소 유효성 검사

      
        int fd = *(int *)(f->esp + 4);
        const void *buffer = *(const void **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);

        if (buffer == NULL)
          exit_process(-1);
        for (unsigned i = 0; i < size; i++)
          check_user_vaddr(buffer + i); //버퍼 포인터 유효성 검사
        
        if (fd == STDOUT_FILENO) //표준 출력
        {
            putbuf(buffer, size); //콘솔에 버퍼 출력
            f->eax = size; //쓰기 성공한 바이트 수 반환
        }
        else
        {
            exit_process(-1); //지원하지 않는 파일 디스크립터는 프로세스 종료
        }

    }
    break;

    case SYS_EXEC: //SYS_EXEC와 일치하면, 이 시스템 콜 처리
      // 2. 인자 1 (cmd_line 주소)의 주소 유효성 검사 (f->esp + 4)
      check_user_vaddr(f->esp + 4); //포인터가 저장된 주소 유효성 검사
      cmd_line = *(const char **)(f->esp + 4);

      // 3. cmd_line 문자열의 시작 주소 유효성 검사
      check_user_vaddr(cmd_line); 

      // 파일 시스템 접근 동기화
      lock_acquire(&filesys_lock); //락 획득
      tid_t tid = process_execute(cmd_line); //프로세스 실행
      lock_release(&filesys_lock); //락 해제

      // 반환 값 설정
      f->eax = tid; //(EAX 레지스터 사용하여 전달)
      break;
      
    default:
      // 정의되지 않은 시스템 콜은 프로세스를 종료
      exit_process(-1);
      break;
    }
}
