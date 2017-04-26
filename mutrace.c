/*-*- Mode: C; c-basic-offset: 8 -*-*/

/***
  This file is part of mutrace.

  Copyright 2009-2017 Lennart Poettering and others

  mutrace is free software: you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  mutrace is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with mutrace. If not, see <http://www.gnu.org/licenses/>.
***/

#include "config.h"

#include <pthread.h>
#include <execinfo.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sched.h>
#include <malloc.h>
#include <signal.h>
#include <math.h>

#include <isc/types.h>
#include <isc/result.h>
#include <isc/rwlock.h>

#if !defined (__linux__) || !defined(__GLIBC__)
#error "This stuff only works on Linux!"
#endif

#ifndef SCHED_RESET_ON_FORK
/* "Your libc lacks the definition of SCHED_RESET_ON_FORK. We'll now
 * define it ourselves, however make sure your kernel is new
 * enough! */
#define SCHED_RESET_ON_FORK 0x40000000
#endif

#if defined(__i386__) || defined(__x86_64__)
#define DEBUG_TRAP __asm__("int $3")
#else
#define DEBUG_TRAP raise(SIGTRAP)
#endif

#define LIKELY(x) (__builtin_expect(!!(x),1))
#define UNLIKELY(x) (__builtin_expect(!!(x),0))

#define FP_NS_PER_MS 1000000.0

struct stacktrace_info {
        void **frames;
        int nb_frame;
        char *as_string;
};

struct location_stats {
        struct stacktrace_info stacktrace;

        unsigned n_blocker;
        unsigned n_blockee;

        struct location_stats *next;
};

struct thread_info {
        pid_t owner;
        struct stacktrace_info stacktrace;
        struct location_stats *locked_from;
        struct thread_info *next;
};

struct mutex_info {
        pthread_mutex_t *mutex;
        pthread_rwlock_t *rwlock;
        isc_rwlock_t *rwl;

        int type, protocol, kind;
        bool broken:1;
        bool realtime:1;
        bool dead:1;
        bool detailed_tracking:1;
        bool is_rw:1;

        unsigned n_lock_level;

        pid_t last_owner;

        unsigned n_locked;
        unsigned n_owner_changed;
        unsigned n_contended;

        uint64_t nsec_locked_total;
        uint64_t nsec_locked_max;

        uint64_t nsec_contended_total; /* mutexes only */
        uint64_t nsec_contended_max; /* mutexes only */

        uint64_t nsec_read_contended_total; /* rwlocks only */
        uint64_t nsec_read_contended_max; /* rwlocks only */
        uint64_t nsec_write_contended_total; /* rwlocks only */
        uint64_t nsec_write_contended_max; /* rwlocks only */

        uint64_t nsec_timestamp;
        struct stacktrace_info origin_stacktrace;

        unsigned id;

        struct mutex_info *next;

        struct location_stats *locked_from;

        struct thread_info *holder_list;
};

static unsigned hash_size = 3371; /* probably a good idea to pick a prime here */
#ifndef MUTRACE_DEFAULT_FRAMES_MAX
#   define MUTRACE_DEFAULT_FRAMES_MAX 16
#endif
static unsigned frames_max = MUTRACE_DEFAULT_FRAMES_MAX;
static unsigned frames_memsize = MUTRACE_DEFAULT_FRAMES_MAX * sizeof(void*);

static volatile unsigned n_broken = 0;
static volatile unsigned n_collisions = 0;
static volatile unsigned n_self_contended = 0;

static unsigned show_n_locked_min = 1;
static unsigned show_n_owner_changed_min = 2;
static unsigned show_n_contended_min = 0;
static unsigned show_n_max = 10;

static bool raise_trap = false;
static bool track_rt = false;
/* TODO: detail_contentious_* need to be settable from environment variables */
static bool detail_contentious_mutexes = false;
static bool detail_contentious_rwlocks = false;
static bool detail_contentious_isc_rwlocks = false;

static int (*real_pthread_mutex_init)(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) = NULL;
static int (*real_pthread_mutex_destroy)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_lock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_timedlock)(pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_wait)(pthread_cond_t *cond, pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_timedwait)(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;

static int (*real_pthread_create)(pthread_t *newthread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg) = NULL;

static int (*real_pthread_rwlock_init)(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) = NULL;
static int (*real_pthread_rwlock_destroy)(pthread_rwlock_t *rwlock) = NULL;
static int (*real_pthread_rwlock_rdlock)(pthread_rwlock_t *rwlock) = NULL;
static int (*real_pthread_rwlock_tryrdlock)(pthread_rwlock_t *rwlock) = NULL;
static int (*real_pthread_rwlock_timedrdlock)(pthread_rwlock_t *rwlock, const struct timespec *abstime) = NULL;
static int (*real_pthread_rwlock_wrlock)(pthread_rwlock_t *rwlock) = NULL;
static int (*real_pthread_rwlock_trywrlock)(pthread_rwlock_t *rwlock) = NULL;
static int (*real_pthread_rwlock_timedwrlock)(pthread_rwlock_t *rwlock, const struct timespec *abstime) = NULL;
static int (*real_pthread_rwlock_unlock)(pthread_rwlock_t *rwlock);

static isc_result_t (*real_isc_rwlock_init)(isc_rwlock_t *rwl, unsigned int read_quota, unsigned int write_quota) = NULL;
static isc_result_t (*real_isc_rwlock_lock)(isc_rwlock_t *rwl, isc_rwlocktype_t type) = NULL;
static isc_result_t (*real_isc_rwlock_trylock)(isc_rwlock_t *rwl, isc_rwlocktype_t type) = NULL;
static isc_result_t (*real_isc_rwlock_unlock)(isc_rwlock_t *rwl, isc_rwlocktype_t type) = NULL;
static isc_result_t (*real_isc_rwlock_tryupgrade)(isc_rwlock_t *rwl) = NULL;
static void (*real_isc_rwlock_destroy)(isc_rwlock_t *rwl) = NULL;

static void (*real_exit)(int status) __attribute__((noreturn)) = NULL;
static void (*real__exit)(int status) __attribute__((noreturn)) = NULL;
static void (*real__Exit)(int status) __attribute__((noreturn)) = NULL;
static int (*real_backtrace)(void **array, int size) = NULL;
static char **(*real_backtrace_symbols)(void *const *array, int size) = NULL;
static void (*real_backtrace_symbols_fd)(void *const *array, int size, int fd) = NULL;

static struct mutex_info **alive_mutexes = NULL, **dead_mutexes = NULL;
static pthread_mutex_t *mutexes_lock = NULL;

static __thread bool recursive = false;

static volatile bool initialized = false;
static volatile bool threads_existing = false;

static uint64_t nsec_timestamp_setup;

/* Must be kept in sync with summary_order_details. */
typedef enum {
        ORDER_ID = 0,
        ORDER_N_LOCKED,
        ORDER_N_OWNER_CHANGED,
        ORDER_N_CONTENDED,
        ORDER_NSEC_LOCKED_TOTAL,
        ORDER_NSEC_LOCKED_MAX,
        ORDER_NSEC_LOCKED_AVG,
        ORDER_NSEC_CONTENDED_TOTAL,
        ORDER_NSEC_CONTENDED_AVG,
        ORDER_NSEC_READ_CONTENDED_TOTAL,
        ORDER_NSEC_READ_CONTENDED_MAX,
        ORDER_NSEC_READ_CONTENDED_AVG,
        ORDER_NSEC_WRITE_CONTENDED_TOTAL,
        ORDER_NSEC_WRITE_CONTENDED_MAX,
        ORDER_NSEC_WRITE_CONTENDED_AVG,

        ORDER_INVALID, /* MUST STAY LAST */
} SummaryOrder;

typedef struct {
        const char *command; /* as passed to --order command line argument */
        const char *ui_string; /* as displayed by show_summary() */
} SummaryOrderDetails;

/* Must be kept in sync with SummaryOrder. */
static const SummaryOrderDetails summary_order_details[] = {
        { "id", "mutex number" },
        { "n-locked", "lock count" },
        { "n-owner-changed", "owner change count" },
        { "n-contended", "contention count" },
        { "nsec-locked-total", "total time locked" },
        { "nsec-locked-max", "maximum time locked" },
        { "nsec-locked-avg", "average time locked" },
        { "nsec-contended-total", "total time contended" },
        { "nsec-contended-avg", "average time contended" },
        { "nsec-read-contended-total", "total time (read) contended" },
        { "nsec-read-contended-max", "maximum time (read) contended" },
        { "nsec-read-contended-avg", "average time (read) contended" },
        { "nsec-write-contended-total", "total time (write) contended" },
        { "nsec-write-contended-max", "maximum time (write) contended" },
        { "nsec-write-contended-avg", "average time (write) contended" },
};

static SummaryOrder summary_order = ORDER_N_CONTENDED;

static int parse_env_summary_order(const char *n, unsigned *t);

static void setup(void) __attribute ((constructor));
static void shutdown(void) __attribute ((destructor));

/* the struct stactrace_info owns and keeps a pointer to the returned string */
static const char *stacktrace_to_string(struct stacktrace_info stacktrace);

static void sigusr1_cb(int sig);

static pid_t _gettid(void) {
        return (pid_t) syscall(SYS_gettid);
}

static uint64_t nsec_now(void) {
        struct timespec ts;
        int r;

        r = clock_gettime(CLOCK_MONOTONIC, &ts);
        assert(r == 0);

        return
                (uint64_t) ts.tv_sec * 1000000000ULL +
                (uint64_t) ts.tv_nsec;
}

static const char *get_prname(void) {
        static char prname[17];
        int r;

        r = prctl(PR_GET_NAME, prname);
        assert(r == 0);

        prname[16] = 0;

        return prname;
}

static int parse_env_unsigned(const char *n, unsigned *t) {
        const char *e;
        char *x = NULL;
        unsigned long ul;

        if (!(e = getenv(n)))
                return 0;

        errno = 0;
        ul = strtoul(e, &x, 0);
        if (!x || *x || errno != 0)
                return -1;

        *t = (unsigned) ul;

        if ((unsigned long) *t != ul)
                return -1;

        return 0;
}

static int parse_env_summary_order(const char *n, unsigned *t) {
        const char *e;
        unsigned int i;

        if (!(e = getenv(n)))
                return 0;

        for (i = ORDER_ID; i < ORDER_INVALID; i++) {
                if (strcmp(e, summary_order_details[i].command) == 0) {
                        *t = i;
                        return 0;
                }
        }

        return -1;
}

/* Maximum tolerated relative error when comparing doubles */
/* TODO: consider making this settable via an environment variable */
#define MAX_RELATIVE_ERROR 0.001

static bool doubles_equal(double a, double b) {
        /* Make sure we don't divide by zero. */
        if (fpclassify(b) == FP_ZERO)
                return (fpclassify(a) == FP_ZERO);

        return ((a - b) / b <= MAX_RELATIVE_ERROR);
}

#define LOAD_FUNC(name)                                                 \
        do {                                                            \
                *(void**) (&real_##name) = dlsym(RTLD_NEXT, #name);     \
                assert(real_##name);                                    \
        } while (false)

#define LOAD_FUNC_VERSIONED(name, version)                              \
        do {                                                            \
                *(void**) (&real_##name) = dlvsym(RTLD_NEXT, #name, version); \
                assert(real_##name);                                    \
        } while (false)

static void load_functions(void) {
        static volatile bool loaded = false;

        if (LIKELY(loaded))
                return;

        recursive = true;

        /* If someone uses a shared library constructor that is called
         * before ours we might not be initialized yet when the first
         * lock related operation is executed. To deal with this we'll
         * simply call the original implementation and do nothing
         * else, but for that we do need the original function
         * pointers. */

        LOAD_FUNC(pthread_create);

        LOAD_FUNC(pthread_mutex_init);
        LOAD_FUNC(pthread_mutex_destroy);
        LOAD_FUNC(pthread_mutex_lock);
        LOAD_FUNC(pthread_mutex_trylock);
        LOAD_FUNC(pthread_mutex_timedlock);
        LOAD_FUNC(pthread_mutex_unlock);

        /* There's some kind of weird incompatibility problem causing
         * pthread_cond_timedwait() to freeze if we don't ask for this
         * explicit version of these functions */
        LOAD_FUNC_VERSIONED(pthread_cond_wait, "GLIBC_2.3.2");
        LOAD_FUNC_VERSIONED(pthread_cond_timedwait, "GLIBC_2.3.2");

        LOAD_FUNC(pthread_rwlock_init);
        LOAD_FUNC(pthread_rwlock_destroy);
        LOAD_FUNC(pthread_rwlock_rdlock);
        LOAD_FUNC(pthread_rwlock_tryrdlock);
        LOAD_FUNC(pthread_rwlock_timedrdlock);
        LOAD_FUNC(pthread_rwlock_wrlock);
        LOAD_FUNC(pthread_rwlock_trywrlock);
        LOAD_FUNC(pthread_rwlock_timedwrlock);
        LOAD_FUNC(pthread_rwlock_unlock);

        LOAD_FUNC(isc_rwlock_init);
        LOAD_FUNC(isc_rwlock_lock);
        LOAD_FUNC(isc_rwlock_trylock);
        LOAD_FUNC(isc_rwlock_unlock);
        LOAD_FUNC(isc_rwlock_tryupgrade);
        LOAD_FUNC(isc_rwlock_destroy);

        LOAD_FUNC(exit);
        LOAD_FUNC(_exit);
        LOAD_FUNC(_Exit);

        LOAD_FUNC(backtrace);
        LOAD_FUNC(backtrace_symbols);
        LOAD_FUNC(backtrace_symbols_fd);

        loaded = true;
        recursive = false;
}

static void setup(void) {
        struct sigaction sigusr_action;
        pthread_mutex_t *m, *last;
        int r;
        unsigned t;

        load_functions();

        if (LIKELY(initialized))
                return;

        if (!dlsym(NULL, "main"))
                fprintf(stderr,
                        "mutrace: Application appears to be compiled without -rdynamic. It might be a\n"
                        "mutrace: good idea to recompile with -rdynamic enabled since this produces more\n"
                        "mutrace: useful stack traces.\n\n");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        if (__malloc_hook) {
#pragma GCC diagnostic pop
                fprintf(stderr,
                        "mutrace: Detected non-glibc memory allocator. Your program uses some\n"
                        "mutrace: alternative memory allocator (jemalloc?) which is not compatible with\n"
                        "mutrace: mutrace. Please rebuild your program with the standard memory\n"
                        "mutrace: allocator or fix mutrace to handle yours correctly.\n");

                /* The reason for this is that jemalloc and other
                 * allocators tend to call pthread_mutex_xxx() from
                 * the allocator. However, we need to call malloc()
                 * ourselves from some mutex operations so this might
                 * create an endless loop eventually overflowing the
                 * stack. glibc's malloc() does locking too but uses
                 * lock routines that do not end up calling
                 * pthread_mutex_xxx(). */

                real_exit(1);
        }

        t = hash_size;
        if (parse_env_unsigned("MUTRACE_HASH_SIZE", &t) < 0 || t == 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_HASH_SIZE.\n");
        else
                hash_size = t;

        t = frames_max;
        if (parse_env_unsigned("MUTRACE_FRAMES", &t) < 0 || t == 0) {
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_FRAMES.\n");
        }
        else {
                frames_max = t;
                frames_memsize = frames_max * sizeof(void*);
        }

        t = show_n_locked_min;
        if (parse_env_unsigned("MUTRACE_LOCKED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_LOCKED_MIN.\n");
        else
                show_n_locked_min = t;

        t = show_n_owner_changed_min;
        if (parse_env_unsigned("MUTRACE_OWNER_CHANGED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_OWNER_CHANGED_MIN.\n");
        else
                show_n_owner_changed_min = t;

        t = show_n_contended_min;
        if (parse_env_unsigned("MUTRACE_CONTENDED_MIN", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_CONTENDED_MIN.\n");
        else
                show_n_contended_min = t;

        t = show_n_max;
        if (parse_env_unsigned("MUTRACE_MAX", &t) < 0)
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_MAX.\n");
        else
                show_n_max = t;

        t = summary_order;
        if (parse_env_summary_order("MUTRACE_SUMMARY_ORDER", &t) < 0) {
                fprintf(stderr, "mutrace: WARNING: Failed to parse $MUTRACE_SUMMARY_ORDER.\n");
        }
        else {
                summary_order = t;
        }

        if (getenv("MUTRACE_TRAP"))
                raise_trap = true;

        if (getenv("MUTRACE_TRACK_RT"))
                track_rt = true;

        alive_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(alive_mutexes);

        dead_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(dead_mutexes);

        mutexes_lock = malloc(hash_size * sizeof(pthread_mutex_t));
        assert(mutexes_lock);

        for (m = mutexes_lock, last = mutexes_lock+hash_size; m < last; m++) {
                r = real_pthread_mutex_init(m, NULL);

                assert(r == 0);
        }

        /* Listen for SIGUSR1 and print out a summary of what's happened so far
         * when we receive it. */
        sigusr_action.sa_handler = sigusr1_cb;
        sigemptyset(&sigusr_action.sa_mask);
        sigusr_action.sa_flags = 0;
        sigaction(SIGUSR1, &sigusr_action, NULL);

        nsec_timestamp_setup = nsec_now();

        initialized = true;

        fprintf(stderr, "mutrace: "PACKAGE_VERSION" successfully initialized for process %s (PID: %lu).\n",
                get_prname(), (unsigned long) getpid());
}

static unsigned long mutex_hash(pthread_mutex_t *mutex) {
        unsigned long u;

        u = (unsigned long) mutex;
        u /= sizeof(void*);

        return u % hash_size;
}

static unsigned long rwlock_hash(pthread_rwlock_t *rwlock) {
        unsigned long u;

        u = (unsigned long) rwlock;
        u /= sizeof(void*);

        return u % hash_size;
}

static unsigned long isc_rwlock_hash(isc_rwlock_t *rwl) {
        unsigned long u;

        u = (unsigned long) rwl;
        u /= sizeof(void*);

        return u % hash_size;
}

static void lock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_trylock(mutexes_lock + u);

        if (UNLIKELY(r == EBUSY)) {
                __sync_fetch_and_add(&n_self_contended, 1);
                r = real_pthread_mutex_lock(mutexes_lock + u);
        }

        assert(r == 0);
}

static void unlock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_unlock(mutexes_lock + u);
        assert(r == 0);
}

/* the memory pointed to by stacktrace.frames is owned by some caller up the stack */
static struct location_stats *find_locking_location(struct mutex_info *mi, const struct stacktrace_info stacktrace) {
        struct location_stats *candidate, **pcandidate;

        pcandidate = &(mi->locked_from);
        candidate = mi->locked_from;

        while (candidate) {
                if (!memcmp(*(candidate->stacktrace.frames), *(stacktrace.frames), frames_memsize))
                        return candidate;

                pcandidate = &(candidate->next);
                candidate = candidate->next;
        }

        *pcandidate = calloc(1, sizeof(struct location_stats));

        assert(*pcandidate);

        (*pcandidate)->stacktrace.frames = calloc(frames_max, sizeof(void*));

        assert((*pcandidate)->stacktrace.frames);

        memcpy( *((*pcandidate)->stacktrace.frames), *(stacktrace.frames), frames_memsize );
        (*pcandidate)->stacktrace.nb_frame = stacktrace.nb_frame;

        return *pcandidate;
}

static struct thread_info *find_thread(struct mutex_info *mi, pid_t cur_thread) {
        struct thread_info *candidate;

        candidate = mi->holder_list;

        while (candidate) {
                if (candidate->owner == cur_thread)
                        break;

                candidate = candidate->next;
        }

        return candidate;
}

/*
 * Note that mutex_info_compare sorts the data in descending order
 * because that's how we're going to use it
 */
static int mutex_info_compare(const void *_a, const void *_b) {
        const struct mutex_info
                *a = *(const struct mutex_info**) _a,
                *b = *(const struct mutex_info**) _b;
        double a_avg, b_avg;

        #define ORDER_COUNT(metric) \
                if (a->metric > b->metric) \
                        return -1; \
                else if (a->metric < b->metric) \
                        return 1;

        #define ORDER_AVG(metric, metric_counter) \
                if (a->metric_counter > 0 && b->metric_counter > 0) { \
                        a_avg = a->metric / a->metric_counter; \
                        b_avg = b->metric / b->metric_counter; \
                        if (!doubles_equal(a_avg, b_avg)) \
                                return ((a_avg - b_avg) < 0.0) ? -1 : 1; \
                }

        /* Order by the user's chosen ordering first, then fall back to a static
         * ordering. */
        switch (summary_order) {
                case ORDER_ID:
                        ORDER_COUNT(id);
                        break;
                case ORDER_N_LOCKED:
                        ORDER_COUNT(n_locked);
                        break;
                case ORDER_N_OWNER_CHANGED:
                        ORDER_COUNT(n_owner_changed);
                        break;
                case ORDER_N_CONTENDED:
                        ORDER_COUNT(n_contended);
                        break;
                case ORDER_NSEC_LOCKED_TOTAL:
                        ORDER_COUNT(nsec_locked_total);
                        break;
                case ORDER_NSEC_LOCKED_MAX:
                        ORDER_COUNT(nsec_locked_max);
                        break;
                case ORDER_NSEC_LOCKED_AVG:
                        ORDER_AVG(nsec_locked_total, n_locked);
                        break;
                case ORDER_NSEC_CONTENDED_TOTAL:
                        ORDER_COUNT(nsec_contended_total);
                        break;
                case ORDER_NSEC_CONTENDED_AVG:
                        ORDER_AVG(nsec_contended_total, n_contended);
                        break;
                case ORDER_NSEC_READ_CONTENDED_TOTAL:
                        ORDER_COUNT(nsec_read_contended_total);
                        break;
                case ORDER_NSEC_READ_CONTENDED_MAX:
                        ORDER_COUNT(nsec_read_contended_max);
                        break;
                case ORDER_NSEC_READ_CONTENDED_AVG:
                        ORDER_AVG(nsec_read_contended_total, n_contended);
                        break;
                case ORDER_NSEC_WRITE_CONTENDED_TOTAL:
                        ORDER_COUNT(nsec_write_contended_total);
                        break;
                case ORDER_NSEC_WRITE_CONTENDED_MAX:
                        ORDER_COUNT(nsec_write_contended_max);
                        break;
                case ORDER_NSEC_WRITE_CONTENDED_AVG:
                        ORDER_AVG(nsec_write_contended_total, n_contended);
                        break;
                default:
                        /* Should never be reached. */
                        assert(0);
        }

        /* Fall back to a static ordering. */
        ORDER_COUNT(n_contended);
        ORDER_COUNT(nsec_contended_max);
        ORDER_COUNT(nsec_contended_total);
        ORDER_AVG(nsec_contended_total, n_contended);
        ORDER_COUNT(nsec_read_contended_max);
        ORDER_COUNT(nsec_read_contended_total);
        ORDER_AVG(nsec_read_contended_total, n_contended);
        ORDER_COUNT(nsec_write_contended_max);
        ORDER_COUNT(nsec_write_contended_total);
        ORDER_AVG(nsec_write_contended_total, n_contended);
        ORDER_COUNT(n_owner_changed);
        ORDER_COUNT(n_locked);
        ORDER_COUNT(nsec_locked_max);
        ORDER_COUNT(nsec_locked_total);
        ORDER_AVG(nsec_locked_total, n_locked);
        ORDER_COUNT(id);

        #undef ORDER_AVG
        #undef ORDER_COUNT

        /* Let's make the output deterministic */
        if (a > b)
                return -1;
        else if (a < b)
                return 1;

        return 0;
}

static bool mutex_info_show(struct mutex_info *mi) {

        /* Mutexes used by real-time code are always noteworthy */
        if (mi->realtime)
                return true;

        if (mi->n_locked < show_n_locked_min)
                return false;

        if (mi->n_owner_changed < show_n_owner_changed_min)
                return false;

        if (mi->n_contended < show_n_contended_min)
                return false;

        return true;
}

static bool mutex_info_dump(struct mutex_info *mi) {
        struct location_stats *locked_from;

        if (!mutex_info_show(mi))
                return false;

        fprintf(stderr,
                "\nMutex #%u (0x%p) first referenced by:\n"
                "%s", mi->id, mi->mutex ? (void*) mi->mutex : mi->rwlock ? (void*) mi->rwlock : (void*) mi->rwl, stacktrace_to_string(mi->origin_stacktrace));

        locked_from = mi->locked_from;

        while (locked_from) {
                if (locked_from->n_blocker || locked_from->n_blockee)
                        fprintf(stderr,
                                "    contentious location: blocked %u times, blocker %u times\n"
                                "%s", locked_from->n_blocker, locked_from->n_blockee, stacktrace_to_string( locked_from->stacktrace ));

                locked_from = locked_from->next;
        }

        return true;
}

static char mutex_type_name(int type) {
        switch (type) {

                case PTHREAD_MUTEX_NORMAL:
                        return '-';

                case PTHREAD_MUTEX_RECURSIVE:
                        return 'r';

                case PTHREAD_MUTEX_ERRORCHECK:
                        return 'e';

                case PTHREAD_MUTEX_ADAPTIVE_NP:
                        return 'a';

                default:
                        return '?';
        }
}

static char mutex_protocol_name(int protocol) {
        switch (protocol) {

                case PTHREAD_PRIO_NONE:
                        return '-';

                case PTHREAD_PRIO_INHERIT:
                        return 'i';

                case PTHREAD_PRIO_PROTECT:
                        return 'p';

                default:
                        return '?';
        }
}

static char rwlock_kind_name(int kind) {
        switch (kind) {

                case PTHREAD_RWLOCK_PREFER_READER_NP:
                        return 'r';

                case PTHREAD_RWLOCK_PREFER_WRITER_NP:
                        return 'w';

                case PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP:
                        return 'W';

                default:
                        return '?';
        }
}

/* TODO: add options of other output formats, e.g. JSON */
static bool mutex_info_stat(struct mutex_info *mi) {

        if (!mutex_info_show(mi))
                return false;

        fprintf(stderr,
                "%8u %8u %8u %8u %12.3f %12.3f %12.3f %12.3f %12.3f %12.3f %c%c%c%c%c%c\n",
                mi->id,
                mi->n_locked,
                mi->n_owner_changed,
                mi->n_contended,
                (double) mi->nsec_locked_total / FP_NS_PER_MS,
                (double) mi->nsec_locked_total / mi->n_locked / FP_NS_PER_MS,
                (double) mi->nsec_locked_max / FP_NS_PER_MS,
                (double) mi->nsec_contended_total / FP_NS_PER_MS,
                (double) mi->nsec_contended_total / mi->n_locked / FP_NS_PER_MS,
                (double) mi->nsec_contended_max / FP_NS_PER_MS,
                mi->mutex ? 'M' : mi->rwlock ? 'W' : 'I',
                mi->broken ? '!' : (mi->dead ? 'x' : '-'),
                track_rt ? (mi->realtime ? 'R' : '-') : '.',
                mi->mutex ? mutex_type_name(mi->type) : '.',
                mi->mutex ? mutex_protocol_name(mi->protocol) : '.',
                mi->rwlock ? rwlock_kind_name(mi->kind) : '.');

        if (mi->is_rw) {
                fprintf(stderr,
                        "                                                                        RD %12.3f %12.3f %12.3f ||||||\n",
                        (double) mi->nsec_read_contended_total / FP_NS_PER_MS,
                        (double) mi->nsec_read_contended_total / mi->n_locked / FP_NS_PER_MS,
                        (double) mi->nsec_read_contended_max / FP_NS_PER_MS);
                fprintf(stderr,
                        "                                                                        WR %12.3f %12.3f %12.3f ||||||\n",
                        (double) mi->nsec_write_contended_total / FP_NS_PER_MS,
                        (double) mi->nsec_write_contended_total / mi->n_locked / FP_NS_PER_MS,
                        (double) mi->nsec_write_contended_max / FP_NS_PER_MS);
        }

        return true;
}

static void show_summary_internal(void) {
        struct mutex_info *mi, **table;
        unsigned n, u, i, m;
        uint64_t t;
        long n_cpus;

        t = nsec_now() - nsec_timestamp_setup;

        fprintf(stderr,
                "\n"
                "mutrace: Showing statistics for process %s (PID: %lu).\n", get_prname(), (unsigned long) getpid());

        n = 0;
        for (u = 0; u < hash_size; u++) {
                lock_hash_mutex(u);

                for (mi = alive_mutexes[u]; mi; mi = mi->next)
                        n++;

                for (mi = dead_mutexes[u]; mi; mi = mi->next)
                        n++;
        }

        if (n <= 0) {
                fprintf(stderr,
                        "mutrace: No mutexes used.\n");
                return;
        }

        fprintf(stderr,
                "mutrace: %u mutexes used.\n", n);

        table = malloc(sizeof(struct mutex_info*) * n);

        i = 0;
        for (u = 0; u < hash_size; u++) {
                for (mi = alive_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }

                for (mi = dead_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }
        }
        assert(i == n);

        qsort(table, n, sizeof(table[0]), mutex_info_compare);

        for (i = 0, m = 0; i < n && (show_n_max <= 0 || m < show_n_max); i++)
                m += mutex_info_dump(table[i]) ? 1 : 0;

        if (m > 0) {
                fprintf(stderr,
                        "\n"
                        "mutrace: Showing %u mutexes in order of %s:\n"
                        "\n"
                        " Mutex #   Locked  Changed    Cont. tot.Time[ms] avg.Time[ms] max.Time[ms] tot.Cont[ms] avg.Cont[ms] max.Cont[ms]  Flags\n",
                        m, summary_order_details[summary_order].ui_string);

                for (i = 0, m = 0; i < n && (show_n_max <= 0 || m < show_n_max); i++)
                        m += mutex_info_stat(table[i]) ? 1 : 0;


                if (i < n)
                        fprintf(stderr,
                                "     ...      ...      ...      ...          ...          ...          ...          ...          ...          ... ||||||\n");
                else
                        fprintf(stderr,
                                "                                                                                                                  ||||||\n");

                fprintf(stderr,
                        "                                                                                                                  /|||||\n"
                        "          Object:                                                            M = Mutex, W = RWLock, I = isc_rwlock /||||\n"
                        "           State:                                                                        x = dead, ! = inconsistent /|||\n"
                        "             Use:                                                                        R = used in realtime thread /||\n"
                        "      Mutex Type:                                                         r = RECURSIVE, e = ERRORCHECK, a = ADAPTIVE /|\n"
                        "  Mutex Protocol:                                                                             i = INHERIT, p = PROTECT /\n"
                        "     RWLock Kind: r = PREFER_READER, w = PREFER_WRITER, W = PREFER_WRITER_NONREC \n");

                if (!track_rt)
                        fprintf(stderr,
                                "\n"
                                "mutrace: Note that the flags column R is only valid in --track-rt mode!\n");

        } else
                fprintf(stderr,
                        "\n"
                        "mutrace: No mutex contended according to filtering parameters.\n");

        free(table);

        for (u = 0; u < hash_size; u++)
                unlock_hash_mutex(u);

        fprintf(stderr,
                "\n"
                "mutrace: Total runtime is %0.3f ms.\n", (double) t / FP_NS_PER_MS);

        n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        assert(n_cpus >= 1);

        if (n_cpus <= 1)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: Results for uniprocessor machine. Results might be more interesting\n"
                        "                  when run on an SMP machine!\n");
        else
                fprintf(stderr,
                        "\n"
                        "mutrace: Results for SMP with %li processors.\n", n_cpus);

        if (n_broken > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u inconsistent mutex uses detected. Results might not be reliable.\n"
                        "mutrace:          Fix your program first!\n", n_broken);

        if (n_collisions > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal hash collisions detected. Results might not be as reliable as they could be.\n"
                        "mutrace:          Try to increase --hash-size=, which is currently at %u.\n", n_collisions, hash_size);

        if (n_self_contended > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal mutex contention detected. Results might not be reliable as they could be.\n"
                        "mutrace:          Try to increase --hash-size=, which is currently at %u.\n", n_self_contended, hash_size);
}

/* Print out the summary only the first time this is called. */
static void show_summary(void) {
        static pthread_mutex_t summary_mutex = PTHREAD_MUTEX_INITIALIZER;
        static bool shown_summary = false;

        real_pthread_mutex_lock(&summary_mutex);

        if (shown_summary)
                goto finish;

        show_summary_internal();

finish:
        shown_summary = true;

        real_pthread_mutex_unlock(&summary_mutex);
}

/* Print out the summary every time this is called. */
static void show_summary_again(void) {
        static pthread_mutex_t summary_mutex = PTHREAD_MUTEX_INITIALIZER;

        real_pthread_mutex_lock(&summary_mutex);

        show_summary_internal();

        real_pthread_mutex_unlock(&summary_mutex);
}

static void shutdown(void) {
        show_summary();
}

void exit(int status) {
        show_summary();
        real_exit(status);
}

void _exit(int status) {
        show_summary();
        real_exit(status);
}

void _Exit(int status) {
        show_summary();
        real__Exit(status);
}

void sigusr1_cb(int sig) {
        show_summary_again();
}

static bool is_realtime(void) {
        int policy;

        policy = sched_getscheduler(_gettid());
        assert(policy >= 0);

        policy &= ~SCHED_RESET_ON_FORK;

        return
                policy == SCHED_FIFO ||
                policy == SCHED_RR;
}

static bool verify_frame(const char *s) {

        /* Generated by glibc's native backtrace_symbols() on Fedora */
        if (strstr(s, "/" SONAME "("))
                return false;

        /* Generated by glibc's native backtrace_symbols() on Debian */
        if (strstr(s, "/" SONAME " ["))
                return false;

        /* Generated by backtrace-symbols.c */
        if (strstr(s, __FILE__":"))
                return false;

        return true;
}

static int light_backtrace(void **buffer, int size) {
#if defined(__i386__) || defined(__x86_64__)
        int osize = 0;
        void *stackaddr;
        size_t stacksize;
        void *frame;

        pthread_attr_t attr;
        pthread_getattr_np(pthread_self(), &attr);
        pthread_attr_getstack(&attr, &stackaddr, &stacksize);
        pthread_attr_destroy(&attr);

#if defined(__i386__)
        __asm__("mov %%ebp, %[frame]": [frame] "=r" (frame));
#elif defined(__x86_64__)
        __asm__("mov %%rbp, %[frame]": [frame] "=r" (frame));
#endif
        while (osize < size &&
               frame >= stackaddr &&
               frame < (void *)((char *)stackaddr + stacksize)) {
                buffer[osize++] = *((void **)frame + 1);
                frame = *(void **)frame;
        }

        return osize;
#else
        return real_backtrace(buffer, size);
#endif
}

static struct stacktrace_info generate_stacktrace(void) {
        struct stacktrace_info stacktrace;

        stacktrace.frames = calloc(frames_max, sizeof(void*));
        assert(stacktrace.frames);

        stacktrace.nb_frame = light_backtrace(stacktrace.frames, frames_max);
        assert(stacktrace.nb_frame >= 0);

        return stacktrace;
}

static const char *stacktrace_to_string(struct stacktrace_info stacktrace) {
        char **strings, *ret, *p;
        int i;
        size_t k;
        bool b;

        if (stacktrace.as_string != NULL)
                return stacktrace.as_string;

        strings = real_backtrace_symbols(stacktrace.frames, stacktrace.nb_frame);
        assert(strings);

        k = 0;
        for (i = 0; i < stacktrace.nb_frame; i++)
                k += strlen(strings[i]) + 2;

        ret = malloc(k + 1);
        assert(ret);

        b = false;
        for (i = 0, p = ret; i < stacktrace.nb_frame; i++) {
                if (!b && !verify_frame(strings[i]))
                        continue;

                if (!b && i > 0) {
                        /* Skip all but the first stack frame of ours */
                        *(p++) = '\t';
                        strcpy(p, strings[i-1]);
                        p += strlen(strings[i-1]);
                        *(p++) = '\n';
                }

                b = true;

                *(p++) = '\t';
                strcpy(p, strings[i]);
                p += strlen(strings[i]);
                *(p++) = '\n';
        }

        *p = 0;

        free(strings);

        stacktrace.as_string = ret;

        return ret;
}

static struct mutex_info *mutex_info_add(unsigned long u, pthread_mutex_t *mutex, int type, int protocol) {
        struct mutex_info *mi;

        /* Needs external locking */

        if (alive_mutexes[u])
                __sync_fetch_and_add(&n_collisions, 1);

        mi = calloc(1, sizeof(struct mutex_info));
        assert(mi);

        mi->mutex = mutex;
        mi->type = type;
        mi->protocol = protocol;
        mi->is_rw = false;
        mi->origin_stacktrace = generate_stacktrace();

        mi->next = alive_mutexes[u];
        alive_mutexes[u] = mi;

        return mi;
}

static void mutex_info_remove(unsigned u, pthread_mutex_t *mutex) {
        struct mutex_info *mi, *p;

        /* Needs external locking */

        for (mi = alive_mutexes[u], p = NULL; mi; p = mi, mi = mi->next)
                if (mi->mutex == mutex)
                        break;

        if (!mi)
                return;

        if (p)
                p->next = mi->next;
        else
                alive_mutexes[u] = mi->next;

        mi->dead = true;
        mi->next = dead_mutexes[u];
        dead_mutexes[u] = mi;
}

static struct mutex_info *mutex_info_acquire(pthread_mutex_t *mutex) {
        unsigned long u;
        struct mutex_info *mi;

        u = mutex_hash(mutex);
        lock_hash_mutex(u);

        for (mi = alive_mutexes[u]; mi; mi = mi->next)
                if (mi->mutex == mutex)
                        return mi;

        /* FIXME: We assume that static mutexes are NORMAL, which
         * might not actually be correct */
        return mutex_info_add(u, mutex, PTHREAD_MUTEX_NORMAL, PTHREAD_PRIO_NONE);
}

static void mutex_info_release(pthread_mutex_t *mutex) {
        unsigned long u;

        u = mutex_hash(mutex);
        unlock_hash_mutex(u);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
        int r;
        unsigned long u;

        if (UNLIKELY(!initialized && recursive)) {
                static const pthread_mutex_t template = PTHREAD_MUTEX_INITIALIZER;
                /* Now this is incredibly ugly. */

                memcpy(mutex, &template, sizeof(pthread_mutex_t));
                return 0;
        }

        load_functions();

        r = real_pthread_mutex_init(mutex, mutexattr);
        if (r != 0)
                return r;

        if (LIKELY(initialized && !recursive)) {
                int type = PTHREAD_MUTEX_NORMAL;
                int protocol = PTHREAD_PRIO_NONE;

                recursive = true;
                u = mutex_hash(mutex);
                lock_hash_mutex(u);

                mutex_info_remove(u, mutex);

                if (mutexattr) {
                        int k;

                        k = pthread_mutexattr_gettype(mutexattr, &type);
                        assert(k == 0);

                        k = pthread_mutexattr_getprotocol(mutexattr, &protocol);
                        assert(k == 0);
                }

                mutex_info_add(u, mutex, type, protocol);

                unlock_hash_mutex(u);
                recursive = false;
        }

        return r;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
        unsigned long u;

        assert(initialized || !recursive);

        load_functions();

        if (LIKELY(initialized && !recursive)) {
                recursive = true;

                u = mutex_hash(mutex);
                lock_hash_mutex(u);

                mutex_info_remove(u, mutex);

                unlock_hash_mutex(u);

                recursive = false;
        }

        return real_pthread_mutex_destroy(mutex);
}

/* the memory pointed to by blockee_stacktrace.frames is owned by the caller */
static void update_blocking_counts(struct mutex_info *mi, const struct stacktrace_info blockee_stacktrace) {
        struct location_stats *blockee_callpoint;
        struct thread_info *blocker_ti;

        blocker_ti = mi->holder_list;

        while (blocker_ti) {
                if (!blocker_ti->locked_from && blocker_ti->stacktrace.nb_frame)
                        /* find_locking_location modifies mi with the pointer, if necessary */
                        blocker_ti->locked_from = find_locking_location(mi, blocker_ti->stacktrace);

                if (blocker_ti->locked_from)
                        blocker_ti->locked_from->n_blocker++;

                blocker_ti = blocker_ti->next;
        }

        /* find_locking_location modifies mi with the pointer, if necessary */
        blockee_callpoint = find_locking_location(mi, blockee_stacktrace);
        blockee_callpoint->n_blockee++;
}

static void generic_lock(struct mutex_info *mi, bool busy, bool for_write, uint64_t nsec_contended, bool do_details) {
        struct thread_info *this_ti;
        pid_t tid;

        mi->n_lock_level++;
        mi->n_locked++;

        tid = _gettid();

        this_ti = calloc(1, sizeof(struct thread_info));
        assert(this_ti);
        this_ti->owner = tid;

        if (mi->detailed_tracking) {
                this_ti->stacktrace = generate_stacktrace();
        }

        if (busy) {
                mi->n_contended++;

                mi->nsec_contended_total += nsec_contended;

                if (nsec_contended > mi->nsec_contended_max)
                        mi->nsec_contended_max = nsec_contended;

                if (mi->is_rw) {
                        if (for_write) {
                                mi->nsec_write_contended_total += nsec_contended;

                                if (nsec_contended > mi->nsec_write_contended_max)
                                        mi->nsec_write_contended_max = nsec_contended;
                        }
                        else {
                                mi->nsec_read_contended_total += nsec_contended;

                                if (nsec_contended > mi->nsec_read_contended_max)
                                        mi->nsec_read_contended_max = nsec_contended;
                        }
                }

                if (mi->detailed_tracking) {
                        update_blocking_counts(mi, this_ti->stacktrace);
                }
                else if (do_details && mi->n_contended > 10000) {
                        mi->detailed_tracking = true;
                }
        }

        /*
         * if we're the new owner, and the sole owner, then we can
         * forget the details of the prior holders, if any
         */
        if (mi->n_lock_level == 1) {
                struct thread_info *t;

                while (mi->holder_list) {
                        t = mi->holder_list;
                        mi->holder_list = t->next;
                        if (t->stacktrace.nb_frame) {
                                free(t->stacktrace.frames);

                                if (t->stacktrace.as_string)
                                        free(t->stacktrace.as_string);
                        }
                        free(t);
                }

                /*
                 * only count wall-clock time once, not once per locking thread
                 */
                mi->nsec_timestamp = nsec_now();
        }

        this_ti->next = mi->holder_list;
        mi->holder_list = this_ti;

        if (mi->last_owner != tid) {
                if (mi->last_owner != 0)
                        mi->n_owner_changed++;

                mi->last_owner = tid;
        }

        if (track_rt && !mi->realtime && is_realtime())
                mi->realtime = true;
}

static void mutex_lock(pthread_mutex_t *mutex, bool busy, uint64_t nsec_contended) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level > 0 && mi->type != PTHREAD_MUTEX_RECURSIVE) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_lock(mi, busy, true, nsec_contended, detail_contentious_mutexes);

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                /* During the initialization phase we might be called
                 * inside of dlsym(). Since we'd enter an endless loop
                 * if we tried to resolved the real
                 * pthread_mutex_lock() here then we simply fake the
                 * lock which should be safe since no thread can be
                 * running yet. */

                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_mutex_trylock(mutex);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_mutex_lock(mutex);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r != 0))
                        return r;
        }

        mutex_lock(mutex, busy, wait_time);
        return r;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_mutex_trylock(mutex);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_mutex_timedlock(mutex, abstime);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r == ETIMEDOUT))
                        busy = true;
                else if (UNLIKELY(r != 0))
                        return r;
        }

        mutex_lock(mutex, busy, wait_time);
        return r;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
        int r;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_mutex_trylock(mutex);
        if (UNLIKELY(r != 0))
                return r;

        mutex_lock(mutex, false, 0);
        return r;
}

static void generic_unlock(struct mutex_info *mi) {
        uint64_t t;
        pid_t tid;
        struct thread_info *ti;

        mi->n_lock_level--;

        tid = _gettid();

        ti = find_thread(mi, tid);

        /*
         * I'd love to assert(ti) here, but I don't fee confident in doing so
         */
        if (ti)
                /* 
                 * remove this frame from future consideration when unlocking
                 * needed for recursive locks
                 * from what I can tell, no legitmate thread will hever have ID 0
                 */
                ti->owner = 0;

        /*
         * only count wall-clock time once, not once per locking thread
         */
        if (mi->n_lock_level == 0) {
                t = nsec_now() - mi->nsec_timestamp;
                mi->nsec_locked_total += t;

                if (t > mi->nsec_locked_max)
                        mi->nsec_locked_max = t;
        }
}

static void mutex_unlock(pthread_mutex_t *mutex) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level <= 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_unlock(mi);

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        mutex_unlock(mutex);

        return real_pthread_mutex_unlock(mutex);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
        int r;

        assert(initialized || !recursive);

        load_functions();

        mutex_unlock(mutex);
        r = real_pthread_cond_wait(cond, mutex);

        /* Unfortunately we cannot distuingish mutex contention and
         * the condition not being signalled here. */
        mutex_lock(mutex, false, 0);

        return r;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) {
        int r;

        assert(initialized || !recursive);

        load_functions();

        mutex_unlock(mutex);
        r = real_pthread_cond_timedwait(cond, mutex, abstime);
        mutex_lock(mutex, false, 0);

        return r;
}

int pthread_create(pthread_t *newthread,
                   const pthread_attr_t *attr,
                   void *(*start_routine) (void *),
                   void *arg) {

        load_functions();

        if (UNLIKELY(!threads_existing)) {
                threads_existing = true;
                setup();
        }

        return real_pthread_create(newthread, attr, start_routine, arg);
}

int backtrace(void **array, int size) {
        int r;

        load_functions();

        /* backtrace() internally uses a mutex. To avoid an endless
         * loop we need to disable ourselves so that we don't try to
         * call backtrace() ourselves when looking at that lock. */

        recursive = true;
        r = real_backtrace(array, size);
        recursive = false;

        return r;
}

char **backtrace_symbols(void *const *array, int size) {
        char **r;

        load_functions();

        recursive = true;
        r = real_backtrace_symbols(array, size);
        recursive = false;

        return r;
}

void backtrace_symbols_fd(void *const *array, int size, int fd) {
        load_functions();

        recursive = true;
        real_backtrace_symbols_fd(array, size, fd);
        recursive = false;
}

static struct mutex_info *rwlock_info_add(unsigned long u, pthread_rwlock_t *rwlock, int kind) {
        struct mutex_info *mi;

        /* Needs external locking */

        if (alive_mutexes[u])
                __sync_fetch_and_add(&n_collisions, 1);

        mi = calloc(1, sizeof(struct mutex_info));
        assert(mi);

        mi->rwlock = rwlock;
        mi->kind = kind;
        mi->is_rw = true;
        mi->origin_stacktrace = generate_stacktrace();

        mi->next = alive_mutexes[u];
        alive_mutexes[u] = mi;

        return mi;
}

static void rwlock_info_remove(unsigned long u, pthread_rwlock_t *rwlock) {
        struct mutex_info *mi, *p;

        /* Needs external locking */

        for (mi = alive_mutexes[u], p = NULL; mi; p = mi, mi = mi->next)
                if (mi->rwlock == rwlock)
                        break;

        if (!mi)
                return;

        if (p)
                p->next = mi->next;
        else
                alive_mutexes[u] = mi->next;

        mi->dead = true;
        mi->next = dead_mutexes[u];
        dead_mutexes[u] = mi;
}

static struct mutex_info *rwlock_info_acquire(pthread_rwlock_t *rwlock) {
        unsigned long u;
        struct mutex_info *mi;

        u = rwlock_hash(rwlock);
        lock_hash_mutex(u);

        for (mi = alive_mutexes[u]; mi; mi = mi->next)
                if (mi->rwlock == rwlock)
                        return mi;

        /* FIXME: We assume that static mutexes are RWLOCK_DEFAULT,
         * which might not actually be correct */
        return rwlock_info_add(u, rwlock, PTHREAD_RWLOCK_DEFAULT_NP);
}

static void rwlock_info_release(pthread_rwlock_t *rwlock) {
        unsigned long u;

        u = rwlock_hash(rwlock);
        unlock_hash_mutex(u);
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
        int r;
        unsigned long u;

        if (UNLIKELY(!initialized && recursive)) {
                static const pthread_rwlock_t template = PTHREAD_RWLOCK_INITIALIZER;
                /* Now this is incredibly ugly. */

                memcpy(rwlock, &template, sizeof(pthread_rwlock_t));
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_init(rwlock, attr);
        if (r != 0)
                return r;

        if (LIKELY(initialized && !recursive)) {
                int kind = PTHREAD_RWLOCK_DEFAULT_NP;

                recursive = true;
                u = rwlock_hash(rwlock);
                lock_hash_mutex(u);

                rwlock_info_remove(u, rwlock);

                if (attr) {
                        int k;

                        k = pthread_rwlockattr_getkind_np(attr, &kind);
                        assert(k == 0);
                }

                rwlock_info_add(u, rwlock, kind);

                unlock_hash_mutex(u);
                recursive = false;
        }

        return r;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
        unsigned long u;

        assert(initialized || !recursive);

        load_functions();

        if (LIKELY(initialized && !recursive)) {
                recursive = true;

                u = rwlock_hash(rwlock);
                lock_hash_mutex(u);

                rwlock_info_remove(u, rwlock);

                unlock_hash_mutex(u);

                recursive = false;
        }

        return real_pthread_rwlock_destroy(rwlock);
}

static void rwlock_lock(pthread_rwlock_t *rwlock, bool for_write, bool busy, uint64_t nsec_contended) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = rwlock_info_acquire(rwlock);

        if (mi->n_lock_level > 0 && for_write) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_lock(mi, busy, for_write, nsec_contended, detail_contentious_rwlocks);

        rwlock_info_release(rwlock);
        recursive = false;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_tryrdlock(rwlock);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_rwlock_rdlock(rwlock);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r == ETIMEDOUT))
                        busy = true;
                else if (UNLIKELY(r != 0))
                        return r;
        }

        rwlock_lock(rwlock, false, busy, wait_time);
        return r;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
        int r;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_tryrdlock(rwlock);
        if (UNLIKELY(r != 0))
                return r;

        rwlock_lock(rwlock, false, false, 0);
        return r;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock, const struct timespec *abstime) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_tryrdlock(rwlock);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_rwlock_timedrdlock(rwlock, abstime);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r == ETIMEDOUT))
                        busy = true;
                else if (UNLIKELY(r != 0))
                        return r;
        }

        rwlock_lock(rwlock, false, busy, wait_time);
        return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_trywrlock(rwlock);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_rwlock_wrlock(rwlock);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r == ETIMEDOUT))
                        busy = true;
                else if (UNLIKELY(r != 0))
                        return r;
        }

        rwlock_lock(rwlock, true, busy, wait_time);
        return r;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
        int r;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_trywrlock(rwlock);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        rwlock_lock(rwlock, true, false, 0);
        return r;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock, const struct timespec *abstime) {
        int r;
        bool busy;
        uint64_t wait_time = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_pthread_rwlock_trywrlock(rwlock);
        if (UNLIKELY(r != EBUSY && r != 0))
                return r;

        if (UNLIKELY((busy = (r == EBUSY)))) {
                uint64_t start_time = nsec_now();

                r = real_pthread_rwlock_timedwrlock(rwlock, abstime);

                wait_time = nsec_now() - start_time;

                if (UNLIKELY(r == ETIMEDOUT))
                        busy = true;
                else if (UNLIKELY(r != 0))
                        return r;
        }

        rwlock_lock(rwlock, true, busy, wait_time);
        return r;
}

static void rwlock_unlock(pthread_rwlock_t *rwlock) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = rwlock_info_acquire(rwlock);

        if (mi->n_lock_level <= 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_unlock(mi);

        rwlock_info_release(rwlock);
        recursive = false;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        rwlock_unlock(rwlock);

        return real_pthread_rwlock_unlock(rwlock);
}

static struct mutex_info *isc_rwlock_info_add(unsigned long u, isc_rwlock_t *rwl) {
        struct mutex_info *mi;

        /* Needs external locking */

        if (alive_mutexes[u])
                __sync_fetch_and_add(&n_collisions, 1);

        mi = calloc(1, sizeof(struct mutex_info));
        assert(mi);

        mi->rwl = rwl;
        mi->is_rw = true;
        mi->origin_stacktrace = generate_stacktrace();

        mi->next = alive_mutexes[u];
        alive_mutexes[u] = mi;

        return mi;
}

static void isc_rwlock_info_remove(unsigned long u, isc_rwlock_t *rwl) {
        struct mutex_info *mi, *p;

        /* Needs external locking */

        for (mi = alive_mutexes[u], p = NULL; mi; p = mi, mi = mi->next)
                if (mi->rwl == rwl)
                        break;

        if (!mi)
                return;

        if (p)
                p->next = mi->next;
        else
                alive_mutexes[u] = mi->next;

        mi->dead = true;
        mi->next = dead_mutexes[u];
        dead_mutexes[u] = mi;
}

static struct mutex_info *isc_rwlock_info_acquire(isc_rwlock_t *rwl) {
        unsigned long u;
        struct mutex_info *mi;

        u = isc_rwlock_hash(rwl);
        lock_hash_mutex(u);

        for (mi = alive_mutexes[u]; mi; mi = mi->next)
                if (mi->rwl == rwl)
                        return mi;

        return isc_rwlock_info_add(u, rwl);
}

static void isc_rwlock_info_release(isc_rwlock_t *rwl) {
        unsigned long u;

        u = isc_rwlock_hash(rwl);
        unlock_hash_mutex(u);
}

isc_result_t isc_rwlock_init(isc_rwlock_t *rwl, unsigned int read_quota, unsigned int write_quota) {
        isc_result_t r;
        unsigned long u;

        if (UNLIKELY(!initialized && recursive)) {
                /* this means that we don't yet have the real_isc_rwlock_init pointer
                 * and that somehow we're already in the process of getting information
                 * aobut a lock.  this is bad.
                 * with the pthreads types an attempt is made to recover.
                 * no such attempt will be made here.
                 */
                assert(0);
        }

        load_functions();

        r = real_isc_rwlock_init(rwl, read_quota, write_quota);
        if (r != ISC_R_SUCCESS)
                return r;

        if (LIKELY(initialized && !recursive)) {
                recursive = true;

                u = isc_rwlock_hash(rwl);
                lock_hash_mutex(u);

                isc_rwlock_info_remove(u, rwl);

                isc_rwlock_info_add(u, rwl);

                unlock_hash_mutex(u);
                recursive = false;
        }

        return r;
}

void isc_rwlock_destroy(isc_rwlock_t *rwl) {
        unsigned long u;

        assert(initialized || !recursive);

        load_functions();

        if (LIKELY(initialized && !recursive)) {
                recursive = true;

                u = isc_rwlock_hash(rwl);
                lock_hash_mutex(u);

                isc_rwlock_info_remove(u, rwl);

                unlock_hash_mutex(u);

                recursive = false;
        }

        real_isc_rwlock_destroy(rwl);
}

static void irwlock_lock(isc_rwlock_t *rwl, bool for_write, bool busy, uint64_t nsec_blocked) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = isc_rwlock_info_acquire(rwl);

        if (mi->n_lock_level > 0 && for_write) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_lock(mi, busy, for_write, nsec_blocked, detail_contentious_isc_rwlocks);

        isc_rwlock_info_release(rwl);
        recursive = false;
}

static void irwlock_upgrade_failed(isc_rwlock_t *rwl) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = isc_rwlock_info_acquire(rwl);

        mi->n_contended++;

        if (mi->detailed_tracking) {
                struct stacktrace_info this_stacktrace = generate_stacktrace();

                update_blocking_counts(mi, this_stacktrace);

                free(this_stacktrace.frames);
        }
        else if (detail_contentious_isc_rwlocks && mi->n_contended > 10000) {
                mi->detailed_tracking = true;
        }

        isc_rwlock_info_release(rwl);
        recursive = false;
}

isc_result_t isc_rwlock_lock(isc_rwlock_t *rwl, isc_rwlocktype_t type) {
        isc_result_t r;
        bool busy;
        uint64_t nsec_blocked = 0;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_isc_rwlock_trylock(rwl, type);

        if (UNLIKELY(r != ISC_R_LOCKBUSY && r != ISC_R_SUCCESS))
                return r;

        if (UNLIKELY((busy = (r == ISC_R_LOCKBUSY)))) {
                uint64_t nsec_ts = nsec_now();

                r = real_isc_rwlock_lock(rwl, type);

                nsec_blocked = nsec_now() - nsec_ts;

                if (UNLIKELY( r != ISC_R_SUCCESS))
                        return r;
        }

        irwlock_lock(rwl, type == isc_rwlocktype_write, busy, nsec_blocked);
        return r;
}

isc_result_t isc_rwlock_trylock(isc_rwlock_t *rwl, isc_rwlocktype_t type) {
        isc_result_t r;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_isc_rwlock_trylock(rwl, type);

        if (UNLIKELY(r != ISC_R_SUCCESS))
                return r;

        irwlock_lock(rwl, type == isc_rwlocktype_write, false, 0);
        return r;
}

isc_result_t isc_rwlock_tryupgrade(isc_rwlock_t *rwl) {
        isc_result_t r;

        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        r = real_isc_rwlock_tryupgrade(rwl);

        if (r == ISC_R_LOCKBUSY)
                irwlock_upgrade_failed(rwl);

        return r;
}

static void irwlock_unlock(isc_rwlock_t *rwl) {
        struct mutex_info *mi;

        if (UNLIKELY(!initialized || recursive))
                return;

        recursive = true;
        mi = isc_rwlock_info_acquire(rwl);

        if (mi->n_lock_level <= 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;

                if (raise_trap)
                        DEBUG_TRAP;
        }

        generic_unlock(mi);

        isc_rwlock_info_release(rwl);
        recursive = false;
}

isc_result_t isc_rwlock_unlock(isc_rwlock_t *rwl, isc_rwlocktype_t type) {
        if (UNLIKELY(!initialized && recursive)) {
                assert(!threads_existing);
                return 0;
        }

        load_functions();

        irwlock_unlock(rwl);

        return real_isc_rwlock_unlock(rwl, type);
}

