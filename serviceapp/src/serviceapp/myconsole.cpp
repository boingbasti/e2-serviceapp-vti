#include <lib/base/eerror.h>
#include <sys/vfs.h> // for statfs
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "myconsole.h"
#include "common.h"

extern bool g_debugLoggingEnabled;
#define SALOG(fmt, ...) do { \
    if (g_debugLoggingEnabled) { \
        FILE *_sf = fopen("/tmp/serviceapp.log", "a"); \
        if (_sf) { fprintf(_sf, "[serviceapp] " fmt "\n", ##__VA_ARGS__); fflush(_sf); fclose(_sf); } \
    } \
} while(0)

int bidirpipe(int pfd[], const char *cmd , const char * const argv[], const char *cwd )
{
    int pfdin[2];  /* from child to parent */
    int pfdout[2]; /* from parent to child */
    int pfderr[2]; /* stderr from child to parent */
    int pid;       /* child's pid */

    if ( pipe(pfdin) == -1 || pipe(pfdout) == -1 || pipe(pfderr) == -1)
        return(-1);

    if ( ( pid = vfork() ) == -1 )
        return(-1);
    else if (pid == 0) /* child process */
    {
        setsid();
        if ( close(0) == -1 || close(1) == -1 || close(2) == -1 )
            _exit(0);

        if (dup(pfdout[0]) != 0 || dup(pfdin[1]) != 1 || dup(pfderr[1]) != 2 )
            _exit(0);

        if (close(pfdout[0]) == -1 || close(pfdout[1]) == -1 ||
                close(pfdin[0]) == -1 || close(pfdin[1]) == -1 ||
                close(pfderr[0]) == -1 || close(pfderr[1]) == -1 )
            _exit(0);

        for (unsigned int i=3; i < 90; ++i )
            close(i);

        if (cwd)
            chdir(cwd);

        execvp(cmd, (char * const *)argv); 
                /* the vfork will actually suspend the parent thread until execvp is called. thus it's ok to use the shared arg/cmdline pointers here. */
        _exit(0);
    }
    if (close(pfdout[0]) == -1 || close(pfdin[1]) == -1 || close(pfderr[1]) == -1)
            return(-1);

    pfd[0] = pfdin[0];
    pfd[1] = pfdout[1];
    pfd[2] = pfderr[0];

    return(pid);
}

DEFINE_REF(eConsoleContainer);

eConsoleContainer::eConsoleContainer():
    pid(-1),
    killstate(0),
    buffer(2049),
    pollTimer(NULL)
{
    for (int i=0; i < 3; ++i)
    {
        fd[i]=-1;
        filefd[i]=-1;
    }
}

int eConsoleContainer::setCWD( const char *path )
{
    struct stat dir_stat;

    if (stat(path, &dir_stat) == -1)
        return -1;

    if (!S_ISDIR(dir_stat.st_mode))
        return -2;

    m_cwd = path;
    return 0;
}

int eConsoleContainer::execute(eMainloop *context, const char *cmd )
{
    int argc = 3;
    const char *argv[argc + 1];
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = cmd;
    argv[argc] = NULL;

    return execute(context, argv[0], argv);
}

int eConsoleContainer::execute(eMainloop *context, const char *cmdline, const char * const argv[])
{
    if (running())
        return -1;

    pid=-1;
    killstate=0;

    // get one read ,one write and the err pipe to the prog..
    pid = bidirpipe(fd, cmdline, argv, m_cwd.empty() ? 0 : m_cwd.c_str());

    SALOG("eConsoleContainer::execute bidirpipe returned pid=%d, fd[0]=%d", pid, fd[0]);
    if ( pid == -1 )
        return -3;

//  eDebug("pipe in = %d, out = %d, err = %d", fd[0], fd[1], fd[2]);

    ::fcntl(fd[0], F_SETFL, O_NONBLOCK);
    ::fcntl(fd[1], F_SETFL, O_NONBLOCK);
    ::fcntl(fd[2], F_SETFL, O_NONBLOCK);
    SALOG("eConsoleContainer::execute: creating pollTimer");
    pollTimer = eTimer::create(eApp);
    initTimerMutex(pollTimer);
    pollTimer->AddRef();
    SALOG("eConsoleContainer::execute: pollTimer=%p, connecting", (void*)pollTimer);
    pollTimer->timeout.connect(SigC::bind(SigC::slot(&eConsoleContainer::s_pollPipes), this));
    SALOG("eConsoleContainer::execute: connected, starting timer");
    pollTimer->start(20, false);
    SALOG("eConsoleContainer::execute: timer started, returning");
    return 0;
}

eConsoleContainer::~eConsoleContainer()
{
    kill();
}

void eConsoleContainer::kill()
{
    SALOG("kill: enter killstate=%d pid=%d pollTimer=%p", killstate, pid, (void*)pollTimer);
    if ( killstate != -1 && pid != -1 )
    {
        eDebug("user kill(SIGKILL) console App");
        killstate=-1;
        /*
         * Use a negative pid value, to signal the whole process group
         * ('pid' might not even be running anymore at this point)
         */
        ::kill(-pid, SIGKILL);
        /*
         * Collect the exit status after SIGKILL. The kernel closes all file
         * descriptors (and thus releases hardware) when the process exits, not
         * when the parent calls waitpid — but waitpid confirms that the signal
         * was fully processed and prevents zombie accumulation.
         */
        int _ws;
        ::waitpid(pid, &_ws, 0);
        SALOG("kill: SIGKILL sent and process reaped, closing pipes");
        closePipes();
        SALOG("kill: pipes closed");
    }
    SALOG("kill: stepA outbuf.size=%zu", (size_t)outbuf.size());
    while( !outbuf.empty() ) // cleanup out buffer
    {
        queue_data d = outbuf.front();
        outbuf.pop();
        delete [] d.data;
    }
    SALOG("kill: stepB outbuf cleared, pollTimer=%p", (void*)pollTimer);
    if (pollTimer) {
        SALOG("kill: stepC bActive=%d", (int)pollTimer->isActive());
        pollTimer->stop();
        SALOG("kill: stepD stop done");
        pollTimer->Release();
        SALOG("kill: stepE release done");
        pollTimer = NULL;
    }
    SALOG("kill: done");

    for (int i=0; i < 3; ++i)
    {
        if ( filefd[i] >= 0 )
            close(filefd[i]);
    }
}

void eConsoleContainer::sendCtrlC()
{
    if ( killstate != -1 && pid != -1 )
    {
        eDebug("user send SIGINT(Ctrl-C) to console App");
        /*
         * Use a negative pid value, to signal the whole process group
         * ('pid' might not even be running anymore at this point)
         */
        ::kill(-pid, SIGINT);
        /*
         * Also signal the main pid directly: verified live with a real
         * recording that a multithreaded ffmpeg 7 process (DASH recording,
         * network/demuxer worker threads) can swallow a process-group-wide
         * SIGINT entirely and keep running to the source's natural end,
         * while the exact same signal sent straight to its main pid
         * triggers ffmpeg's own term_exit handler immediately (exit code
         * 255, trailer written, file valid). Harmless no-op for single-
         * process apps like exteplayer3, where pid IS already the sole
         * process in its group.
         */
        ::kill(pid, SIGINT);
    }
}

void eConsoleContainer::sendEOF()
{
    if (fd[1] != -1)
    {
        ::close(fd[1]);
        fd[1]=-1;
    }
}

void eConsoleContainer::closePipes()
{
    if (fd[0] != -1)
    {
        ::close(fd[0]);
        fd[0]=-1;
    }
    if (fd[1] != -1)
    {
        ::close(fd[1]);
        fd[1]=-1;
    }
    if (fd[2] != -1)
    {
        ::close(fd[2]);
        fd[2]=-1;
    }
    while( outbuf.size() ) // cleanup out buffer
    {
        queue_data d = outbuf.front();
        outbuf.pop();
        delete [] d.data;
    }
    pid = -1;
}

void eConsoleContainer::readyRead(int what)
{
    SALOG("eConsoleContainer::readyRead what=%d", what);
    bool hungup = what & eSocketNotifier::Hungup;
    if (what & (eSocketNotifier::Priority|eSocketNotifier::Read))
    {
//      eDebug("what = %d");
        char* buf = &buffer[0];
        int rd;
        while((rd = read(fd[0], buf, 2048)) > 0)
        {
            buf[rd]=0;
            if (dataAvail) dataAvail(buf);
            if (stdoutAvail) stdoutAvail(buf);
            if ( filefd[1] >= 0 )
                ::write(filefd[1], buf, rd);
            if (!hungup)
                break;
        }
    }
    readyErrRead(eSocketNotifier::Priority|eSocketNotifier::Read); /* be sure to flush all data which might be already written */
    if (hungup)
    {
        int childstatus;
        int retval = killstate;
        /*
         * We have to call 'wait' on the child process, in order to avoid zombies.
         * Also, this gives us the chance to provide better exit status info to appClosed.
         */
        if (::waitpid(-pid, &childstatus, 0) > 0)
        {
            if (WIFEXITED(childstatus))
            {
                retval = WEXITSTATUS(childstatus);
            }
        }
        closePipes();
        SALOG("eConsoleContainer::readyRead hungup, childstatus=%d, retval=%d, calling appClosed", childstatus, retval);
        if (appClosed) appClosed(retval);
    }
}

void eConsoleContainer::readyErrRead(int what)
{
    if (what & (eSocketNotifier::Priority|eSocketNotifier::Read))
    {
//      eDebug("what = %d");
        char* buf = &buffer[0];
        int rd;
        while((rd = read(fd[2], buf, 2048)) > 0)
        {
/*          for ( int i = 0; i < rd; i++ )
                eDebug("%d = %c (%02x)", i, buf[i], buf[i] );*/
            buf[rd]=0;
            if (dataAvail) dataAvail(buf);
            if (stderrAvail) stderrAvail(buf);
        }
    }
}

void eConsoleContainer::write( const char *data, int len )
{
    char *tmp = new char[len];
    memcpy(tmp, data, len);
    outbuf.push(queue_data(tmp,len));
}

void eConsoleContainer::readyWrite(int what)
{
    if (what&eSocketNotifier::Write && outbuf.size() )
    {
        queue_data &d = outbuf.front();
        int wr = ::write( fd[1], d.data+d.dataSent, d.len-d.dataSent );
        if (wr < 0)
            eDebug("eConsoleContainer write failed (%m)");
        else
            d.dataSent += wr;
        if (d.dataSent == d.len)
        {
            delete [] d.data;
            outbuf.pop();
        }
    }
    if ( !outbuf.size() )
    {
        if ( filefd[0] >= 0 )
        {
            char* buf = &buffer[0];
            int rsize = read(filefd[0], buf, 2048);
            if ( rsize > 0 )
                write(buf, rsize);
            else
            {
                close(filefd[0]);
                filefd[0] = -1;
                ::close(fd[1]);
                eDebug("readFromFile done - closing eConsoleContainer stdin pipe");
                fd[1]=-1;
                if (dataSent) dataSent(0);
            }
        }
    }
}


/* Translate raw poll(2) revents to eSocketNotifier flag values.
   POLLHUP (16) != eSocketNotifier::Hungup (8); mismatch prevents appClosed from ever firing. */
static int pollToSockFlags(int revents)
{
    int f = 0;
    if (revents & POLLIN)  f |= eSocketNotifier::Read;
    if (revents & POLLPRI) f |= eSocketNotifier::Priority;
    if (revents & POLLOUT) f |= eSocketNotifier::Write;
    if (revents & POLLHUP) f |= eSocketNotifier::Hungup;
    return f;
}

void eConsoleContainer::pollPipes()
{
    if (killstate == -1 && pid == -1)
        return; // not running

    struct pollfd pfd[3];
    pfd[0].fd = fd[0];
    pfd[0].events = POLLIN | POLLPRI | POLLHUP;
    pfd[0].revents = 0;

    pfd[1].fd = fd[1];
    pfd[1].events = POLLOUT;
    pfd[1].revents = 0;

    pfd[2].fd = fd[2];
    pfd[2].events = POLLIN | POLLPRI | POLLHUP;
    pfd[2].revents = 0;

    int n = ::poll(pfd, 3, 0);
    if (n > 0)
    {
        if (pfd[0].revents) readyRead(pollToSockFlags(pfd[0].revents));
        if (pfd[2].revents) readyErrRead(pollToSockFlags(pfd[2].revents));
        if (pfd[1].revents && outbuf.size()) readyWrite(pollToSockFlags(pfd[1].revents));
    }
}
