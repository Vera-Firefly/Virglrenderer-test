/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_worker.h"

/* One and only one of ENABLE_RENDER_SERVER_WORKER_* must be set.
 *
 * With ENABLE_RENDER_SERVER_WORKER_PROCESS, each worker is a subprocess
 * forked from the server process.
 *
 * With ENABLE_RENDER_SERVER_WORKER_THREAD, each worker is a thread of the
 * server process.
 *
 * With ENABLE_RENDER_SERVER_WORKER_MINIJAIL, each worker is a subprocess
 * forked from the server process, jailed with minijail.
 */
#if (ENABLE_RENDER_SERVER_WORKER_PROCESS + ENABLE_RENDER_SERVER_WORKER_THREAD +          \
     ENABLE_RENDER_SERVER_WORKER_MINIJAIL) != 1
#error "no worker defined"
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <threads.h>
#include <unistd.h>

struct render_worker {
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   thrd_t thread;
#else
   pid_t pid;
#endif
   bool reaped;

   char thread_data[];
};

#ifdef ENABLE_RENDER_SERVER_WORKER_MINIJAIL

#include <fcntl.h>
#include <libminijail.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdio.h>
#include <sys/stat.h>

static bool
load_bpf_program(struct sock_fprog *prog, const char *path)
{
   int fd = -1;
   void *data = NULL;

   fd = open(path, O_RDONLY);
   if (fd < 0)
      goto fail;

   const off_t size = lseek(fd, 0, SEEK_END);
   if (size <= 0 || size % sizeof(struct sock_filter))
      goto fail;
   lseek(fd, 0, SEEK_SET);

   data = malloc(size);
   if (!data)
      goto fail;

   off_t cur = 0;
   while (cur < size) {
      const ssize_t r = read(fd, (char *)data + cur, size - cur);
      if (r <= 0)
         goto fail;
      cur += r;
   }

   close(fd);

   prog->len = size / sizeof(struct sock_filter);
   prog->filter = data;

   return true;

fail:
   if (data)
      free(data);
   if (fd >= 0)
      close(fd);
   return false;
}

static struct render_worker_jail *
create_minijail(enum render_worker_jail_seccomp_filter seccomp_filter,
                const char *seccomp_path)
{
   struct minijail *j = minijail_new();

   /* TODO namespaces and many more */
   minijail_no_new_privs(j);

   if (seccomp_filter != RENDER_WORKER_JAIL_SECCOMP_NONE) {
      if (seccomp_filter == RENDER_WORKER_JAIL_SECCOMP_BPF) {
         struct sock_fprog prog;
         if (!load_bpf_program(&prog, seccomp_path)) {
            minijail_destroy(j);
            return NULL;
         }

         minijail_set_seccomp_filters(j, &prog);
         free(prog.filter);
      } else {
         if (seccomp_filter == RENDER_WORKER_JAIL_SECCOMP_MINIJAIL_POLICY_LOG)
            minijail_log_seccomp_filter_failures(j);
         minijail_parse_seccomp_filters(j, seccomp_path);
      }

      minijail_use_seccomp_filter(j);
   }

   return (struct render_worker_jail *)j;
}

static pid_t
fork_minijail(const struct render_worker_jail *jail)
{
   struct minijail *j = minijail_new();
   if (!j)
      return -1;

   /* is this faster? */
   if (minijail_copy_jail((const struct minijail *)jail, j)) {
      minijail_destroy(j);
      return -1;
   }

   pid_t pid = minijail_fork(j);
   minijail_destroy(j);

   return pid;
}

#endif /* ENABLE_RENDER_SERVER_WORKER_MINIJAIL */

struct render_worker_jail *
render_worker_jail_create(enum render_worker_jail_seccomp_filter seccomp_filter,
                          const char *seccomp_path)
{
#if defined(ENABLE_RENDER_SERVER_WORKER_MINIJAIL)
   return create_minijail(seccomp_filter, seccomp_path);
#else
   /* TODO RENDER_WORKER_JAIL_SECCOMP_BPF */
   if (seccomp_filter != RENDER_WORKER_JAIL_SECCOMP_NONE)
      return NULL;
   (void)seccomp_path;
   return (void *)1;
#endif
}

void
render_worker_jail_destroy(struct render_worker_jail *jail)
{
#if defined(ENABLE_RENDER_SERVER_WORKER_MINIJAIL)
   minijail_destroy((struct minijail *)jail);
#else
   (void)jail;
#endif
}

struct render_worker *
render_worker_create(struct render_worker_jail *jail,
                     int (*thread_func)(void *thread_data),
                     void *thread_data,
                     size_t thread_data_size)
{
   struct render_worker *worker = calloc(1, sizeof(*worker) + thread_data_size);
   if (!worker)
      return NULL;

   memcpy(worker->thread_data, thread_data, thread_data_size);

   bool ok;
#if defined(ENABLE_RENDER_SERVER_WORKER_PROCESS)
   worker->pid = fork();
   ok = worker->pid >= 0;
   (void)jail;
   (void)thread_func;
#elif defined(ENABLE_RENDER_SERVER_WORKER_THREAD)
   ok = thrd_create(&worker->thread, thread_func, worker->thread_data) == thrd_success;
   (void)jail;
#elif defined(ENABLE_RENDER_SERVER_WORKER_MINIJAIL)
   worker->pid = fork_minijail(jail);
   ok = worker->pid >= 0;
   (void)thread_func;
#endif
   if (!ok) {
      free(worker);
      return NULL;
   }

   return worker;
}

bool
render_worker_is_record(const struct render_worker *worker)
{
   /* return false if called from the worker itself */
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   return !thrd_equal(worker->thread, thrd_current());
#else
   return worker->pid > 0;
#endif
}

void
render_worker_kill(struct render_worker *worker)
{
   assert(render_worker_is_record(worker));

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   /* we trust the thread to clean up and exit in finite time */
#else
   kill(worker->pid, SIGKILL);
#endif
}

bool
render_worker_reap(struct render_worker *worker, bool wait)
{
   assert(render_worker_is_record(worker));

   if (worker->reaped)
      return true;

   bool ok;
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   (void)wait;
   ok = thrd_join(worker->thread, NULL) == thrd_success;
#else
   const int options = WEXITED | (wait ? 0 : WNOHANG);
   siginfo_t siginfo = { 0 };
   const int ret = waitid(P_PID, worker->pid, &siginfo, options);
   ok = !ret && siginfo.si_pid == worker->pid;
#endif

   worker->reaped = ok;
   return ok;
}

void
render_worker_destroy(struct render_worker *worker)
{
   free(worker);
}
