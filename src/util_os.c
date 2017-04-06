
#include "util_os.h"
uint32_t max_tries = 10, sleep_between_tries = 500;

#if __linux__
#include <utime.h>
#include <sys/sendfile.h>
#include <sys/wait.h>

uint64_t os_getfilesize(const char *filepath)
{
    struct stat64 st = {};
    staticassert(sizeof(st.st_size) == sizeof(uint64_t));
    log_errno(int ret, stat64(filepath, &st), filepath);
    return ret == 0 ? cast64s64u(st.st_size) : 0;
}

uint64_t os_getmodifiedtime(const char *filepath)
{
    struct stat64 st = {};
    staticassert(sizeof(st.st_size) == sizeof(uint64_t));
    log_errno(int ret, stat64(filepath, &st), filepath);
    return ret == 0 ? st.st_mtime : 0;
}

bool os_setmodifiedtime_nearestsecond(const char *filepath, uint64_t t)
{
    bool ret = false;
    struct stat64 st = {};
    staticassert(sizeof(st.st_size) == sizeof(uint64_t));
    log_errno(int retstat, stat64(filepath, &st), filepath);
    if (retstat >= 0)
    {
        struct utimbuf new_times = { 0 };
        new_times.actime = st.st_atime;
        new_times.modtime = (time_t)t;
        log_errno(int rettime, utime(filepath, &new_times));
        ret = (rettime == 0);
    }

    return ret;
}

bool os_create_dir(const char *s)
{
    bool is_file = false;
    bool exists = os_file_or_dir_exists(s, &is_file);
    if (exists && is_file)
    {
        log_b(0, "exists && is_file %s", s);
        return false;
    }
    else if (exists)
    {
        return true;
    }
    else
    {
        log_errno(bool ret, mkdir(s, 0777 /*permissions*/) == 0, s);
        return ret;
    }
}

bool os_file_or_dir_exists(const char *filepath, bool *is_file)
{
    struct stat64 st = {};
    errno = 0;
    int n = stat64(filepath, &st);
    log_b(n == 0 || errno == ENOENT, "%s %d", filepath, errno);
    *is_file = (st.st_mode & S_IFDIR) == 0;
    return n == 0;
}

check_result os_copy_impl(const char *s1, const char *s2, bool overwrite_ok)
{
    sv_result currenterr = {};
    int read_fd = -1, write_fd = -1;
    if (s_equal(s1, s2))
    {
        sv_log_writefmt("skipping attempted copy of %s onto itself", s1);
        goto cleanup;
    }

    check_b(os_isabspath(s1), "%s", s1);
    check_b(os_isabspath(s2), "%s", s2);
    confirm_writable(s2);
    check_errno(read_fd, open(s1, O_RDONLY | O_BINARY), s1, s2);
    struct stat64 st = {};
    check_errno(_, fstat64(read_fd, &st), s1, s2);
    check_errno(write_fd, open(s2, overwrite_ok ?
        (O_WRONLY | O_CREAT) : (O_WRONLY | O_CREAT | O_EXCL),
        st.st_mode), s1, s2);
    check_errno(_, cast64s32s(sendfile(write_fd, read_fd, NULL, cast64s64u(st.st_size))), s1, s2);

cleanup:
    os_fd_close(&read_fd);
    os_fd_close(&write_fd);
    return currenterr;
}

bool os_copy(const char *s1, const char *s2, bool overwrite_ok)
{
    sv_result res = os_copy_impl(s1, s2, overwrite_ok);
    bool ret = res.code == 0;
    log_b(ret, "got %s", cstr(res.msg));
    sv_result_close(&res);
    return ret;
}

bool os_move(const char *s1, const char *s2, bool overwrite_ok)
{
    if (s_equal(s1, s2))
    {
        sv_log_writefmt("skipping attempted move of %s onto itself", s1);
        return true;
    }

    if (!os_isabspath(s1) || !os_isabspath(s2))
    {
        sv_log_writefmt("must have abs paths but given %s, %s", s1, s2);
        return false;
    }

    /* when more widely supported, use renameat2 which does not have a window. */
    confirm_writable(s2);
    if (!overwrite_ok)
    {
        struct stat64 st = {};
        errno = 0;
        if (stat64(s2, &st) == 0 || errno != ENOENT)
        {
            sv_log_writefmt("cannot move %s, %s, dest already exists %d", s1, s2, errno);
            return false;
        }
    }

    log_errno(int ret, renameat(AT_FDCWD, s1, AT_FDCWD, s2), s1, s2);
    return ret == 0;
}

check_result sv_file_open(sv_file *self, const char *path, const char *mode)
{
    sv_result currenterr = {};
    errno = 0;
    self->file = fopen(path, mode); /* allow fopen */
    check_b(self->file, "failed to open %s %s, got %d", path, mode, errno);
    if (strstr(mode, "w") || strstr(mode, "a"))
    {
        confirm_writable(path);
    }

cleanup:
    return currenterr;
}

check_result os_exclusivefilehandle_open(os_exclusivefilehandle *self,
    const char *path, bool allowread, bool *filenotfound)
{
    sv_result currenterr = {};
    bstring tmp = bstring_open();
    set_self_zero();
    self->loggingcontext = bfromcstr(path);
    if (filenotfound)
    {
        *filenotfound = false;
    }

    errno = 0;
    self->fd = open(path, O_RDONLY | O_BINARY);
    if (self->fd < 0 && errno == ENOENT && !os_file_exists(path) && filenotfound)
    {
        set_self_zero();
        *filenotfound = true;
    }
    else
    {
        char errorname[BUFSIZ] = { 0 };
        os_errno_to_buffer(errno, errorname, countof(errorname));
        check_b(self->fd >= 0, "While trying to open %s, we got %s(%d)", path, errorname, errno);
        check_b(self->fd > 0, "expect fd to be > 0");
        int sharing = allowread ? (LOCK_SH | LOCK_NB) : (LOCK_EX | LOCK_NB);
        check_errno(_, flock(self->fd, sharing), path);
    }
    self->os_handle = NULL;
cleanup:
    bdestroy(tmp);
    return currenterr;
}

check_result os_exclusivefilehandle_stat(os_exclusivefilehandle *self,
    uint64_t *size, uint64_t *modtime, bstring permissions)
{
    sv_result currenterr = {};
    struct stat64 st = {};
    staticassert(sizeof(st.st_size) == sizeof(uint64_t));
    check_errno(_, fstat64(self->fd, &st), cstr(self->loggingcontext));
    *size = (uint64_t)st.st_size;
    *modtime = (uint64_t)st.st_mtime;
    os_get_permissions(&st, permissions);

cleanup:
    return currenterr;
}

bool os_setcwd(const char *s)
{
    errno = 0;
    log_errno(int ret, chdir(s), s);
    return ret == 0;
}

bool os_isabspath(const char *s)
{
    return s && s[0] == '/';
}

bstring os_getthisprocessdir()
{
    errno = 0;
    char buffer[PATH_MAX] = { 0 };
    log_errno(int len, cast64s32s(readlink("/proc/self/exe", buffer, sizeof(buffer) - 1)));
    if (len != -1)
    {
        buffer[len] = '\0'; /* value might not be null term'd */
        log_b(os_isabspath(buffer) && os_dir_exists(buffer), "path=%s", buffer);
        if (os_isabspath(buffer) && os_dir_exists(buffer))
        {
            bstring dir = bstring_open();
            os_get_parent(buffer, dir);
            return dir;
        }
    }

    return bstring_open();
}

bstring os_get_create_appdatadir()
{
    bstring ret = bstring_open();
    char *confighome = getenv("XDG_CONFIG_HOME");
    if (confighome && os_isabspath(confighome))
    {
        bassignformat(ret, "%s/downpoured_glacial_backup", confighome);
        if (os_create_dir(cstr(ret)))
            return ret;
    }

    char *home = getenv("HOME");
    if (home && os_isabspath(home))
    {
        bassignformat(ret, "%s/.local/share/downpoured_glacial_backup", home);
        if (os_create_dir(cstr(ret)))
            return ret;

        bassignformat(ret, "%s/downpoured_glacial_backup", home);
        if (os_create_dir(cstr(ret)))
            return ret;
    }

    /* indicate failure*/
    bstrclear(ret);
    sv_log_writeline("Could not create appdatadir");
    return ret;
}

bool os_lock_file_to_detect_other_instances(const char *path, int *out_code)
{
    /* provide O_CLOEXEC so that the duplicate is closed upon exec. */
    confirm_writable(path);
    log_errno(int pid_file, open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666), path);
    if (pid_file < 0)
    {
        *out_code = 1;
        return false;
    }

    log_errno(int ret, flock(pid_file, LOCK_EX | LOCK_NB), path);
    if (ret != 0 && errno != EWOULDBLOCK)
    {
        close(pid_file);
        *out_code = errno;
    }

    /* intentionally leak the lock... will be closed on process exit */
    return ret != 0;
}

void os_open_dir_ui(const char *dir)
{
    printf("The directory is %s\n", dir);
    if (os_dir_exists(dir) && os_isabspath(dir))
    {
        bstring s = bformat("xdg-open \"%s\"", dir);
        ignore_unused_result(system(cstr(s)));
        bdestroy(s);
    }
}

bool os_remove(const char *s)
{
    bool is_file = false;
    bool ret = true;
    confirm_writable(s);
    if (os_file_or_dir_exists(s, &is_file))
    {
        log_errno(int st, remove(s), s);
        ret = (st == 0);
    }

    return ret;
}

void os_clr_console()
{
    ignore_unused_result(system("clear"));
}

void os_clock_gettime(int64_t *seconds, int32_t *milliseconds)
{
    struct timespec tmspec;
    (void)clock_gettime(CLOCK_REALTIME, &tmspec);
    *seconds = (int64_t)tmspec.tv_sec;
    *milliseconds = (int32_t)(tmspec.tv_nsec / (1000 * 1000)); /* allow cast */
}

void os_sleep(uint32_t milliseconds)
{
    usleep((__useconds_t)(milliseconds * 1000));
}

void os_errno_to_buffer(int err, char *s_out, size_t s_out_len)
{
#if defined(__GNU_LIBRARY__) && defined(_GNU_SOURCE)
    char localbuf[BUFSIZ] = "";
    const char *s = strerror_r(err, localbuf, countof(localbuf));
    if (s)
    {
        strncpy(s_out, s, s_out_len - 1);
    }
#else
#error platform not yet implemented (different platforms have differing strerror_r behavior)
#endif
}

void os_win32err_to_buffer(unused(unsigned long), char *buf, size_t buflen)
{
    memset(buf, 0, buflen);
}

byte *os_aligned_malloc(uint32_t size, uint32_t align)
{
    void *result = 0;
    check_fatal(posix_memalign(&result, align, size) == 0,
        "posix_memalign failed, errno=%d", errno);

    return (byte *)result;
}

void os_aligned_free(byte **ptr)
{
    sv_freenull(*ptr);
}

check_result os_binarypath(const char *binname, bstring out)
{
    sv_result currenterr = {};
    const char *path = getenv("PATH");
    check_b(path && path[0] != '\0', "Path environment variable not found.");
    sv_pseudosplit spl = sv_pseudosplit_open(path);
    sv_pseudosplit_split(&spl, ':');
    check(os_binarypath_impl(&spl, binname, out));
cleanup:
    sv_pseudosplit_close(&spl);
    return currenterr;
}

bstring os_get_tmpdir(const char *subdirname)
{
    bstring ret = bstring_open();
    char *candidates[] = { "TMPDIR", "TMP", "TEMP", "TEMPDIR" };
    for (int i = 0; i < countof(candidates); i++)
    {
        const char *val = getenv(candidates[i]);
        log_b(!val || os_isabspath(val), "relative temp path? %s", val);
        if (val && os_isabspath(val) && os_dir_exists(val))
        {
            bassignformat(ret, "%s%s%s", val, pathsep, subdirname);
            if (os_create_dir(cstr(ret)))
            {
                return ret;
            }
        }
    }

    char *fallback_candidates[] = { P_tmpdir, "/tmp" };
    for (int i = 0; i < countof(fallback_candidates); i++)
    {
        const char *val = fallback_candidates[i];
        if (val && os_isabspath(val) && os_dir_exists(val))
        {
            bassignformat(ret, "%s%s%s", val, pathsep, subdirname);
            if (os_create_dir(cstr(ret)))
            {
                return ret;
            }
        }
    }

    bstrclear(ret);
    return ret;
}

void os_init()
{
    /* not needed in linux */
}

void os_get_permissions(const struct stat64 *st, bstring permissions)
{
    mode_t permissions_only = st->st_mode & 0x0fff; /* don't save the type-of-file */
    bassignformat(permissions, "p%x|g%x|u%x", (uint32_t)permissions_only, /* allow cast */
        (uint32_t)st->st_gid, (uint32_t)st->st_uid); /* allow cast */
}

check_result os_set_permissions(const char *filepath, const bstring permissions)
{
    sv_result currenterr = {};
    bstrlist *list = bstrlist_open();
    if (permissions && blength(permissions))
    {
        bstrlist_splitcstr(list, cstr(permissions), "|");
        uint64_t perms = UINT64_MAX, grp = UINT64_MAX, user = UINT64_MAX;
        for (int i = 0; i < list->qty; i++)
        {
            if (bstrlist_view(list, i)[0] == 'p')
            {
                log_b(uintfromstringhex(bstrlist_view(list, i) + 1, &perms),
                    "could not parse permissions, %s %s", filepath, cstr(permissions));
            }
            else if (bstrlist_view(list, i)[0] == 'g')
            {
                log_b(uintfromstringhex(bstrlist_view(list, i) + 1, &grp),
                    "could not parse group, %s %s", filepath, cstr(permissions));
            }
            else if (bstrlist_view(list, i)[0] == 'u')
            {
                log_b(uintfromstringhex(bstrlist_view(list, i) + 1, &user),
                    "could not parse user, %s %s", filepath, cstr(permissions));
            }
            else
            {
                log_b(0, "unknown field, %s %s", filepath, cstr(permissions));
            }
        }

        if (grp != UINT64_MAX && user != UINT64_MAX)
        {
            sv_log_writefmt("chown %s(%lld,%lld)", filepath, castull(grp), castull(user));
            check_errno(_, chown(filepath, (uid_t)user, (gid_t)grp));
        }

        if (perms != UINT64_MAX)
        {
            sv_log_writefmt("chmod %s(%d)", filepath, castull(perms));
            check_errno(_, chmod(filepath, (mode_t)perms));
        }
    }

cleanup:
    bstrlist_close(list);
    return currenterr;
}

bool os_try_set_readable(const char *filepath)
{
    struct stat64 st = { 0 };
    log_errno(int result, stat64(filepath, &st));
    if (result == 0)
    {
        if ((st.st_mode & S_IRUSR) != 0)
        {
            return true;
        }
        else
        {
            mode_t newmode = (st.st_mode & 0x0fff) | S_IRUSR; /* read permission, owner */
            log_errno(result, chmod(filepath, newmode));
            return result == 0;
        }
    }

    return false;
}

static check_result os_recurse_impl_dir(os_recurse_params *params, bstrlist *dirs,
    bool *retriable_err, bstring currentdir, bstring tmpfullpath, bstring permissions)
{
    sv_result currenterr = {};
    *retriable_err = false;
    bstrclear(tmpfullpath);
    errno = 0;
    DIR *dir = opendir(cstr(currentdir));
    if (dir == NULL && errno != ENOENT)
    {
        *retriable_err = true;
        char buf[BUFSIZ] = "";
        os_errno_to_buffer(errno, buf, countof(buf));
        check_b(0, "Could not list \n%s\nbecause of code %s (%d)",
            cstr(currentdir), buf, errno);
    }

    while (dir)
    {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry)
        {
            if (errno != 0)
            {
                *retriable_err = true;
                char buf[BUFSIZ] = "";
                os_errno_to_buffer(errno, buf, countof(buf));
                check_b(0, "Could not continue listing \n%s\nbecause of code %s %d",
                    cstr(currentdir), buf, errno);
            }
            break;
        }
        else if (s_equal(".", entry->d_name) || s_equal("..", entry->d_name))
        {
            continue;
        }

        bassign(tmpfullpath, currentdir);
        bstr_catstatic(tmpfullpath, "/");
        bcatcstr(tmpfullpath, entry->d_name);
        if (entry->d_type == DT_LNK)
        {
            sv_log_writefmt("skipping symlink, %s", cstr(tmpfullpath));
            bstr_catstatic(tmpfullpath, ", skipped symlink");
            bstrlist_append(params->messages, tmpfullpath);
        }
        else if (entry->d_type == DT_DIR)
        {
            bstrlist_append(dirs, tmpfullpath);
            check(params->callback(params->context, tmpfullpath, UINT64_MAX, UINT64_MAX, NULL));
        }
        else if (entry->d_type == DT_REG)
        {
            /* get file info*/
            struct stat64 st = { 0 };
            errno = 0;
            int statresult = stat64(cstr(tmpfullpath), &st);
            if (statresult < 0 && errno == ENOENT)
            {
                sv_log_writefmt("interesting, not found during iteration %s", cstr(tmpfullpath));
            }
            else if (statresult < 0)
            {
                sv_log_writefmt("stat failed, %s errno=%d", cstr(tmpfullpath), errno);
                bstr_catstatic(tmpfullpath, " could not access stat");
                bstrlist_append(params->messages, tmpfullpath);
            }
            else
            {
                os_get_permissions(&st, permissions);
                check(params->callback(params->context, tmpfullpath,
                    st.st_mtime, cast64s64u(st.st_size), permissions));
            }
        }
    }
cleanup:
    if (dir)
    {
        closedir(dir);
    }

    return currenterr;
}

check_result os_recurse_impl(os_recurse_params *params,
    int currentdepth, bstring currentdir, bstring tmpfullpath, bstring permissions)
{
    sv_result currenterr = {};
    sv_result attempt_dir = {};
    bstrlist *dirs = bstrlist_open();
    for (int attempt = 0; attempt < max_tries; attempt++)
    {
        /* traverse just this directory */
        bool retriable_err = false;
        sv_result_close(&attempt_dir);
        attempt_dir = os_recurse_impl_dir(params, dirs, &retriable_err,
            currentdir, tmpfullpath, permissions);
        if (!attempt_dir.code || !retriable_err)
        {
            check(attempt_dir);
            break;
        }
        else
        {
            bstrlist_clear(dirs);
            os_sleep(sleep_between_tries);
        }
    }

    if (attempt_dir.code)
    {
        /* still seeing a retriable_err after max_tries */
        bstrlist_append(params->messages, attempt_dir.msg);
    }

    /* recurse into subdirectories */
    if (currentdepth < params->max_recursion_depth)
    {
        for (uint32_t i = 0; i < dirs->qty; i++)
        {
            check(os_recurse_impl(params, currentdepth + 1, dirs->entry[i], tmpfullpath, permissions));
        }
    }
    else if (params->max_recursion_depth != 0)
    {
        check_b(0, "recursion limit reached in dir %s", cstr(currentdir));
    }

cleanup:
    bstrlist_close(dirs);
    return currenterr;
}

check_result os_recurse(os_recurse_params *params)
{
    sv_result currenterr = {};
    bstring currentdir = bfromcstr(params->root);
    bstring tmpfullpath = bstring_open();
    bstring permissions = bstring_open();
    check_b(os_isabspath(params->root), "expected full path but got %s", params->root);
    check(os_recurse_impl(params, 0, currentdir, tmpfullpath, permissions));

cleanup:
    bdestroy(currentdir);
    bdestroy(tmpfullpath);
    bdestroy(permissions);
    return currenterr;
}

check_result os_run_process(const char *path,
    const char *const args[], unused(bool), unused(bstring), bstring getoutput, int *retcode)
{
    /* note that by default all file descriptors are passed to child process.
    use flags like O_CLOEXEC to prevent this. */
    sv_result currenterr = {};
    const int READ = 0, WRITE = 1;
    int parent_to_child[2] = {-1, -1};
    int child_to_parent[2] = {-1, -1};
    pid_t pid = 0;
    bstrclear(getoutput);
    check_b(os_isabspath(path), "os_run_process needs full path but given %s.", path);
    check_b(os_file_exists(path), "os_run_process needs existing file but given %s.", path);
    check_errno(_, pipe(parent_to_child));
    check_errno(_, pipe(child_to_parent));
    check_errno(int forkresult, fork());
    if (forkresult == 0)
    {
        /* child */
        check_errno(_, dup2(child_to_parent[WRITE], STDOUT_FILENO));
        check_errno(_, dup2(child_to_parent[WRITE], STDERR_FILENO));
        /* closing a descriptor in the child will close it for the child, but not the parent.
        so close everything we don't need */
        close_set_invalid(child_to_parent[READ]);
        close_set_invalid(parent_to_child[WRITE]);
        close_set_invalid(parent_to_child[READ]);
        close(STDIN_FILENO);

        execv(path, (char *const *)args);
        exit(1); /* not reached, execv does not return */
    }
    else
    {
        /* parent */
        char buffer[4096] = "";
        close_set_invalid(parent_to_child[READ]);
        close_set_invalid(parent_to_child[WRITE]);
        close_set_invalid(child_to_parent[WRITE]);
        while (true)
        {
            log_errno(int read_result, cast64s32s(read(
                child_to_parent[READ], buffer, countof(buffer))));

            if (read_result <= 0)
            {
                /* don't exit on read() failure because some errno like EINTR and EAGAIN are fine */
                break;
            }
            else
            {
                bcatblk(getoutput, buffer, read_result);
            }
        }

        int status = -1;
        check_errno(_, waitpid(pid, &status, 0));
        *retcode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

cleanup:
    close_set_invalid(parent_to_child[READ]);
    close_set_invalid(parent_to_child[WRITE]);
    close_set_invalid(child_to_parent[READ]);
    close_set_invalid(child_to_parent[WRITE]);
    return currenterr;
}

os_perftimer os_perftimer_start()
{
    os_perftimer ret = {};
    ret.timestarted = time(NULL);
    return ret;
}

double os_perftimer_read(const os_perftimer *timer)
{
    time_t unixtime = time(NULL);
    return (double)(unixtime - timer->timestarted);
}

void os_perftimer_close(unused_ptr(os_perftimer))
{
    /* no allocations, nothing needs to be done. */
}

void os_print_mem_usage()
{
    puts("Getting memory usage is not yet implemented.");
}

bstring parse_cmd_line_args(int argc, char **argv, bool *is_low)
{
    bstring ret = bstring_open();
    if (argc >= 3 && s_equal("-directory", argv[1]))
    {
        bassigncstr(ret, argv[2]);
        *is_low = (argc >= 4 && s_equal("-low", argv[3]));
    }
    else if (argc > 1)
    {
        printf("warning: unrecognized command-line syntax.");
    }
    return ret;
}

const bool islinux = true;

#elif _WIN32

#include <VersionHelpers.h>
const int MaxUtf8BytesPerCodepoint = 4;

/* per-thread buffers for UTF8 to UTF16, so allocs aren't needed */
__declspec(thread) sv_wstr threadlocal1;
__declspec(thread) sv_wstr threadlocal2;
__declspec(thread) sv_wstr tls_contents;

void wide_to_utf8(const wchar_t *wide, bstring output)
{
    sv_result currenterr = {};

    /* reserve max space (4 bytes per codepoint) and resize later */
    int widelen = wcslen32s(wide);
    bstring_alloczeros(output, widelen * MaxUtf8BytesPerCodepoint);
    SetLastError(0);
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, widelen,
        (char *)output->data, widelen * MaxUtf8BytesPerCodepoint, NULL, NULL);

    check_fatal(written > 0 || wide[0] == L'\0',
        "to_utf8 failed on string %S lasterr=%lu", wide, GetLastError());

    output->slen = written;
}

void utf8_to_wide(const char *utf8, sv_wstr *output)
{
    sv_result currenterr = {};

    /* reserve size */
    int utf8len = strlen32s(utf8);
    sv_wstr_clear(output);
    sv_wstr_alloczeros(output, utf8len);
    SetLastError(0);
    int written = MultiByteToWideChar(CP_UTF8, 0, utf8, utf8len,
        (wchar_t *)output->arr.buffer, cast32u32s(output->arr.length) - 1 /*nul-term*/);

    check_fatal(written >= 0 && (written > 0 || utf8[0] == '\0'),
        "to_wide failed on string %s lasterr=%lu", utf8, GetLastError());

    sv_wstr_truncate(output, cast32s32u(written));
}

void os_init()
{
    const int initialbuffersize = 4096;
    threadlocal1 = sv_wstr_open(initialbuffersize);
    threadlocal2 = sv_wstr_open(initialbuffersize);
    tls_contents = sv_wstr_open(initialbuffersize);
    check_fatal(IsWindowsVistaOrGreater(),
        "This program does not support an OS earlier than Windows Vista.");
    SetConsoleOutputCP(CP_UTF8);
}

bool os_file_or_dir_exists(const char *filepath, bool *is_file)
{
    utf8_to_wide(filepath, &threadlocal1);
    SetLastError(0);
    DWORD file_attr = GetFileAttributesW(wcstr(threadlocal1));
    log_b(file_attr != INVALID_FILE_ATTRIBUTES || GetLastError() == ERROR_FILE_NOT_FOUND ||
        GetLastError() == ERROR_PATH_NOT_FOUND, "%s %lu", filepath, GetLastError());
    *is_file = ((file_attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
    return file_attr != INVALID_FILE_ATTRIBUTES;
}

uint64_t os_getfilesize(const char *s)
{
    utf8_to_wide(s, &threadlocal1);
    WIN32_FILE_ATTRIBUTE_DATA data = { 0 };
    log_win32(BOOL ret, GetFileAttributesEx(wcstr(threadlocal1), GetFileExInfoStandard, &data),
        FALSE, s);

    if (!ret)
    {
        return 0;
    }

    ULARGE_INTEGER size;
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart;
}

uint64_t os_getmodifiedtime(const char *s)
{
    utf8_to_wide(s, &threadlocal1);
    WIN32_FILE_ATTRIBUTE_DATA data = { 0 };
    log_win32(BOOL ret, GetFileAttributesEx(wcstr(threadlocal1), GetFileExInfoStandard, &data),
        FALSE, s);

    if (!ret)
    {
        return 0;
    }

    ULARGE_INTEGER time;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    return time.QuadPart;
}

bool os_setmodifiedtime_nearestsecond(const char *s, uint64_t t)
{
    bool ret = false;
    utf8_to_wide(s, &threadlocal1);
    log_win32(HANDLE handle, CreateFileW(wcstr(threadlocal1),
        GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL), INVALID_HANDLE_VALUE, s);
    if (handle != INVALID_HANDLE_VALUE)
    {
        FILETIME ft = { lower32(t), upper32(t) };
        log_win32(ret, SetFileTime(handle, NULL, NULL, &ft) != FALSE, false, s);
    }

    CloseHandleNull(&handle);
    return ret;
}

bool os_create_dir(const char *s)
{
    bool is_file = false;
    bool exists = os_file_or_dir_exists(s, &is_file);
    if (exists && is_file)
    {
        log_b(0, "exists && is_file %s", s);
        return false;
    }
    else if (exists)
    {
        return true;
    }
    else
    {
        utf8_to_wide(s, &threadlocal1);
        log_win32(bool ret, CreateDirectoryW(wcstr(threadlocal1), NULL) != FALSE, false, s);
        return ret;
    }
}

bool os_copy(const char *s1, const char *s2, bool overwrite_ok)
{
    confirm_writable(s2);
    utf8_to_wide(s1, &threadlocal1);
    utf8_to_wide(s2, &threadlocal2);
    if (wcscmp(wcstr(threadlocal1), wcstr(threadlocal2)) == 0)
    {
        sv_log_writefmt("skipping attempted copy of %s onto itself", s1);
        return true;
    }
    else
    {
        log_win32(BOOL ret,
            CopyFileW(wcstr(threadlocal1), wcstr(threadlocal2), !overwrite_ok), FALSE, s1, s2);
        return ret != FALSE;
    }
}

bool os_move(const char *s1, const char *s2, bool overwrite_ok)
{
    confirm_writable(s2);
    utf8_to_wide(s1, &threadlocal1);
    utf8_to_wide(s2, &threadlocal2);
    if (wcscmp(wcstr(threadlocal1), wcstr(threadlocal2)) == 0)
    {
        sv_log_writefmt("skipping attempted move of %s onto itself", s1);
        return true;
    }
    else
    {
        DWORD flags = overwrite_ok ? MOVEFILE_REPLACE_EXISTING : 0UL;
        log_win32(BOOL ret,
            MoveFileExW(wcstr(threadlocal1), wcstr(threadlocal2), flags), FALSE, s1, s2);
        return ret != FALSE;
    }
}

check_result sv_file_open(sv_file *self, const char *path, const char *mode)
{
    sv_result currenterr = {};
    utf8_to_wide(path, &threadlocal1);
    utf8_to_wide(mode, &threadlocal2);
    errno = 0;
    self->file = _wfopen(wcstr(threadlocal1), wcstr(threadlocal2)); /* allow fopen */
    check_b(self->file, "failed to open %s %s, got %d", path, mode, errno);
    if (strstr(mode, "w") || strstr(mode, "a"))
    {
        confirm_writable(path);
    }

cleanup:
    return currenterr;
}

check_result os_exclusivefilehandle_open(os_exclusivefilehandle *self,
    const char *path, bool allowread, bool *filenotfound)
{
    sv_result currenterr = {};
    bstring tmp = bstring_open();
    set_self_zero();
    self->loggingcontext = bfromcstr(path);
    utf8_to_wide(path, &threadlocal1);
    if (filenotfound)
    {
        *filenotfound = false;
    }

    SetLastError(0);
    DWORD sharing = allowread ? FILE_SHARE_READ : 0; /* allow others to read, but not write */
    self->os_handle = CreateFileW(wcstr(threadlocal1), GENERIC_READ, sharing,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (self->os_handle == INVALID_HANDLE_VALUE && filenotfound &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
    {
        *filenotfound = true;
    }
    else
    {
        char buf[BUFSIZ] = "";
        os_win32err_to_buffer(GetLastError(), buf, countof(buf));
        check_b(self->os_handle, "We could not exclusively open the file %s because of %s (%lu)",
            path, buf, GetLastError());
        check_errno(self->fd, _open_osfhandle((intptr_t)self->os_handle, O_RDONLY | O_BINARY), path);
    }

cleanup:
    bdestroy(tmp);
    return currenterr;
}

check_result os_exclusivefilehandle_stat(os_exclusivefilehandle *self,
    uint64_t *size, uint64_t *modtime, bstring permissions)
{
    sv_result currenterr = {};
    LARGE_INTEGER filesize = {};

    check_win32(BOOL ret, GetFileSizeEx(
        self->os_handle, &filesize), FALSE, cstr(self->loggingcontext));
    *size = cast64s64u(filesize.QuadPart);

    FILETIME ftcreate, ftaccess, ftwrite;
    check_win32(ret, GetFileTime(
        self->os_handle, &ftcreate, &ftaccess, &ftwrite), FALSE, cstr(self->loggingcontext));
    *modtime = make_uint64(ftwrite.dwHighDateTime, ftwrite.dwLowDateTime);
    bstrclear(permissions); /* we don't store file permissions on windows */
cleanup:
    return currenterr;
}

bool os_setcwd(const char *s)
{
    utf8_to_wide(s, &threadlocal1);
    log_win32(BOOL ret, SetCurrentDirectoryW(wcstr(threadlocal1)), FALSE, s);
    return ret != FALSE;
}

bool os_isabspath(const char *s)
{
    return s && s[0] != '\0' && s[1] == ':' && s[2] == '\\';
}

bstring os_getthisprocessdir()
{
    bstring fullpath = bstring_open(), dir = bstring_open(), retvalue = bstring_open();
    wchar_t buffer[PATH_MAX] = L"";
    log_win32(DWORD chars, GetModuleFileNameW(nullptr, buffer, countof(buffer)), 0);
    if (chars > 0 && buffer[0])
    {
        wide_to_utf8(buffer, fullpath);
        os_get_parent(cstr(fullpath), dir);
        log_b(os_isabspath(cstr(dir)) && os_dir_exists(cstr(dir)), "path=%s", cstr(dir));
        if (os_isabspath(cstr(dir)) && os_dir_exists(cstr(dir)))
        {
            bassign(retvalue, dir);
        }
    }

    bdestroy(fullpath);
    bdestroy(dir);
    return retvalue;
}

bstring os_get_create_appdatadir()
{
    bstring ret = bstring_open();
    wchar_t buff[PATH_MAX] = L"";
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, buff);
    log_b(hr == S_OK, "SHGetFolderPath returned %lu", hr);
    if (SUCCEEDED(hr))
    {
        wide_to_utf8(buff, ret);
        if (blength(ret) && os_isabspath(cstr(ret)))
        {
            bcatcstr(ret, "\\downpoured_glacial_backup");
            if (!os_create_dir(cstr(ret)))
            {
                /* indicate failure */
                bstrclear(ret);
            }
        }
    }

    return ret;
}

bool os_lock_file_to_detect_other_instances(const char *path, int *out_code)
{
    /* fopen_s, unlike fopen, defaults to taking an exclusive lock. */
    FILE *file = NULL;
    utf8_to_wide(path, &threadlocal1);
    confirm_writable(path);
    errno_t ret = _wfopen_s(&file, wcstr(threadlocal1), L"wb"); /* allow fopen */
    log_b(ret == 0, "path %s _wfopen_s returned %d", path, ret);

    /* tell the caller if we received an interesting errno. */
    if (ret != 0 && ret != EACCES)
    {
        *out_code = ret;
    }

    /* intentionally leak the file-handle... will be closed on process exit */
    return file == NULL;
}

void os_open_dir_ui(const char *dir)
{
    bool exists = os_dir_exists(dir);
    utf8_to_wide(dir, &threadlocal1);

    wprintf(L"The directory is \n%s\n%s\n", wcstr(threadlocal1), exists ? L"" : L"(not found)");
    if (exists)
    {
        wchar_t buff[PATH_MAX] = L"";
        if (GetWindowsDirectoryW(buff, _countof(buff)))
        {
            sv_wstr_clear(&threadlocal2);
            sv_wstr_append(&threadlocal2, buff);
            sv_wstr_append(&threadlocal2, L"\\explorer.exe \"");
            sv_wstr_append(&threadlocal2, wcstr(threadlocal1));
            sv_wstr_append(&threadlocal2, L"\"");
            (void)_wsystem(wcstr(threadlocal2));
        }
    }
}

bool os_remove(const char *s)
{
    /* ok if s was previously deleted. */
    bool is_file = false;
    bool ret = true;
    confirm_writable(s);
    if (os_file_or_dir_exists(s, &is_file))
    {
        utf8_to_wide(s, &threadlocal1);
        if (is_file)
        {
            log_win32(ret, DeleteFileW(wcstr(threadlocal1)) != FALSE, false, s);
        }
        else
        {
            log_win32(ret, RemoveDirectoryW(wcstr(threadlocal1)) != FALSE, false, s);
        }
    }

    return ret;
}

void os_clr_console()
{
    (void)system("cls");
}

void os_clock_gettime(int64_t *seconds, int32_t *millisecs)
{
    /* based on code by Asain Kujovic under the MIT license */
    FILETIME wintime;
    GetSystemTimeAsFileTime(&wintime);
    ULARGE_INTEGER time64;
    time64.HighPart = wintime.dwHighDateTime;
    time64.LowPart = wintime.dwLowDateTime;
    time64.QuadPart -= 116444736000000000i64;  /* 1jan1601 to 1jan1970 */
    *seconds = (int64_t)time64.QuadPart / 10000000i64;
    *millisecs = (int32_t)((time64.QuadPart - (*seconds * 10000000i64)) / 10000); /* allow cast */
}

void os_sleep(uint32_t milliseconds)
{
    Sleep(milliseconds);
}

void os_errno_to_buffer(int err, char *str, size_t str_len)
{
    (void)strerror_s(str, str_len - 1, err);
}

void os_win32err_to_buffer(unsigned long err, char *buf, size_t buflen)
{
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        (DWORD)err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, cast64u32u(buflen), NULL);
}

byte *os_aligned_malloc(uint32_t size, uint32_t align)
{
    void *result = _aligned_malloc(size, align);
    check_fatal(result, "_aligned_malloc failed");
    return (byte *)result;
}

void os_aligned_free(byte **ptr)
{
    _aligned_free(*ptr);
    *ptr = NULL;
}

check_result os_binarypath(const char *binname, bstring out)
{
    sv_result currenterr = {};
    const wchar_t *path = _wgetenv(L"PATH");
    check_b(path && path[0] != L'\0', "Path environment variable not found.");
    sv_pseudosplit spl = sv_pseudosplit_open("");
    wide_to_utf8(path, spl.text);
    sv_pseudosplit_split(&spl, ';');
    check(os_binarypath_impl(&spl, binname, out));
cleanup:
    sv_pseudosplit_close(&spl);
    return currenterr;
}

bstring os_get_tmpdir(const char *subdirname)
{
    bstring ret = bstring_open();
    wchar_t *candidates[] = { L"TMPDIR", L"TMP", L"TEMP", L"TEMPDIR" };
    for (int i = 0; i < countof(candidates); i++)
    {
        const wchar_t *val = _wgetenv(candidates[i]);
        if (val)
        {
            wide_to_utf8(val, ret);
            log_b(os_isabspath(cstr(ret)) && os_dir_exists(cstr(ret)), "path=%s", cstr(ret));
            if (blength(ret) && os_isabspath(cstr(ret)) && os_dir_exists(cstr(ret)))
            {
                bformata(ret, "%s%s", pathsep, subdirname);
                if (os_create_dir(cstr(ret)))
                {
                    return ret;
                }
            }
        }
    }

    bstrclear(ret);
    return ret;
}

void createsearchspec(const sv_wstr *dir, sv_wstr *buffer)
{
    wchar_t last_char_of_path = *(wpcstr(dir) + dir->arr.length - 2);
    sv_wstr_clear(buffer);
    sv_wstr_append(buffer, wpcstr(dir));
    if (last_char_of_path == L'\\')
    {
        sv_wstr_append(buffer, L"*"); /* "C:\" to "C:\*" */
    }
    else
    {
        sv_wstr_append(buffer, L"\\*"); /* "C:\path" to "C:\path\*" */
    }
}

static check_result os_recurse_impl_dir(os_recurse_params *params, sv_array *w_dirs,
    bool *retriable_err, sv_wstr *currentdir, sv_wstr *tmpfullpath, bstring tmpfullpath_utf8)
{
    sv_result currenterr = {};
    WIN32_FIND_DATAW find_data = { 0 };
    *retriable_err = false;

    bstrclear(tmpfullpath_utf8);
    createsearchspec(currentdir, tmpfullpath);
    SetLastError(0);
    HANDLE h_find = FindFirstFileW(wpcstr(tmpfullpath), &find_data);
    if (h_find == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND &&
        GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_NO_MORE_FILES)
    {
        *retriable_err = true;
        char buf[BUFSIZ] = "";
        os_win32err_to_buffer(GetLastError(), buf, countof(buf));
        check_b(0, "Could not list \n%S\nbecause of code %s (%lu)",
            wpcstr(tmpfullpath), buf, GetLastError());
    }

    while (h_find != INVALID_HANDLE_VALUE)
    {
        if (wcscmp(L".", find_data.cFileName) != 0 &&
            wcscmp(L"..", find_data.cFileName) != 0)
        {
            /* get the full wide path */
            sv_wstr_clear(tmpfullpath);
            sv_wstr_append(tmpfullpath, wpcstr(currentdir));
            sv_wstr_append(tmpfullpath, L"\\");
            sv_wstr_append(tmpfullpath, find_data.cFileName);
            wide_to_utf8(wpcstr(tmpfullpath), tmpfullpath_utf8);
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            sv_log_writefmt("SKIPPED: symlink %s", cstr(tmpfullpath_utf8));
            bstr_catstatic(tmpfullpath_utf8, ", skipped symlink");
            bstrlist_append(params->messages, tmpfullpath_utf8);
        }
        else if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(L".", find_data.cFileName) != 0 &&
            wcscmp(L"..", find_data.cFileName) != 0)
        {
            /* add to a list of directories */
            sv_wstr dirpath = sv_wstr_open(PATH_MAX);
            sv_wstr_append(&dirpath, wpcstr(tmpfullpath));
            sv_array_append(w_dirs, (const byte *)&dirpath, 1);
            check(params->callback(params->context, tmpfullpath_utf8, UINT64_MAX, UINT64_MAX, NULL));
        }
        else if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            uint64_t modtime = make_uint64(
                find_data.ftLastWriteTime.dwHighDateTime, find_data.ftLastWriteTime.dwLowDateTime);
            uint64_t filesize = make_uint64(
                find_data.nFileSizeHigh, find_data.nFileSizeLow);
            check(params->callback(params->context, tmpfullpath_utf8, modtime, filesize, 0));
        }

        SetLastError(0);
        BOOL found_next_file = FindNextFileW(h_find, &find_data);
        if (!found_next_file && GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_NO_MORE_FILES)
        {
            /* h_find is invalid after failed FindNextFile; don't retry even for ERROR_ACCESS_DENIED */
            *retriable_err = true;
            char buf[BUFSIZ] = "";
            os_win32err_to_buffer(GetLastError(), buf, countof(buf));
            check_b(0, "Could not continue listing after \n%S\nbecause of code %s (%lu)",
                wpcstr(tmpfullpath), buf, GetLastError());
        }
        else if (!found_next_file)
        {
            break;
        }
    }
cleanup:
    FindClose(h_find);
    return currenterr;
}

check_result os_recurse_impl(os_recurse_params *params,
    int currentdepth, sv_wstr *currentdir, sv_wstr *tmpfullpath, bstring tmpfullpath_utf8)
{
    sv_result currenterr = {};
    sv_result attempt_dir = {};
    sv_array w_dirs = sv_array_open(sizeof32u(sv_wstr), 0);
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        /* traverse just this directory */
        bool retriable_err = false;
        sv_result_close(&attempt_dir);
        attempt_dir = os_recurse_impl_dir(params,
            &w_dirs, &retriable_err, currentdir, tmpfullpath, tmpfullpath_utf8);

        if (!attempt_dir.code || !retriable_err)
        {
            check(attempt_dir);
            break;
        }
        else
        {
            sv_wstrlist_clear(&w_dirs);
            os_sleep(sleep_between_tries);
        }
    }

    if (attempt_dir.code)
    {
        /* still seeing a retriable_err after max_tries */
        bstrlist_append(params->messages, attempt_dir.msg);
    }

    /* recurse into subdirectories */
    if (currentdepth < params->max_recursion_depth)
    {
        for (uint32_t i = 0; i < w_dirs.length; i++)
        {
            sv_wstr *dir = (sv_wstr *)sv_array_at(&w_dirs, i);
            check(os_recurse_impl(params, currentdepth + 1, dir, tmpfullpath, tmpfullpath_utf8));
        }
    }
    else if (params->max_recursion_depth != 0)
    {
        check_b(0, "recursion limit reached in dir %S", wpcstr(currentdir));
    }

cleanup:
    sv_wstrlist_clear(&w_dirs);
    sv_array_close(&w_dirs);
    return currenterr;
}

check_result os_recurse(os_recurse_params *params)
{
    sv_result currenterr = {};
    sv_wstr currentdir = sv_wstr_open(PATH_MAX);
    sv_wstr tmpfullpath = sv_wstr_open(PATH_MAX);
    bstring tmpfullpath_utf8 = bstring_open();
    utf8_to_wide(params->root, &currentdir);
    check_b(os_isabspath(params->root), "expected full path but got %s", params->root);
    check(os_recurse_impl(params, 0, &currentdir, &tmpfullpath, tmpfullpath_utf8));

cleanup:
    sv_wstr_close(&currentdir);
    sv_wstr_close(&tmpfullpath);
    bdestroy(tmpfullpath_utf8);
    return currenterr;
}

check_result os_run_process_ex(const char *path, const char *const args[], os_proc_op op,
    os_exclusivefilehandle *providestdin, bool fastjoinargs, bstring useargscombined,
    bstring getoutput, int *retcode)
{
    sv_result currenterr = {};
    bstrclear(getoutput);
    HANDLE childstd_in_rd = NULL;
    HANDLE childstd_in_wr = NULL;
    HANDLE childstd_out_rd = NULL;
    HANDLE childstd_out_wr = NULL;
    PROCESS_INFORMATION procinfo = { 0 };
    STARTUPINFOW startinfo = { 0 };
    SECURITY_ATTRIBUTES sa = { 0 };
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    check_b(os_isabspath(path), "os_run_process needs full path but given %s.", path);
    check_b(os_file_exists(path), "os_run_process needs existing file but given %s.", path);
    check_b((op == os_proc_stdin) == (providestdin != 0), "bad parameters.");

    check_b(argvquote(path, args, useargscombined, fastjoinargs), "On Windows, we do not support "
        "filenames containing quote characters or ending with backslash, but got %s",
        cstr(useargscombined));

    utf8_to_wide(cstr(useargscombined), &threadlocal1);
    startinfo.cb = sizeof(STARTUPINFO);

    /*
    Don't use popen() or system(). Simpler, and does work, but slower (opens a shell process),
    harder to run securely, will fail in gui contexts, sometimes needs all file buffers flushed,
    and there are reports that repeated system() calls will alert a/v software

    Make sure we avoid deadlock.
    Deadlock example: we 1) redirect stdout and stdin
    2) write to the stdin pipe and close it
    3) read from the stdout pipe and close it
    This might appear to work, because of buffering, but consider a child that writes a lot of
    data to stdout before it even listens to stdin.
    The child will fill up the buffer and then hang waiting for someone to listen to its stdout.
    */
    check_win32(_, CreatePipe(&childstd_out_rd, &childstd_out_wr, &sa, 0),
        FALSE, cstr(useargscombined));
    check_win32(_, SetHandleInformation(childstd_out_rd, HANDLE_FLAG_INHERIT, 0),
        FALSE, cstr(useargscombined));
    check_win32(_, CreatePipe(&childstd_in_rd, &childstd_in_wr, &sa, 0),
        FALSE, cstr(useargscombined));
    check_win32(_, SetHandleInformation(childstd_in_wr, HANDLE_FLAG_INHERIT, 0),
        FALSE, cstr(useargscombined));

    /* If we had a child process that called CloseHandle on its stderr or stdout,
    we would use duplicatehandle. */
    startinfo.hStdError = op == os_proc_stdin ? INVALID_HANDLE_VALUE : childstd_out_wr;
    startinfo.hStdOutput = op == os_proc_stdin ? INVALID_HANDLE_VALUE : childstd_out_wr;
    startinfo.hStdInput = op == os_proc_stdin ? childstd_in_rd : INVALID_HANDLE_VALUE;
    startinfo.dwFlags |= STARTF_USESTDHANDLES;

    check_win32(_, CreateProcessW(NULL,
        (wchar_t *)wcstr(threadlocal1), /* nb: CreateProcess needs a writable buffer */
        NULL,          /* process security attributes */
        NULL,          /* primary thread security attributes */
        TRUE,          /* handles are inherited */
        0,             /* creation flags */
        NULL,          /* use parent's environment */
        NULL,          /* use parent's current directory */
        &startinfo,
        &procinfo),
        FALSE, cstr(useargscombined));

    char buf[4096] = "";
    CloseHandleNull(&childstd_out_wr);
    CloseHandleNull(&childstd_in_rd);
    if (op == os_proc_stdin)
    {
        LARGE_INTEGER location = { 0 };
        check_b(providestdin->os_handle > 0, "invalid file handle %s",
            cstr(providestdin->loggingcontext));
        check_win32(_, SetFilePointerEx(providestdin->os_handle, location, NULL, FILE_BEGIN),
            FALSE, cstr(providestdin->loggingcontext));
        while (true)
        {
            DWORD dw_read = 0;
            BOOL success = ReadFile(providestdin->os_handle, buf, countof(buf), &dw_read, NULL);
            if (!success || dw_read == 0)
            {
                break;
            }

            DWORD dw_written = 0;
            log_win32(success, WriteFile(childstd_in_wr, buf, dw_read, &dw_written, NULL), FALSE,
                cstr(providestdin->loggingcontext));
            check_b(dw_read == dw_written, "%s wanted to write %d but only wrote %d",
                cstr(providestdin->loggingcontext), dw_read, dw_written);
            if (!success)
            {
                break;
            }
        }
    }
    else if (op == os_proc_stdout_stderr)
    {
        while (true)
        {
            SetLastError(0);
            DWORD dw_read = 0;
            BOOL ret = ReadFile(childstd_out_rd, buf, countof(buf), &dw_read, NULL);
            log_b(ret || GetLastError() == 0 || GetLastError() == ERROR_BROKEN_PIPE,
                "lasterr=%lu", GetLastError());
            if (!ret || dw_read == 0)
            {
                break;
            }

            bcatblk(getoutput, buf, dw_read);
        }
    }
    else
    {
        check_b(0, "not reached");
    }

    CloseHandleNull(&childstd_in_wr);
    log_win32(DWORD result, WaitForSingleObject(procinfo.hProcess, INFINITE),
        WAIT_FAILED, cstr(useargscombined));

    log_b(result == WAIT_OBJECT_0, "got wait result %lu", result);

    DWORD dwret = 1;
    log_win32(_, GetExitCodeProcess(procinfo.hProcess, &dwret), FALSE, cstr(useargscombined));
    log_b(dwret == 0, "cmd=%s ret=%lu", cstr(useargscombined), dwret);
    *retcode = (int)dwret; /* allow cast */

cleanup:
    CloseHandleNull(&procinfo.hProcess);
    CloseHandleNull(&procinfo.hThread);
    CloseHandleNull(&childstd_in_rd);
    CloseHandleNull(&childstd_in_wr);
    CloseHandleNull(&childstd_out_rd);
    CloseHandleNull(&childstd_out_wr);
    return currenterr;
}

check_result os_run_process(const char *path, const char *const args[], bool fastjoinargs,
    bstring useargscombined, bstring getoutput, int *retcode)
{
    return os_run_process_ex(path, args, os_proc_stdout_stderr, NULL, fastjoinargs,
        useargscombined, getoutput, retcode);
}

check_result os_set_permissions(unused_ptr(const char), unused(const bstring))
{
    return OK;
}

bool os_try_set_readable(unused_ptr(const char))
{
    return true;
}

os_perftimer os_perftimer_start()
{
    os_perftimer ret = {};
    LARGE_INTEGER time = { 0 };
    QueryPerformanceCounter(&time);
    ret.timestarted = time.QuadPart;
    return ret;
}

double os_perftimer_read(const os_perftimer *timer)
{
    LARGE_INTEGER time = { 0 };
    QueryPerformanceCounter(&time);
    int64_t diff = time.QuadPart - timer->timestarted;
    LARGE_INTEGER freq = { 0 };
    QueryPerformanceFrequency(&freq);
    return (diff) / ((double)freq.QuadPart);
}

void os_perftimer_close(os_perftimer *)
{
    /* no allocations, nothing needs to be done. */
}

void os_print_mem_usage()
{
    PROCESS_MEMORY_COUNTERS_EX obj = {};
    obj.cb = sizeof(obj);
    log_win32(BOOL ret, GetProcessMemoryInfo(
        GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&obj, obj.cb), FALSE);

    if (ret)
    {
        printf("\tPageFaultCount: %lu\n", obj.PageFaultCount);
        printf("\tPeakWorkingSetSize:(%fMb)\n", obj.PeakWorkingSetSize / (1024.0*1024.0));
        printf("\tWorkingSetSize: (%fMb)\n", obj.WorkingSetSize / (1024.0*1024.0));
        printf("\tPagefileUsage: (%fMb)\n", obj.PagefileUsage / (1024.0*1024.0));
        printf("\tPeakPagefileUsage: (%fMb)\n", obj.PeakPagefileUsage / (1024.0*1024.0));
        printf("\tPrivateUsage: (%fMb)\n", obj.PrivateUsage / (1024.0*1024.0));
    }
}

check_result os_run_process_as(PROCESS_INFORMATION *outinfo, wchar_t *cmd,
    const wchar_t *user, const wchar_t *pass)
{
    sv_result currenterr = {};
    memset(outinfo, 0, sizeof(*outinfo));

    /* We'll use CreateProcessWithLogonW instead of LogonUser, according to MSDN
    it needs fewer privileges and so is likely to work on more configs. */
    STARTUPINFOW startinfo = { 0 };
    startinfo.cb = sizeof(STARTUPINFOW);
    SetLastError(0);
    BOOL ret = CreateProcessWithLogonW(user,
        NULL /* domain */, pass, 0, NULL, cmd, 0, 0, 0, &startinfo, outinfo);

    if (!ret)
    {
        char errorname[BUFSIZ] = { 0 };
        os_win32err_to_buffer(GetLastError(), errorname, countof(errorname));
        currenterr.code = 1;
        currenterr.msg = bfromcstr(errorname);
    }

    return currenterr;
}

check_result os_restart_as_other_user(const char *data_dir)
{
    sv_result currenterr = {};
    bstring othername = bstring_open();
    bstring otherpass = bstring_open();
    sv_wstr wothername = sv_wstr_open(0);
    sv_wstr wotherpass = sv_wstr_open(0);
    PROCESS_INFORMATION procinfo = { 0 };
    sv_result res = {};
    wchar_t modulepath[PATH_MAX] = L"";
    check_win32(_, GetModuleFileNameW(nullptr, modulepath, countof(modulepath)), 0,
        "Could not get path to glacial_backup.exe");
    check_win32(_, GetFileAttributesW(modulepath), INVALID_FILE_ATTRIBUTES,
        "Could not get path to glacial_backup.exe");
    check_b(os_dir_exists(data_dir), "not found %s", data_dir);

    /* get utf16 version of data_dir */
    utf8_to_wide(data_dir, &threadlocal1);

    /* build arguments in tls_path2 */
    sv_wstr_clear(&threadlocal2);
    sv_wstr_append(&threadlocal2, L"\"");
    sv_wstr_append(&threadlocal2, modulepath);
    sv_wstr_append(&threadlocal2, L"\" -directory \"");
    sv_wstr_append(&threadlocal2, wcstr(threadlocal1));
    sv_wstr_append(&threadlocal2, L"\" -low");

    /* CreateProcess takes a writable buffer, luckily we already have one. */
    wchar_t *command = (wchar_t *)wcstr(threadlocal2);
    while (true)
    {
        ask_user_prompt("Please enter the username, or 'q' to cancel.", true, othername);
        if (!blength(othername))
        {
            break;
        }

        ask_user_prompt("Please enter the password, or 'q' to cancel.", true, otherpass);
        if (!blength(otherpass))
        {
            break;
        }

        utf8_to_wide(cstr(othername), &wothername);
        utf8_to_wide(cstr(otherpass), &wotherpass);
        sv_result_close(&res);
        res = os_run_process_as(&procinfo, command, wcstr(wothername), wcstr(wotherpass));
        if (res.code)
        {
            printf("Could not start process. %s.\n", cstr(res.msg));
            if (!ask_user_yesno("Try again?"))
            {
                break;
            }
        }
        else
        {
            puts("\n\nStarted other process successfully.");
            exit(0);
        }
    }

cleanup:
    CloseHandleNull(&procinfo.hProcess);
    CloseHandleNull(&procinfo.hThread);
    sv_result_close(&res);
    memzero_s(otherpass->data, cast32s32u(max(0, otherpass->slen)));
    memzero_s(wotherpass.arr.buffer, wotherpass.arr.elementsize * wotherpass.arr.length);
    bdestroy(othername);
    bdestroy(otherpass);
    sv_wstr_close(&wothername);
    sv_wstr_close(&wotherpass);
    return currenterr;
}

bstring parse_cmd_line_args(int argc, wchar_t *argv[], bool *is_low)
{
    bstring ret = bstring_open();
    if (argc >= 3 && wcscmp(L"-directory", argv[1]) == 0)
    {
        wide_to_utf8(argv[2], ret);
        *is_low = (argc >= 4 && wcscmp(L"-low", argv[3]) == 0);
    }
    else if (argc > 1)
    {
        printf("warning: unrecognized command-line syntax.");
    }

    return ret;
}

const bool islinux = false;

#else
#error "platform not yet supported"
#endif

bool os_file_exists(const char *filepath)
{
    bool is_file = false;
    return os_file_or_dir_exists(filepath, &is_file) && is_file;
}

bool os_dir_exists(const char *filepath)
{
    bool is_file = false;
    return os_file_or_dir_exists(filepath, &is_file) && !is_file;
}

void sv_file_close(sv_file *self)
{
    if (self && self->file)
    {
        fclose(self->file);
        set_self_zero();
    }
}

check_result sv_file_writefile(const char *filepath,
    const char *contents, const char *mode)
{
    sv_result currenterr = {};
    sv_file file = {};
    check(sv_file_open(&file, filepath, mode));
    fputs(contents, file.file);

cleanup:
    sv_file_close(&file);
    return currenterr;
}

check_result sv_file_readfile(const char *filepath, bstring contents)
{
    sv_result currenterr = {};
    sv_file file = {};

    /* get length of file*/
    uint64_t filesize = os_getfilesize(filepath);
    check_b(filesize < 10 * 1024 * 1024,
        "file %s is too large to be read into a string", filepath);

    /* allocate mem */
    bstrclear(contents);
    checkstr(bstring_alloczeros(contents, cast64u32s(filesize)));

    /* copy the data. don't support 'text' mode because the length might differ */
    check(sv_file_open(&file, filepath, "rb"));
    size_t read = fread(contents->data, 1, (size_t)filesize, file.file);
    contents->slen = cast64u32s(filesize);
    check_b(filesize == 0 || read != 0, "fread failed");

cleanup:
    sv_file_close(&file);
    return currenterr;
}

void os_fd_close(int *fd)
{
    if (*fd && *fd != -1)
    {
        log_errno(_, close(*fd), NULL);
        *fd = 0;
    }
}

check_result os_exclusivefilehandle_tryuntil_open(os_exclusivefilehandle *self,
    const char *path, bool allowread, bool *filenotfound)
{
    sv_result res = {};
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        /* we're trying to open an exclusive handle, so it might take a few attempts */
        sv_result_close(&res);
        res = os_exclusivefilehandle_open(self, path, allowread, filenotfound);
        if (res.code == 0 || (filenotfound && *filenotfound))
        {
            /* important to return early if file is not found. consider the following:
            1) user adds directory containing 1000files and runs a successful backup
            2) this directory is renamed
            3) backup is run again. we'll get 1000 file-not-founds; we shouldn't wait 5s for each. */
            break;
        }
        else if (res.code)
        {
            os_sleep(sleep_between_tries);
        }
    }

    return res;
}

void os_exclusivefilehandle_close(os_exclusivefilehandle *self)
{
    if (self && self->fd && self->fd != -1)
    {
        /* msdn: don't need to call CloseHandle on os_handle, closing fd is sufficient. */
        os_fd_close(&self->fd);
        bdestroy(self->loggingcontext);
        set_self_zero();
    }
}

bool os_tryuntil_remove(const char *s)
{
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        if (os_remove(s))
        {
            return true;
        }

        os_sleep(sleep_between_tries);
    }
    return false;
}

bool os_tryuntil_move(const char *s1, const char *s2, bool overwrite_ok)
{
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        if (os_move(s1, s2, overwrite_ok))
        {
            return true;
        }

        os_sleep(sleep_between_tries);
    }
    return false;
}

bool os_tryuntil_copy(const char *s1, const char *s2, bool overwrite_ok)
{
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        if (os_copy(s1, s2, overwrite_ok))
        {
            return true;
        }

        os_sleep(sleep_between_tries);
    }
    return false;
}

bool os_create_dirs(const char *s)
{
    if (!os_isabspath(s))
    {
        return false;
    }

    if (os_dir_exists(s) || os_create_dir(s))
    {
        return true;
    }

    bstrlist *list = bstrlist_open();
    bstring parent = bfromcstr(s);
    while (s_contains(cstr(parent), pathsep))
    {
        bstrlist_append(list, parent);
        os_get_parent(bstrlist_view(list, list->qty - 1), parent);
    }

    for (int i = list->qty - 1; i >= 0; i--)
    {
        if (!os_create_dir(bstrlist_view(list, i)))
        {
            return false;
        }
    }

    bstrlist_close(list);
    bdestroy(parent);
    return os_dir_exists(s);
}

/* on failure, places stderr into getoutput */
int os_tryuntil_run_process(const char *path,
    const char *const args[], bool fastjoinargs, bstring useargscombined, bstring getoutput)
{
    int retcode = INT_MAX;
    for (uint32_t attempt = 0; attempt < max_tries; attempt++)
    {
        sv_result res = os_run_process(path, args, fastjoinargs, useargscombined, getoutput, &retcode);

        if (res.code || retcode)
        {
            bconcat(getoutput, res.msg);
            sv_result_close(&res);
            os_sleep(sleep_between_tries);
            retcode = retcode ? retcode : -1;
        }
        else
        {
            break;
        }
    }

    return retcode;
}

void os_get_filename(const char *fullpath, bstring filename)
{
    const char *last_slash = strrchr(fullpath, pathsep[0]);
    if (last_slash)
    {
        bassigncstr(filename, last_slash + 1);
    }
    else
    {
        bassigncstr(filename, fullpath);
    }
}

void os_get_parent(const char *fullpath, bstring dir)
{
    const char *last_slash = strrchr(fullpath, pathsep[0]);
    if (last_slash)
    {
        bassignblk(dir, fullpath, cast64s32s(last_slash - fullpath));
    }
    else
    {
        /* we were given a leaf name, so set dir to the empty string. */
        bstrclear(dir);
    }
}

void os_split_dir(const char *fullpath, bstring dir, bstring filepath)
{
    os_get_parent(fullpath, dir);
    os_get_filename(fullpath, filepath);
}

bool os_recurse_is_dir(uint64_t modtime, uint64_t filesize)
{
    return modtime == UINT64_MAX && filesize == UINT64_MAX;
}

bool os_dir_empty(const char *dir)
{
    /* returns false if directory could not be accessed. */
    bstrlist *list = bstrlist_open();
    sv_result result_listfiles = os_listfiles(dir, list, false);
    int count_files = list->qty;
    sv_result result_listdirs = os_listdirs(dir, list, false);
    int count_dirs = list->qty;
    bool ret = result_listfiles.code == 0 && result_listdirs.code == 0 &&
        count_files + count_dirs == 0;

    sv_result_close(&result_listfiles);
    sv_result_close(&result_listdirs);
    bstrlist_close(list);
    return ret;
}

bool os_is_dir_writable(const char *dir)
{
    bool ret = true;
    bstring canary = bformat("%s%stestwrite.tmp", dir, pathsep);
    bstring was_restrict_write_access = bstrcpy(restrict_write_access);
    bassigncstr(restrict_write_access, dir);
    sv_result result = sv_file_writefile(cstr(canary), "test", "wb");
    if (result.code)
    {
        sv_result_close(&result);
        ret = false;
    }
    else
    {
        os_tryuntil_remove(cstr(canary));
    }

    bassign(restrict_write_access, was_restrict_write_access);
    bdestroy(was_restrict_write_access);
    bdestroy(canary);
    return ret;
}

bool os_issubdirof(const char *s1, const char *s2)
{
    bstring s1slash = bformat("%s%s", s1, pathsep);
    bstring s2slash = bformat("%s%s", s2, pathsep);
    bool ret = s_startswithlen(cstr(s2slash), blength(s2slash), cstr(s1slash), blength(s1slash));
    bdestroy(s1slash);
    bdestroy(s2slash);
    return ret;
}

check_result os_listdirs_callback(void *context,
    const bstring filepath, uint64_t modtime, uint64_t filesize, unused(const bstring))
{
    bstrlist *list = (bstrlist *)context;
    if (os_recurse_is_dir(modtime, filesize))
    {
        bstrlist_append(list, filepath);
    }
    return OK;
}

check_result os_listfiles_callback(void *context,
    const bstring filepath, uint64_t modtime, uint64_t filesize, unused(const bstring))
{
    bstrlist *list = (bstrlist *)context;
    if (!os_recurse_is_dir(modtime, filesize))
    {
        bstrlist_append(list, filepath);
    }
    return OK;
}

check_result os_listfilesordirs(const char *dir, bstrlist *list, bool sorted, bool filesordirs)
{
    sv_result currenterr = {};
    bstrlist_clear(list);
    os_recurse_params params = { list, dir,
        filesordirs ? &os_listfiles_callback : &os_listdirs_callback, 0 /* max depth */,
        NULL };
    check(os_recurse(&params));

    if (sorted)
    {
        bstrlist_sort(list);
    }

cleanup:
    return currenterr;
}

check_result os_listfiles(const char *dir, bstrlist *list, bool sorted)
{
    sv_result currenterr = {};
    check_b(os_dir_exists(dir), "path=%s", dir);
    check(os_listfilesordirs(dir, list, sorted, true));
cleanup:
    return currenterr;
}

check_result os_listdirs(const char *dir, bstrlist *list, bool sorted)
{
    sv_result currenterr = {};
    check_b(os_dir_exists(dir), "path=%s", dir);
    check(os_listfilesordirs(dir, list, sorted, false));
cleanup:
    return currenterr;
}

check_result os_tryuntil_deletefiles(const char *dir, const char *filenamepattern)
{
    sv_result currenterr = {};
    bstring filenameonly = bstring_open();
    bstrlist *files_seen = bstrlist_open();
    check_b(os_isabspath(dir), "path=%s", dir);
    if (os_dir_exists(dir))
    {
        check(os_listfiles(dir, files_seen, false));
        for (int i = 0; i < files_seen->qty; i++)
        {
            os_get_filename(bstrlist_view(files_seen, i), filenameonly);
            if (fnmatch_simple(filenamepattern, cstr(filenameonly)))
            {
                sv_log_writefmt("del:%s matching %s", bstrlist_view(files_seen, i), filenamepattern);
                check_b(os_tryuntil_remove(bstrlist_view(files_seen, i)),
                    "failed to delete file %s", bstrlist_view(files_seen, i));
            }
        }
    }

cleanup:
    bdestroy(filenameonly);
    bstrlist_close(files_seen);
    return currenterr;
}

check_result os_tryuntil_movebypattern(const char *dir,
    const char *namepattern, const char *destdir, bool canoverwrite, int *moved)
{
    sv_result currenterr = {};
    bstring nameonly = bstring_open();
    bstring destpath = bstring_open();
    bstring movefailed = bstring_open();
    bstrlist *files_seen = bstrlist_open();
    check(os_listfiles(dir, files_seen, false));
    *moved = 0;
    for (int i = 0; i < files_seen->qty; i++)
    {
        os_get_filename(bstrlist_view(files_seen, i), nameonly);
        if (fnmatch_simple(namepattern, cstr(nameonly)))
        {
            bassignformat(destpath, "%s%s%s", destdir, pathsep, cstr(nameonly));
            sv_log_writefmt("movebypattern %s to %s", bstrlist_view(files_seen, i), cstr(destpath));
            if (os_tryuntil_move(bstrlist_view(files_seen, i), cstr(destpath), canoverwrite))
            {
                (*moved)++;
            }
            else
            {
                bformata(movefailed, "Move from %s to %s failed ",
                    bstrlist_view(files_seen, i), cstr(destpath));
            }
        }
    }

    check_b(blength(movefailed) == 0, "%s", cstr(movefailed));

cleanup:
    bdestroy(nameonly);
    bdestroy(destpath);
    bdestroy(movefailed);
    bstrlist_close(files_seen);
    return currenterr;
}

bstring restrict_write_access = NULL;
void confirm_writable(const char *s)
{
    check_fatal(s_startswith(s, cstr(restrict_write_access)),
        "Attempted to write to non-writable path '%s'. Please submit a bug report to the authors.", s);

    check_fatal(!s_contains(s, pathsep ".." pathsep),
        "Attempted to write to non-writable path with relative directory '%s'. "
        "Please submit a bug report to the authors.", s);
}

check_result os_findlastfilewithextension(const char *dir, const char *extension, bstring path)
{
    sv_result currenterr = {};
    bstrclear(path);
    bstrlist *list = bstrlist_open();
    check(os_listfiles(dir, list, true));
    for (int i = list->qty - 1; i >= 0; i--)
    {
        if (s_endswith(bstrlist_view(list, i), extension))
        {
            bassigncstr(path, bstrlist_view(list, i));
            break;
        }
    }
cleanup:
    bstrlist_close(list);
    return currenterr;
}

bstring os_make_subdir(const char *parent, const char *leaf)
{
    bstring ret = bstring_open();
    bassignformat(ret, "%s%s%s", parent, pathsep, leaf);
    if (os_create_dir(cstr(ret)))
    {
        return ret;
    }
    else
    {
        bstrclear(ret);
        return ret;
    }
}

check_result os_binarypath_impl(sv_pseudosplit *spl, const char *binname, bstring out)
{
    for (uint32_t i = 0; i < spl->splitpoints.length; i++)
    {
        const char *dir = sv_pseudosplit_viewat(spl, i);
        bassignformat(out, "%s%s%s", dir, pathsep, binname);
        if (os_file_exists(cstr(out)))
        {
            return OK;
        }
    }

    bstrclear(out);
    return OK;
}

/* Daniel Colascione, blogs.msdn.microsoft.com */
void argvquoteone(const char *arg, bstring result)
{
    bconchar(result, '"');
    const char* argend = arg + strlen(arg);
    for (const char* it = arg; ; it++)
    {
        int countBackslashes = 0;
        while (it != argend && *it == L'\\') {
            it++;
            countBackslashes++;
        }

        if (it == argend) {
            /* Escape all backslashes, but let the terminating
            double quotation mark we add below be interpreted
            as a metacharacter. */
            for (int j = 0; j < countBackslashes * 2; j++)
            {
                bconchar(result, '\\');
            }

            break;
        }
        else if (*it == L'"') {
            /* Escape all backslashes and the following
            double quotation mark. */
            for (int j = 0; j < countBackslashes * 2 + 1; j++)
            {
                bconchar(result, '\\');
            }

            bconchar(result, *it);
        }
        else {
            /* Backslashes aren't special here. */
            for (int j = 0; j < countBackslashes; j++)
            {
                bconchar(result, '\\');
            }

            bconchar(result, *it);
        }
    }
    bconchar(result, '"');
}

bool argvquote(const char *path,
    const char *const args[] /* NULL-terminated */, bstring result, bool fast)
{
    bassigncstr(result, "\"");
    bcatcstr(result, path);
    bconchar(result, '"');
    while (args && *args)
    {
        bconchar(result, ' ');
        if (fast)
        {
            bconchar(result, '"');
            bcatcstr(result, *args);
            bconchar(result, '"');

            /* just because a parameter is a valid windows file or directory doesn't
            mean it is safe, some methods accept c:\path\ as a directory, and it's been
            reported that low-level tricks can create a filepath containing double quote
            characters. */
            size_t arglen = strlen(*args);
            if ((*args)[arglen - 1] == '\\')
            {
                return false;
            }

            for (size_t i = 0; i < arglen; i++)
            {
                if ((*args)[i] == '"')
                {
                    return false;
                }
            }
        }
        else
        {
            argvquoteone(*args, result);
        }

        args++;
    }

    return true;
}