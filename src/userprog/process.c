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

/* 프로세스 실행 간 동기화를 위한 구조체 */
struct exec_info {
  char *cmdline;
  struct semaphore done;
  bool success;
  /* [P2-1 FIX] 자식 프로세스 메타데이터 전달용 */
  struct child_process *cp; 
};

/* Starts a new thread running a user program loaded from FILENAME. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  char name_copy[16];
  strlcpy (name_copy, file_name, sizeof name_copy);
  char *save_ptr;
  char *prog_name = strtok_r (name_copy, " ", &save_ptr);

  struct exec_info *info = malloc (sizeof (struct exec_info));
  if (info == NULL) 
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  
  info->cmdline = fn_copy;
  info->success = false;
  sema_init (&info->done, 0);

  /* [P2-1 FIX] 부모가 관리할 자식 메타데이터 생성 */
  struct child_process *cp = malloc (sizeof (struct child_process));
  if (cp == NULL)
    {
      palloc_free_page (fn_copy);
      free (info);
      return TID_ERROR;
    }
  
  cp->tid = TID_ERROR;
  cp->exit_status = -1;
  sema_init (&cp->wait_sema, 0);
  list_push_back (&thread_current ()->children, &cp->elem);
  
  info->cp = cp; // 자식에게 포인터 전달

  tid = thread_create (prog_name, PRI_DEFAULT, start_process, info);
  
  if (tid == TID_ERROR)
    {
      // 실패 시 리스트에서 제거 및 해제
      list_remove (&cp->elem);
      free (cp);
      palloc_free_page (fn_copy);
      free (info);
      return TID_ERROR;
    }
  
  /* 스레드 생성 성공 시 TID 기록 */
  cp->tid = tid;

  /* Wait for load */
  sema_down (&info->done);

  if (!info->success)
    tid = TID_ERROR;

  free (info);
  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *aux_)
{
  struct exec_info *info = aux_;
  char *cmdline = info->cmdline; 
  struct intr_frame if_;
  bool success;

#ifdef USERPROG
  struct thread *cur = thread_current ();
  cur->exit_status = -1;
  cur->load_success = false;
  
  /* [P2-1 FIX] 부모가 만들어준 내 메타데이터 포인터 저장 */
  cur->cp = info->cp;
#endif

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  lock_acquire (&filesys_lock);
  success = load (cmdline, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);

  if (success) 
    {
      if (!push_arguments (&if_.esp, cmdline))
        success = false;
    }

  palloc_free_page (cmdline);

  info->success = success;
#ifdef USERPROG
  cur->load_success = success;
#endif
  sema_up (&info->done);

  if (!success) 
    thread_exit ();

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status. */
int
process_wait (tid_t child_tid UNUSED)
{
  /* [P2-1 FIX] 자식 리스트 검색 */
  struct thread *cur = thread_current ();
  struct list_elem *e;

  for (e = list_begin (&cur->children); e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_process *cp = list_entry (e, struct child_process, elem);
      if (cp->tid == child_tid)
        {
          /* 자식이 종료될 때까지 대기 */
          sema_down (&cp->wait_sema);
          
          /* 종료 코드 획득 */
          int status = cp->exit_status;
          
          /* 정보 사용 후 리스트에서 제거 및 해제 */
          list_remove (&cp->elem);
          free (cp);
          
          return status;
        }
    }
    
  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

#ifdef USERPROG
  if (cur->pagedir != NULL && cur->load_success) 
    {
      printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
    }
    
  if (cur->bin_file != NULL)
    {
      file_close (cur->bin_file);
      cur->bin_file = NULL;
    }

  /* [P2-1 FIX] 부모가 볼 수 있는 메타데이터 업데이트 */
  if (cur->cp != NULL)
    {
      cur->cp->exit_status = cur->exit_status;
      sema_up (&cur->cp->wait_sema);
    }
#endif

  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* process_activate 및 load, load_segment 등 나머지 코드는 동일 ... */
/* 파일 길이가 길어 나머지 부분은 기존 코드와 동일합니다 (load 함수 포함) */
/* 만약 전체 코드가 필요하시면 말씀해 주세요. */

void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

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

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

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

  char *fn_copy = palloc_get_page (0);
  if (fn_copy == NULL) goto done;
  strlcpy (fn_copy, file_name, PGSIZE);
  char *save_ptr;
  char *prog_name = strtok_r (fn_copy, " ", &save_ptr);

  file = filesys_open (prog_name);
  palloc_free_page (fn_copy);

  if (file == NULL)
    {
      printf ("load: %s: open failed\n", prog_name);
      goto done;
    }

  file_deny_write (file);

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", prog_name);
      goto done;
    }

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
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  if (success) 
    {
      t->bin_file = file; 
    }
  else 
    {
      file_close (file);
    }
  return success;
}

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
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

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

  for (token = strtok_r (copy, " ", &save_ptr); token != NULL;
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