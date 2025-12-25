#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "threads/synch.h"        /* [P2-1 FIX] */
#include "threads/malloc.h"       /* [P2-1 FIX] malloc/free */

/* [P2-1 FIX] syscall.c의 전역 락 공유 */
extern struct lock filesys_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *file_name, void (**eip) (void), void **esp);
static bool push_arguments (void **esp, const char *cmdline);

/* [P2-1 FIX] exec 동기화용 */
struct exec_info {
  char *cmdline;                 /* palloc page */
  struct semaphore done;         /* 부모가 기다림 */
  bool success;                  /* load+argpassing 성공 */
};

tid_t
process_execute (const char *file_name)
{
  /* thread name은 프로그램명(첫 토큰)만 */
  char name_copy[16];
  strlcpy (name_copy, file_name, sizeof name_copy);
  char *save_ptr;
  char *prog = strtok_r (name_copy, " ", &save_ptr);
  if (prog == NULL)
    return TID_ERROR;

  /* cmdline copy */
  char *cmdline = palloc_get_page (0);
  if (cmdline == NULL)
    return TID_ERROR;
  strlcpy (cmdline, file_name, PGSIZE);

  struct exec_info *info = malloc (sizeof *info);
  if (info == NULL)
    {
      palloc_free_page (cmdline);
      return TID_ERROR;
    }
  info->cmdline = cmdline;
  info->success = false;
  sema_init (&info->done, 0);

  /* thread 생성 */
  tid_t tid = thread_create (prog, PRI_DEFAULT, start_process, info);
  if (tid == TID_ERROR)
    {
      palloc_free_page (cmdline);
      free (info);
      return TID_ERROR;
    }

  /* [P2-1 FIX] 부모는 자식 load 성공/실패 확정까지 기다림 */
  sema_down (&info->done);

  /* 명세: 로드 실패면 exec는 -1 반환해야 하므로 TID_ERROR로 변환 */
  if (!info->success)
    tid = TID_ERROR;

  free (info);
  return tid;
}

static void
start_process (void *aux_)
{
  struct exec_info *info = aux_;
  char *cmdline = info->cmdline;

  struct intr_frame if_;
  bool success = false;

  /* thread 필드 초기화(출력 통제용) */
#ifdef USERPROG
  struct thread *cur = thread_current ();
  cur->exited = false;
  cur->exit_status = -1;
  cur->load_completed = false;
  cur->load_success = false;
#endif

  /* cmdline parse용 복사 */
  char *copy = palloc_get_page (0);
  if (copy == NULL)
    goto done;
  strlcpy (copy, cmdline, PGSIZE);

  char *save_ptr;
  char *prog = strtok_r (copy, " ", &save_ptr);
  if (prog == NULL)
    goto done;

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* [P2-1 FIX] 파일 시스템 접근 동기화 */
  lock_acquire (&filesys_lock);
  success = load (prog, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);

  if (success)
    {
      void *new_esp = if_.esp;
      if (!push_arguments (&new_esp, cmdline))
        success = false;
      else
        if_.esp = new_esp;
    }

done:
#ifdef USERPROG
  cur->load_completed = true;
  cur->load_success = success;
#endif

  info->success = success;
  sema_up (&info->done);

  if (copy != NULL) palloc_free_page (copy);
  palloc_free_page (cmdline);

  if (!success)
    thread_exit ();

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

int
process_wait (tid_t child_tid UNUSED)
{
  /* 2-1 요구사항: 임시 무한 대기 */
  for (;;)
    thread_yield ();
}

void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd = cur->pagedir;

#ifdef USERPROG
  /* [P2-1 FIX] 종료 메시지는 여기서 “한 번만” 출력한다.
     - 유저 프로세스만(pagedir != NULL)
     - load 성공한 프로세스만(load_success == true)
     - exit_status: syscall exit이면 그 값, 예외면 -1 */
  if (cur->pagedir != NULL && cur->load_success)
    {
      int status = cur->exited ? cur->exit_status : -1;
      printf ("%s: exit(%d)\n", cur->name, status);
    }
#endif

  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* context switch마다 호출 */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ====== load() 이하: 네 코드 기반, 단 “printf 제거”만 반영 ====== */

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_W 2

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool install_page (void *upage, void *kpage, bool writable);

bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  file = filesys_open (file_name);
  if (file == NULL)
    goto done;   /* [P2-1 FIX] 출력 금지 */

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    goto done;   /* [P2-1 FIX] 출력 금지 */

  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;

      switch (phdr.p_type)
        {
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;

              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }

              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;

        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;

        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

done:
  file_close (file);
  return success;
}

/* validate_segment/load_segment/setup_stack/install_page는 네 코드 그대로 두면 됨 */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;
  if (phdr->p_memsz < phdr->p_filesz)
    return false;
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;
  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool
setup_stack (void **esp)
{
  uint8_t *kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  bool success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
  if (success)
    *esp = PHYS_BASE;
  else
    palloc_free_page (kpage);

  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* push_arguments는 네 코드 그대로 사용 */
static bool
push_arguments (void **esp, const char *cmdline)
{
  char *copy = palloc_get_page (0);
  if (copy == NULL)
    return false;
  strlcpy (copy, cmdline, PGSIZE);

  char *argv[64];
  int argc = 0;

  char *token, *save_ptr;
  for (token = strtok_r (copy, " ", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc >= 64)
        {
          palloc_free_page (copy);
          return false;
        }
      argv[argc++] = token;
    }

  if (argc == 0)
    {
      palloc_free_page (copy);
      return false;
    }

  void *sp = *esp;
  char *arg_addrs[64];

  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      sp = (uint8_t *) sp - len;
      memcpy (sp, argv[i], len);
      arg_addrs[i] = sp;
    }

  uintptr_t sp_val = (uintptr_t) sp;
  size_t pad = sp_val % 4;
  if (pad)
    {
      sp = (uint8_t *) sp - pad;
      memset (sp, 0, pad);
    }

  sp = (uint8_t *) sp - sizeof (char *);
  *(char **) sp = NULL;

  for (int i = argc - 1; i >= 0; i--)
    {
      sp = (uint8_t *) sp - sizeof (char *);
      *(char **) sp = arg_addrs[i];
    }

  char **argv0 = (char **) sp;
  sp = (uint8_t *) sp - sizeof (char **);
  *(char ***) sp = argv0;

  sp = (uint8_t *) sp - sizeof (int);
  *(int *) sp = argc;

  sp = (uint8_t *) sp - sizeof (void *);
  *(void **) sp = NULL;

  *esp = sp;
  palloc_free_page (copy);
  return true;
}
