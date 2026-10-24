/* Extra POSIX stubs needed by winpr on Horizon. */
#include <pthread.h>
#include <time.h>
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* t)
{
    (void)t;
    return pthread_mutex_lock(m);
}

int pipe2(int fds[2], int flags) { (void)fds; (void)flags; return -1; }



