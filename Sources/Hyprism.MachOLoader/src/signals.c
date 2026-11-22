// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Darwin signal API on top of Linux signals. Handlers installed by the Mach-O
// program run behind a host handler that presents Darwin siginfo and ucontext
// layouts, then writes any register changes back to the Linux context.

#define _GNU_SOURCE
#include <asm/sigcontext.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "macho.h"

// ---------------------------------------------------------------------------
// Numbers and flags

#define APPLE_NSIG 32

// Index: Darwin signal number. Value: Linux signal number (0 = no equivalent).
static const int k_sig_from_apple[APPLE_NSIG] = {
    [1] = SIGHUP, [2] = SIGINT, [3] = SIGQUIT, [4] = SIGILL, [5] = SIGTRAP, [6] = SIGABRT, [7] = 0 /* EMT */,
    [8] = SIGFPE, [9] = SIGKILL, [10] = SIGBUS, [11] = SIGSEGV, [12] = SIGSYS, [13] = SIGPIPE, [14] = SIGALRM,
    [15] = SIGTERM, [16] = SIGURG, [17] = SIGSTOP, [18] = SIGTSTP, [19] = SIGCONT, [20] = SIGCHLD,
    [21] = SIGTTIN, [22] = SIGTTOU, [23] = SIGIO, [24] = SIGXCPU, [25] = SIGXFSZ, [26] = SIGVTALRM,
    [27] = SIGPROF, [28] = SIGWINCH, [29] = 0 /* INFO */, [30] = SIGUSR1, [31] = SIGUSR2,
};

static int sig_from_apple(int sig) { return sig > 0 && sig < APPLE_NSIG ? k_sig_from_apple[sig] : 0; }

static int sig_to_apple(int host)
{
    for (int i = 1; i < APPLE_NSIG; i++)
        if (k_sig_from_apple[i] == host)
            return i;
    return 0;
}

#define APPLE_SA_ONSTACK 0x0001
#define APPLE_SA_RESTART 0x0002
#define APPLE_SA_RESETHAND 0x0004
#define APPLE_SA_NOCLDSTOP 0x0008
#define APPLE_SA_NODEFER 0x0010
#define APPLE_SA_NOCLDWAIT 0x0020
#define APPLE_SA_SIGINFO 0x0040

static int flags_from_apple(int f)
{
    return ((f & APPLE_SA_ONSTACK) ? SA_ONSTACK : 0) | ((f & APPLE_SA_RESTART) ? SA_RESTART : 0) |
           ((f & APPLE_SA_RESETHAND) ? SA_RESETHAND : 0) | ((f & APPLE_SA_NOCLDSTOP) ? SA_NOCLDSTOP : 0) |
           ((f & APPLE_SA_NODEFER) ? SA_NODEFER : 0) | ((f & APPLE_SA_NOCLDWAIT) ? SA_NOCLDWAIT : 0) |
           ((f & APPLE_SA_SIGINFO) ? SA_SIGINFO : 0);
}

static void set_from_apple(uint32_t apple, sigset_t *host)
{
    sigemptyset(host);
    for (int s = 1; s < APPLE_NSIG; s++)
        if ((apple & (1u << (s - 1))) && sig_from_apple(s))
            sigaddset(host, sig_from_apple(s));
}

static uint32_t set_to_apple(const sigset_t *host)
{
    uint32_t apple = 0;
    for (int s = 1; s < APPLE_NSIG; s++)
        if (sig_from_apple(s) && sigismember(host, sig_from_apple(s)) == 1)
            apple |= 1u << (s - 1);
    return apple;
}

// ---------------------------------------------------------------------------
// Darwin layouts

struct apple_sigaction {
    void *handler;
    uint32_t mask;
    int flags;
};

struct apple_stack {
    void *ss_sp;
    size_t ss_size;
    int ss_flags;
};

struct apple_siginfo {
    int signo;
    int err;
    int code;
    int pid;
    uint32_t uid;
    int status;
    void *addr;
    uint64_t value;
    long band;
    unsigned long pad[7];
};

struct apple_mcontext64 {
    uint64_t far;
    uint32_t esr;
    uint32_t exception;
    uint64_t x[29];
    uint64_t fp, lr, sp, pc;
    uint32_t cpsr, pad;
    __uint128_t v[32];
    uint32_t fpsr, fpcr;
};

struct apple_ucontext {
    int uc_onstack;
    uint32_t uc_sigmask;
    struct apple_stack uc_stack;
    void *uc_link;
    size_t uc_mcsize;
    struct apple_mcontext64 *uc_mcontext;
};

#define APPLE_SS_ONSTACK 0x1
#define APPLE_SS_DISABLE 0x4

// ---------------------------------------------------------------------------
// Handler dispatch

struct installed {
    void *handler;
    int apple_flags;
};

static struct installed g_handlers[APPLE_NSIG];

static struct fpsimd_context *find_fpsimd(mcontext_t *mc)
{
    for (uint8_t *p = mc->__reserved; p < mc->__reserved + sizeof mc->__reserved;) {
        struct _aarch64_ctx *ctx = (struct _aarch64_ctx *)p;
        if (ctx->magic == 0 || ctx->size == 0)
            return NULL;
        if (ctx->magic == FPSIMD_MAGIC)
            return (struct fpsimd_context *)ctx;
        p += ctx->size;
    }
    return NULL;
}

static uint64_t find_esr(mcontext_t *mc)
{
    for (uint8_t *p = mc->__reserved; p < mc->__reserved + sizeof mc->__reserved;) {
        struct _aarch64_ctx *ctx = (struct _aarch64_ctx *)p;
        if (ctx->magic == 0 || ctx->size == 0)
            return 0;
        if (ctx->magic == ESR_MAGIC)
            return ((struct esr_context *)ctx)->esr;
        p += ctx->size;
    }
    return 0;
}

static void host_handler(int host_sig, siginfo_t *info, void *context)
{
    int sig = sig_to_apple(host_sig);
    struct installed h = g_handlers[sig];
    if (!h.handler)
        return;
    if (!(h.apple_flags & APPLE_SA_SIGINFO)) {
        ((void (*)(int))h.handler)(sig);
        return;
    }

    ucontext_t *uc = context;
    mcontext_t *mc = &uc->uc_mcontext;
    struct fpsimd_context *fp = find_fpsimd(mc);

    // Record hardware faults before the program's handler sees them: if the runtime
    // decides the fault is not managed it restores SIG_DFL and the process dies silently.
    if (host_sig == SIGSEGV || host_sig == SIGBUS || host_sig == SIGILL || host_sig == SIGFPE) {
        static _Atomic int reported;
        if (atomic_fetch_add(&reported, 1) < 16) {
            char pc[160], lr[160];
            macho_describe_address(mc->pc, pc, sizeof pc);
            macho_describe_address(mc->regs[30], lr, sizeof lr);
            macho_log("signal", "fault sig %d code %d addr %p pc %s lr %s sp %#llx tid %d", host_sig, info->si_code,
                      info->si_addr, pc, lr, (unsigned long long)mc->sp, gettid());
            // Walk the frame-pointer chain (both ABIs keep x29 frame records) within this stack.
            uintptr_t fp = mc->regs[29], low = mc->sp, high = mc->sp + (16u << 20);
            for (int depth = 0; depth < 24 && fp >= low && fp < high && (fp & 7) == 0; depth++) {
                uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
                if (!ret)
                    break;
                char where[160];
                macho_describe_address(ret, where, sizeof where);
                macho_log("signal", "  #%02d %s", depth, where);
                if (next <= fp)
                    break;
                fp = next;
            }
        }
    }

    struct apple_mcontext64 amc;
    memset(&amc, 0, sizeof amc);
    amc.far = mc->fault_address;
    amc.esr = (uint32_t)find_esr(mc);
    memcpy(amc.x, mc->regs, sizeof amc.x);
    amc.fp = mc->regs[29];
    amc.lr = mc->regs[30];
    amc.sp = mc->sp;
    amc.pc = mc->pc;
    amc.cpsr = (uint32_t)mc->pstate;
    if (fp) {
        memcpy(amc.v, fp->vregs, sizeof amc.v);
        amc.fpsr = fp->fpsr;
        amc.fpcr = fp->fpcr;
    }

    struct apple_ucontext auc;
    memset(&auc, 0, sizeof auc);
    auc.uc_sigmask = set_to_apple(&uc->uc_sigmask);
    auc.uc_stack.ss_sp = uc->uc_stack.ss_sp;
    auc.uc_stack.ss_size = uc->uc_stack.ss_size;
    auc.uc_mcsize = sizeof amc;
    auc.uc_mcontext = &amc;

    struct apple_siginfo ainfo;
    memset(&ainfo, 0, sizeof ainfo);
    ainfo.signo = sig;
    ainfo.err = errno_to_apple(info->si_errno);
    ainfo.code = info->si_code;
    ainfo.pid = info->si_pid;
    ainfo.uid = info->si_uid;
    ainfo.addr = info->si_addr;
    ainfo.value = (uint64_t)(uintptr_t)info->si_value.sival_ptr;

    ((void (*)(int, struct apple_siginfo *, struct apple_ucontext *))h.handler)(sig, &ainfo, &auc);

    // The runtime redirects threads by editing the context; apply its changes.
    memcpy(mc->regs, amc.x, sizeof amc.x);
    mc->regs[29] = amc.fp;
    mc->regs[30] = amc.lr;
    mc->sp = amc.sp;
    mc->pc = amc.pc;
    mc->pstate = (mc->pstate & ~0xF0000000ull) | (amc.cpsr & 0xF0000000u);
    if (fp) {
        memcpy(fp->vregs, amc.v, sizeof amc.v);
        fp->fpsr = amc.fpsr;
        fp->fpcr = amc.fpcr;
    }
}

static int apple_sigaction(int sig, const struct apple_sigaction *act, struct apple_sigaction *old)
{
    int host = sig_from_apple(sig);
    if (!host) {
        if (old)
            memset(old, 0, sizeof *old);
        return 0; // EMT and INFO do not exist on Linux; accept and ignore
    }

    struct sigaction host_old;
    if (sigaction(host, NULL, &host_old) != 0)
        return -1;
    if (old) {
        old->mask = set_to_apple(&host_old.sa_mask);
        if (host_old.sa_sigaction == host_handler) {
            old->handler = g_handlers[sig].handler;
            old->flags = g_handlers[sig].apple_flags;
        } else {
            // Host handlers expect Linux contexts; hide them from the program.
            old->handler = (host_old.sa_handler == SIG_IGN) ? (void *)SIG_IGN : (void *)SIG_DFL;
            old->flags = 0;
        }
    }
    if (!act)
        return 0;

    struct sigaction host_act;
    memset(&host_act, 0, sizeof host_act);
    set_from_apple(act->mask, &host_act.sa_mask);
    if (act->handler == (void *)SIG_DFL && (host == SIGSEGV || host == SIGBUS))
        macho_log("signal", "program restored the default action for signal %d (unhandled native fault)", sig);
    if (act->handler == (void *)SIG_DFL || act->handler == (void *)SIG_IGN) {
        host_act.sa_handler = (void (*)(int))act->handler;
        host_act.sa_flags = flags_from_apple(act->flags & ~APPLE_SA_SIGINFO);
        g_handlers[sig].handler = NULL;
    } else {
        g_handlers[sig].handler = act->handler;
        g_handlers[sig].apple_flags = act->flags;
        host_act.sa_sigaction = host_handler;
        host_act.sa_flags = flags_from_apple(act->flags) | SA_SIGINFO;
    }
    return sigaction(host, &host_act, NULL);
}

static void *apple_signal(int sig, void *handler)
{
    struct apple_sigaction act = { handler, 0, APPLE_SA_RESTART }, old;
    if (apple_sigaction(sig, &act, &old) != 0)
        return (void *)SIG_ERR;
    return old.handler;
}

static int apple_sigaltstack(const struct apple_stack *ss, struct apple_stack *old)
{
    stack_t host, host_old;
    if (ss) {
        host.ss_sp = ss->ss_sp;
        host.ss_size = ss->ss_size;
        host.ss_flags = (ss->ss_flags & APPLE_SS_DISABLE) ? SS_DISABLE : 0;
    }
    int rc = sigaltstack(ss ? &host : NULL, &host_old);
    if (rc == 0 && old) {
        old->ss_sp = host_old.ss_sp;
        old->ss_size = host_old.ss_size;
        old->ss_flags = ((host_old.ss_flags & SS_DISABLE) ? APPLE_SS_DISABLE : 0) |
                        ((host_old.ss_flags & SS_ONSTACK) ? APPLE_SS_ONSTACK : 0);
    }
    return rc;
}

static int how_from_apple(int how)
{
    // Darwin: SIG_BLOCK 1, SIG_UNBLOCK 2, SIG_SETMASK 3.
    return how == 1 ? SIG_BLOCK : how == 2 ? SIG_UNBLOCK : SIG_SETMASK;
}

static int apple_pthread_sigmask(int how, const uint32_t *set, uint32_t *old)
{
    sigset_t host, host_old;
    if (set)
        set_from_apple(*set, &host);
    int rc = pthread_sigmask(how_from_apple(how), set ? &host : NULL, &host_old);
    if (rc == 0 && old)
        *old = set_to_apple(&host_old);
    return errno_to_apple(rc);
}

// A program that kills itself usually printed why on stderr first, which the
// Android host forwards to logcat from another thread: give it time.
static void before_self_signal(const char *how, int sig)
{
    if (sig != SIGABRT && sig != SIGKILL && sig != SIGSEGV && sig != SIGBUS && sig != SIGILL && sig != SIGTRAP)
        return;
    macho_log("signal", "program sends signal %d to itself via %s", sig, how);
    usleep(500 * 1000);
}

static int apple_pthread_kill(pthread_t t, int sig)
{
    if (sig && pthread_equal(t, pthread_self()))
        before_self_signal("pthread_kill", sig_from_apple(sig));
    return errno_to_apple(pthread_kill(t, sig ? sig_from_apple(sig) : 0));
}
static int apple_kill(pid_t pid, int sig)
{
    if (sig && (pid == getpid() || pid == 0))
        before_self_signal("kill", sig_from_apple(sig));
    return kill(pid, sig ? sig_from_apple(sig) : 0);
}
static int apple_raise(int sig)
{
    before_self_signal("raise", sig_from_apple(sig));
    return raise(sig_from_apple(sig));
}

const struct shim signal_shims[] = {
    { "_sigaction", (void *)apple_sigaction },
    { "_signal", (void *)apple_signal },
    { "_sigaltstack", (void *)apple_sigaltstack },
    { "_pthread_sigmask", (void *)apple_pthread_sigmask },
    { "_pthread_kill", (void *)apple_pthread_kill },
    { "_kill", (void *)apple_kill },
    { "_raise", (void *)apple_raise },
};
const size_t signal_shims_count = SHIM_COUNT(signal_shims);
