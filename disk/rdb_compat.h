/* Adapt the DICE onbreak() callback to libnix's Ctrl-C signal. */
#include <signal.h>
#include <unistd.h>
#include <devices/trackdisk.h>

/* rdb.c supplies its own definitions for these trackdisk commands. */
#undef TD_READ64
#undef TD_WRITE64
#undef TD_SEEK64
#undef TD_FORMAT64

static int (*rdb_break_handler)(void);

static void rdb_sigint(int sig)
{
    (void)sig;
    rdb_break_handler();
}

static void onbreak(int (*handler)(void))
{
    rdb_break_handler = handler;
    signal(SIGINT, rdb_sigint);
}
