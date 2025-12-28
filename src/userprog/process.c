#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

extern struct lock filesys_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool push_arguments (void **esp, const char *cmdline);
static bool install_page (void *upage, void *kpage, bool writable);

/* 프로세스 실행 간 동기화를 위한 구조체 [cite: 14] */
struct exec_info {
  char *cmdline;              //시스템 콜에 전달된 전체 명령행 문자열을 자식 스레드에 전달하기 위해 저장
  struct semaphore done;      /* 자식의 로드 완료를 대기하기 위한 세마포어 [cite: 13] */
  bool success;               /* 로딩 성공 여부 [cite: 12] */
  struct child_process *cp;   /* 부모-자식 간 공유할 메타데이터 [cite: 21] */
};

/* Starts a new thread running a user program loaded from FILENAME. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy; //명령행 문자열 저장
  tid_t tid; //생성된 스레드의 id

  /* 파일 이름 복사 */
  fn_copy = palloc_get_page (0); //커널의 새 페이지 할당
  if (fn_copy == NULL) //페이지 할당 성공 검사
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE); //file_name을 커널 페이지로 복사

  /* 프로그램 이름만 추출 (thread_create용) */
  char name_copy[16];
  strlcpy (name_copy, file_name, sizeof name_copy);
  char *save_ptr; 
  char *prog_name = strtok_r (name_copy, " ", &save_ptr); //순수 프로그램 이름

  /* 실행 정보 및 자식 메타데이터 할당 [cite: 165] */
  struct exec_info *info = malloc (sizeof (struct exec_info)); //부모와 자식 간의 동기화 상태를 추적할 동적 메모리 할당
  struct child_process *cp = malloc (sizeof (struct child_process)); //동적 메모리에 자원 할당
  
  if (info == NULL || cp == NULL) //동적 메모리 할당 성공 여부 
    {
      if (fn_copy) palloc_free_page (fn_copy); //할당 실패시 커널 페이지 즉시 반환
      if (info) free (info); //info 해제
      if (cp) free (cp); //cp해제
      return TID_ERROR; //에러값 반환
    }
  
  info->cmdline = fn_copy; //복사한 명령행 info구조체에 저장
  info->success = false; //실패로 초기화
  sema_init (&info->done, 0); //세마포어 초기화

  /* 자식 메타데이터 초기화 [cite: 19] */
  cp->tid = TID_ERROR;
  cp->exit_status = -1;
  cp->waited = false;         /* wait 중복 호출 방지 [cite: 24, 25] */
  cp->exited = false;
  cp->ref_cnt = 2;            /* 부모와 자식이 각각 하나씩 참조 [cite: 28, 165] */
  sema_init (&cp->wait_sema, 0); //부모가 wait(pid)를 호출했을 때 자식이 죽을 때까지 기다리기 위한 전용 세마포어
  
  /* 부모의 자식 리스트에 추가 [cite: 20] */
  list_push_back (&thread_current ()->children, &cp->elem);
  info->cp = cp; //동기화 구조체에 자식 메타데이터 주소를 연결

  /* 스레드 생성 */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, info);
  
  if (tid == TID_ERROR)
    {
      list_remove (&cp->elem);
      free (cp);
      palloc_free_page (fn_copy);
      free (info);
      return TID_ERROR;
    }
  
  cp->tid = tid; //성공적으로 생성된 스레드의 ID를 메타데이터에 기록

  /* 자식이 load를 성공/실패 할 때까지 대기  */
  sema_down (&info->done);

  /* 자식 로드 실패 시 [cite: 12] */
  if (!info->success)
    tid = TID_ERROR;

  free (info);
  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *aux_)
{
  struct exec_info *info = aux_; //info를 가져옴
  char *cmdline = info->cmdline; //로컬변수에 info에 저장된 명령행 문자열 주소 저장
  struct intr_frame if_; //cpu 레지스터 상태 저장 변수
  bool success;

#ifdef USERPROG
  struct thread *cur = thread_current (); 
  cur->exit_status = -1; //초기 상태
  cur->load_success = false; 
  cur->cp = info->cp;         /* 부모가 전달한 메타데이터 연결 */
#endif

  memset (&if_, 0, sizeof if_); //인터럽트 프레임 0
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG; //
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* 파일 로드 시 시스템 락 보호 [cite: 165] */
  lock_acquire (&filesys_lock); 
  success = load (cmdline, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);

  if (success) 
    {
      /* 스택에 인자 설정 [cite: 179, 180] */
      if (!push_arguments (&if_.esp, cmdline))
        success = false;
    }

  palloc_free_page (cmdline); 

  info->success = success; //부모가 기다리고 있는 info 구조체에 로드 결과를 기록
#ifdef USERPROG
  cur->load_success = success; //현재 스레드 정보에도 로드 성공 여부
#endif

  /* 부모에게 로드 결과 전달 [cite: 13] */
  sema_up (&info->done); //로드 과정이 끝났음을 부모에게 알려, 대기 중이던 부모 스레드가 exec 호출에서 깨어나게 함

  if (!success) 
    thread_exit ();

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory"); //설정된 인터럽트 프레임을 사용하여 사용자 모드로 점프하고 프로그램을 실행
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status. */
int
process_wait (tid_t child_tid)
{
  struct thread *cur = thread_current ();
  struct list_elem *e; //자식리스트 순회 변수

  /* 직계 자식인지 확인 [cite: 20, 21] */
  for (e = list_begin (&cur->children); e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_process *cp = list_entry (e, struct child_process, elem); //자식의 cp주소 찾기
      if (cp->tid == child_tid) 
        {
          /* 이미 wait를 호출했다면 실패 [cite: 24, 25] */
          if (cp->waited)
            return -1;
          cp->waited = true; //wait호출 명시 (중복호출방지)

          /* 자식이 종료될 때까지 대기 [cite: 15, 17] */
          if (!cp->exited)
            sema_down (&cp->wait_sema);

          int status = cp->exit_status; //자식이 종료되고, 부모가 깨어났다면 자식의 종료코드 exit_status 가져옴

          /* 리스트에서 제거 및 부모의 참조 해제 [cite: 28] */
          list_remove (&cp->elem);
          cp->ref_cnt--; //참조횟수 감소
          if (cp->ref_cnt == 0) //참조횟수가 0이 되면 cp해제
            free (cp);
          
          return status;
        }
    }
    
  return -1; /* 직계 자식이 아님 [cite: 20] */
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd; //프로세스 페이지 디렉터리 주소 변수

#ifdef USERPROG
  if (cur->pagedir != NULL) //현재 프로세스가 사용자 프로세스인지 확인
    {
      /* 자식이 exit()에 전달한 종료 상태 출력 [cite: 17] */
      printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
    }
    
  if (cur->bin_file != NULL) //현재 실행중인 파일 존재 확인
    {
      lock_acquire (&filesys_lock);
      file_close (cur->bin_file); //파일 닫기
      lock_release (&filesys_lock);
      cur->bin_file = NULL; //실행 포인터 해지
    }

  /* 부모에게 종료 알림 [cite: 16, 19] */
  if (cur->cp != NULL) //부모와 공유하는 메타데이터의 여부 확인
    {
      cur->cp->exit_status = cur->exit_status; //자신의 종료상태 저장->부모가 알 수 있도록
      cur->cp->exited = true; //종료상태 표시
      sema_up (&cur->cp->wait_sema); //부모 깨우기
      
      /* 자신의 메타데이터 참조 해제 [cite: 28, 29] */
      cur->cp->ref_cnt--; //참조횟수 감소
      if (cur->cp->ref_cnt == 0)
        free (cur->cp); //동적 메모리에 할당된 cp해제
      cur->cp = NULL; //cp초기화
    }

  /* 내가 생성한 자식들 정리 (부모의 책임) [cite: 27, 28] */
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children); //자식리스트 하나씩 꺼내기
      struct child_process *cp = list_entry (e, struct child_process, elem); //꺼낸 요소로부터 메타데이터 꺼내기
      cp->ref_cnt--; //참조횟수 감소
      if (cp->ref_cnt == 0)
        free (cp); //cp해제
    }
#endif

  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL; //스레드 구조체 내의 페이지 디렉터리 연결을 끊음
      pagedir_activate (NULL); //현재 페이지 테이블을 커널전용으로 변경
      pagedir_destroy (pd);  //페이지 디렉토리 파괴
    }
}

void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ELF 관련 타입 정의 */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;
struct Elf32_Ehdr {
    unsigned char e_ident[16]; Elf32_Half e_type; Elf32_Half e_machine;
    Elf32_Word e_version; Elf32_Addr e_entry; Elf32_Off e_phoff;
    Elf32_Off e_shoff; Elf32_Word e_flags; Elf32_Half e_ehsize;
    Elf32_Half e_phentsize; Elf32_Half e_phnum; Elf32_Half e_shentsize;
    Elf32_Half e_shnum; Elf32_Half e_shstrndx;
};
struct Elf32_Phdr {
    Elf32_Word p_type; Elf32_Off p_offset; Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr; Elf32_Word p_filesz; Elf32_Word p_memsz;
    Elf32_Word p_flags; Elf32_Word p_align;
};
#define PT_LOAD 1
#define PF_W 2

/* 로드 함수 */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;                     //ELF 실행 파일의 헤더 정보를 담을 구조체
  struct file *file = NULL;                   //포인터 초기화
  off_t file_ofs;                             //파일 내에서 읽을 위치를 저장하는 변수
  bool success = false; 
  int i;

  t->pagedir = pagedir_create ();             //페이지 디렉토리 생성
  if (t->pagedir == NULL) goto done; 
  process_activate ();                         //cpu에 활성화 적용

  /* 파일명만 따로 분리하여 열기 */
  char fn_copy[128]; 
  strlcpy (fn_copy, file_name, sizeof fn_copy);         //명령행 문자열 전체 복사
  char *save_ptr; //분리상태 유지 
  char *prog_name = strtok_r (fn_copy, " ", &save_ptr);     //실제 파일이름만 추출

  file = filesys_open (prog_name);                         //실행 파일 찾아서 열기
  if (file == NULL) goto done;                           

  /* 실행 파일 쓰기 금지 설정 [cite: 81, 84] */
  file_deny_write (file);

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr     //정상적인 ELF 실행 파일인지 형식 검사
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2)
    goto done;

  file_ofs = ehdr.e_phoff;   //실제 프로그램 내용이 시작되는 위치 정보
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;
      file_seek (file, file_ofs);    //현재 읽을 세그먼트 정보의 위치로 이동
      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) goto done;    //세그먼트 상세정보를 읽음
      file_ofs += sizeof phdr;    

      if (phdr.p_type == PT_LOAD)     
        {
          if (validate_segment (&phdr, file))     //세그먼트의 주소나 크기가 유효한지 검증
            {
              bool writable = (phdr.p_flags & PF_W) != 0;     //메모리에 쓰기 가능한지
              uint32_t file_page = phdr.p_offset & ~PGMASK;    //페이지 시작위치 계산
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;       //가상 메모리 페이지 주소 계산
              uint32_t page_offset = phdr.p_vaddr & PGMASK;    //오프셋 계산
              uint32_t read_bytes, zero_bytes;                 //파일을 실제 읽을 크기, 0으로 채울 크기 계산
              if (phdr.p_filesz > 0) {                         //파일에서 읽어올 내용이 있는지      
                  read_bytes = page_offset + phdr.p_filesz;    //페이지 시작+실제 파일데이터 크기
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) - read_bytes);  //0으로 채울 공간의 크기 계산
              } else {
                  read_bytes = 0;          
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);    //해당페이지 전체 0으로 채움
              }
              if (!load_segment (file, file_page, (void *) mem_page, read_bytes, zero_bytes, writable))    //실제 메모리 할당 및 적재
                goto done;
            } else goto done;
        }
    }

  if (!setup_stack (esp)) goto done;     //사용자 프로그램이 사용할 스택 영역 할당
  *eip = (void (*) (void)) ehdr.e_entry; //프로그램의 진입점 저장
  success = true;

done:
  if (success) t->bin_file = file; //파일 객체 주소 보관관
  else file_close (file);
  return success;
}

/* 스택에 인자 패싱 [cite: 179, 180, 187] */
static bool
push_arguments (void **esp, const char *cmdline)
{
  char *copy = palloc_get_page (0);
  if (copy == NULL) return false;
  strlcpy (copy, cmdline, PGSIZE);

  char *argv[64];
  int argc = 0;
  char *token, *save_ptr;

  /* 토큰 분리 */
  for (token = strtok_r (copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
       if (argc >= 64) break;
       argv[argc++] = token;
    }
  if (argc == 0) { palloc_free_page (copy); return false; }

  /* 1. 인자 문자열 push (오른쪽에서 왼쪽) [cite: 179] */
  char *arg_addrs[64];
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp -= len;
      memcpy (*esp, argv[i], len);
      arg_addrs[i] = *esp;
    }

  /* 2. 4바이트 단위 정렬 [cite: 180, 187] */
  uintptr_t sp_val = (uintptr_t)*esp;
  if (sp_val % 4 != 0)
    {
      size_t pad = sp_val % 4;
      *esp -= pad;
      memset (*esp, 0, pad);
    }

  /* 3. argv[argc] (NULL) push */
  *esp -= sizeof (char *);
  *(char **)*esp = NULL;

  /* 4. argv[i]들의 실제 주소 push [cite: 182, 183] */
  for (int i = argc - 1; i >= 0; i--)
    {
      *esp -= sizeof (char *);
      *(char **)*esp = arg_addrs[i];
    }

  /* 5. argv(첫 번째 인자 주소), argc, return address push [cite: 181, 182] */
  char **argv_start = (char **)*esp;
  *esp -= sizeof (char **);
  *(char ***)*esp = argv_start;

  *esp -= sizeof (int);
  *(int *)*esp = argc;

  *esp -= sizeof (void *);
  *(void **)*esp = NULL;

  palloc_free_page (copy);
  return true;
}


//세그먼트 유효성 검사
static bool validate_segment (const struct Elf32_Phdr *phdr, struct file *file) {
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) return false; //파일의 오프셋과 가상주소의 오프셋 정렬확인
  if (phdr->p_offset > (Elf32_Off) file_length (file)) return false;    //시작 위치 확인
  if (phdr->p_memsz < phdr->p_filesz) return false;                     //메모리 할당량은 파일 데이터 크기보다 크거나 같아야함
  if (!is_user_vaddr ((void *) phdr->p_vaddr)) return false;            //사용자 영역에 해당하는 지
  return true;
}

static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,          
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
  file_seek (file, ofs);   //세그먼트 시작점으로 이동
  //읽을 데이터나 0으로 채울 공간이 남아있는 동안 반복
  while (read_bytes > 0 || zero_bytes > 0) {                               
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;  //읽을 바이트 수 결정
      size_t page_zero_bytes = PGSIZE - page_read_bytes;   //남은 바이트 0으로 채움
      uint8_t *kpage = palloc_get_page (PAL_USER);    //물리 메모리페이지 할당
      if (kpage == NULL) return false;              //메모리 할당 실패
    //데이터를 읽고 할당된 메모리에 씀
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
          palloc_free_page (kpage); return false;
      }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);    //남은공간 0으로 채움
      if (!install_page (upage, kpage, writable)) {
          palloc_free_page (kpage); return false;
      }   //물리 페이지를 가상주소에 연결
      read_bytes -= page_read_bytes; zero_bytes -= page_zero_bytes; upage += PGSIZE; //남은 양 계산하고 다음 가상페이지 이동 
  }
  return true;
}

static bool setup_stack (void **esp) {
  uint8_t *kpage = palloc_get_page (PAL_USER | PAL_ZERO);    //물리페이지를 할당받고 초기화
  
  //할당받은 페이지와 가상메모리 연결
  if (kpage != NULL) {
      if (install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true)) {
          *esp = PHYS_BASE; return true;  //esp초기화
      }
      palloc_free_page (kpage);  //페이지 반환
  }
  return false;
}

static bool install_page (void *upage, void *kpage, bool writable) {
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL   //할당받은 페이지가 있는지 확인
          && pagedir_set_page (t->pagedir, upage, kpage, writable));  //가상주소와 물리주소를 매핑
}
