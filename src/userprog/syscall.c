#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h" 
#include "threads/synch.h"

static void check_user_vaddr (const void *vaddr);

/* 파일 시스템 접근을 위한 락. (filesys 디렉터리가 스레드 안전하지 않으므로 필요) */
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

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
  // 널 포인터이거나, 커널 가상 주소 공간에 있거나, 유효한 사용자 주소가 아닌 경우
  if (!vaddr || !is_user_vaddr(vaddr) || vaddr < (void *) 0x08048000) 
  {
    // 잘못된 인자 처리: 프로세스를 종료하고 -1 반환
    thread_current()->exit_status = -1; 
    
    // 프로세스 종료 메시지 출력
    printf("%s: exit(%d)\n", thread_current()->name, -1);

    thread_exit();
  }
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
    case SYS_HALT: //SYS_HALT와 일치하면, 이 시스템 콜 처리
      shutdown_power_off(); //시뮬레이션된 Pintos 시스템을 종료
      break;

    case SYS_EXIT:
      /* void exit (int status) */
      // 2. 인자 1 (status)의 주소 유효성 검사 (f->esp + 4)
      check_user_vaddr(f->esp + 4);
      status = *(int *)(f->esp + 4);

      // 프로세스 종료 메시지 출력 및 종료
      thread_current()->exit_status = status;
      printf("%s: exit(%d)\n", thread_current()->name, status);
      thread_exit();
      break;

    case SYS_EXEC:
      /* pid_t exec (const char *cmd_line) */
      // 2. 인자 1 (cmd_line 주소)의 주소 유효성 검사 (f->esp + 4)
      check_user_vaddr(f->esp + 4);
      cmd_line = *(const char **)(f->esp + 4);

      // 3. cmd_line 문자열의 시작 주소 유효성 검사
      check_user_vaddr(cmd_line);

      // 파일 시스템 접근 동기화
      lock_acquire(&filesys_lock);
      tid_t tid = process_execute(cmd_line);
      lock_release(&filesys_lock);

      // 반환 값 설정
      f->eax = tid;
      break;
      
    default:
      // 정의되지 않은 시스템 콜은 프로세스를 종료
      thread_current()->exit_status = -1;
      printf("%s: exit(%d)\n", thread_current()->name, -1);
      thread_exit();
      break;
    }
}
