/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * filem/rsync: pre-position files with rsync. See filem_rsync.h for why
 * this exists alongside filem/raw; what follows is the flow.
 *
 * ----------------------------------------------------------------------
 * On the HNP, in preposition_files()
 * ----------------------------------------------------------------------
 *  1. Collect every --preload-files path (and the binary, if
 *     --preload-binary was given) from the job's app contexts.
 *  2. Hard-link them all into one staging tree under the session
 *     directory, with rsync --link-dest, which is what fixes the
 *     semantics: a *directory* contributes its contents to the root of
 *     the tree and a *file* contributes itself, so "--preload-files
 *     $(pwd)" reproduces the current directory rather than nesting it.
 *     Hard links mean this costs metadata, not bytes.
 *  3. Start "rsync --daemon" on an ephemeral port exporting that tree
 *     read-only as a single module.
 *  4. xcast where to pull from; wait for one ack per daemon.
 *
 * ----------------------------------------------------------------------
 * On each prted, in the xcast handler
 * ----------------------------------------------------------------------
 *  5. Pull the module into a per-job stash under this node's session
 *     directory, then ack. The HNP's addresses are tried in turn - the
 *     daemon has no way to know which of them is the one routable from
 *     here, and rsync --contimeout keeps a wrong guess cheap.
 *
 * ----------------------------------------------------------------------
 * On each prted, in link_local_files()
 * ----------------------------------------------------------------------
 *  6. Run the job's processes in that stash, so a bare "ls" shows exactly
 *     what was preloaded (filem_rsync_set_workdir). There is a comment
 *     down at set_working_dir() about why this is not done with the
 *     attribute that exists for it.
 *
 * Both rsync invocations block the caller. On the HNP that is a local
 * hard-link pass; on a prted it is the one thing that daemon has to do
 * before the job can start, and rsync's own --timeout/--contimeout bound
 * it, so every daemon acks - with a failure status if it must - rather
 * than leaving the launch hanging. That is the property worth having
 * here, and it is why this is not written as a non-blocking state
 * machine the way filem/raw's chunked transfers have to be.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

#include "filem_rsync.h"

/* the single module the HNP's rsync daemon exports. One module, not one
 * per preloaded path, because step 2 above has already merged everything
 * into one tree - which is what lets a prted do the whole pre-position
 * with a single rsync. */
#define PRTE_FILEM_RSYNC_MODULE "preload"

static int rsync_init(void);
static int rsync_finalize(void);
static int rsync_preposition_files(prte_job_t *jdata, prte_filem_completion_cbfunc_t cbfunc,
                                   void *cbdata);
static int rsync_link_local_files(prte_job_t *jdata, prte_app_context_t *app);

prte_filem_base_module_t prte_filem_rsync_module
    = {.filem_init = rsync_init,
       .filem_finalize = rsync_finalize,
       /* we don't use any of the following - the framework's put/get/rm
        * API is for explicit point-to-point moves, and nothing in PRRTE
        * calls it; preposition_files/link_local_files are the two entry
        * points that matter, same as filem/raw */
       .put = prte_filem_base_none_put,
       .put_nb = prte_filem_base_none_put_nb,
       .get = prte_filem_base_none_get,
       .get_nb = prte_filem_base_none_get_nb,
       .rm = prte_filem_base_none_rm,
       .rm_nb = prte_filem_base_none_rm_nb,
       .wait = prte_filem_base_none_wait,
       .wait_all = prte_filem_base_none_wait_all,
       /* now the APIs we *do* use */
       .preposition_files = rsync_preposition_files,
       .link_local_files = rsync_link_local_files};

/* ------------------------------------------------------------------ */
/* trackers                                                            */
/* ------------------------------------------------------------------ */

/* HNP side: one in-flight pre-position, waiting for its acks */
typedef struct {
    pmix_list_item_t super;
    char *nspace; /* the job being pre-positioned for */
    prte_filem_completion_cbfunc_t cbfunc;
    void *cbdata;
    int32_t nrecvd;
    int32_t status; /* first non-zero status any daemon reported */
    bool complete;
} prte_filem_rsync_outbound_t;

static void outbound_const(prte_filem_rsync_outbound_t *p)
{
    p->nspace = NULL;
    p->cbfunc = NULL;
    p->cbdata = NULL;
    p->nrecvd = 0;
    p->status = 0;
    p->complete = false;
}
static void outbound_dest(prte_filem_rsync_outbound_t *p)
{
    free(p->nspace);
}
static PMIX_CLASS_INSTANCE(prte_filem_rsync_outbound_t, pmix_list_item_t, outbound_const,
                           outbound_dest);

/* daemon side: where a job's pre-positioned tree landed on this node */
typedef struct {
    pmix_list_item_t super;
    char *nspace;
    char *stash; /* absolute path of the received tree */
} prte_filem_rsync_stash_t;

static void stash_const(prte_filem_rsync_stash_t *p)
{
    p->nspace = NULL;
    p->stash = NULL;
}
static void stash_dest(prte_filem_rsync_stash_t *p)
{
    free(p->nspace);
    free(p->stash);
}
static PMIX_CLASS_INSTANCE(prte_filem_rsync_stash_t, pmix_list_item_t, stash_const, stash_dest);

/* local state */
static pmix_list_t outbound;  /* HNP only */
static pmix_list_t stashes;   /* every daemon, HNP included */
static pid_t rsyncd_pid = 0;  /* HNP only: the exporting rsync daemon */
static int rsyncd_port = 0;
static char *rsyncd_stage = NULL; /* HNP only: the staged tree it exports */

static void recv_preposition(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                             prte_rml_tag_t tag, void *cbdata);
static void recv_ack(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                     prte_rml_tag_t tag, void *cbdata);

static int rsync_init(void)
{
    PMIX_CONSTRUCT(&stashes, pmix_list_t);

    /* catch the "here is where to pull from" xcast. Registered on the
     * HNP too - it is a daemon like any other and hosts local processes
     * that need the files just as much. */
    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FILEM_BASE, PRTE_RML_PERSISTENT,
                  recv_preposition, NULL);

    if (PRTE_PROC_IS_MASTER) {
        PMIX_CONSTRUCT(&outbound, pmix_list_t);
        PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_FILEM_BASE_RESP, PRTE_RML_PERSISTENT,
                      recv_ack, NULL);
    }

    return PRTE_SUCCESS;
}

static int rsync_finalize(void)
{
    if (0 < rsyncd_pid) {
        kill(rsyncd_pid, SIGTERM);
        while (0 > waitpid(rsyncd_pid, NULL, 0) && EINTR == errno) {
            continue;
        }
        rsyncd_pid = 0;
    }
    free(rsyncd_stage);
    rsyncd_stage = NULL;

    PMIX_LIST_DESTRUCT(&stashes);
    if (PRTE_PROC_IS_MASTER) {
        PMIX_LIST_DESTRUCT(&outbound);
    }

    /* the staging tree and every stash live under the session directory,
     * so PRRTE's own session cleanup removes them - nothing to unlink
     * here */
    return PRTE_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* running rsync                                                       */
/* ------------------------------------------------------------------ */

static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(sig, &act, (struct sigaction *) 0);
}

/* exec rsync in the child of a fork(); never returns */
static void rsync_child(char **argv)
{
    int fdin;
    sigset_t sigs;

    fdin = open("/dev/null", O_RDWR);
    if (0 <= fdin) {
        dup2(fdin, 0);
        close(fdin);
    }
    pmix_close_open_file_descriptors(-1);

    set_handler_default(SIGTERM);
    set_handler_default(SIGINT);
    set_handler_default(SIGHUP);
    set_handler_default(SIGPIPE);
    set_handler_default(SIGCHLD);
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);

    execv(prte_filem_rsync_path, argv);
    pmix_output(0, "filem:rsync: exec of %s failed with errno=%s(%d)\n", prte_filem_rsync_path,
                strerror(errno), errno);
    exit(127);
}

/* Run rsync to completion; returns its exit status, or -1 if it could
 * not be started or died on a signal. Blocking on purpose - see the
 * comment at the top of this file. */
static int run_rsync(char **argv)
{
    pid_t pid;
    int status = 0;

    if (0 < pmix_output_get_verbosity(prte_filem_base_framework.framework_output)) {
        char *joined = PMIx_Argv_join(argv, ' ');
        pmix_output(prte_filem_base_framework.framework_output, "%s filem:rsync: running: %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (NULL == joined) ? "?" : joined);
        free(joined);
    }

    pid = fork();
    if (pid < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        return -1;
    }
    if (0 == pid) {
        rsync_child(argv);
    }
    while (0 > waitpid(pid, &status, 0)) {
        if (EINTR != errno) {
            return -1;
        }
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* the argv every rsync invocation starts with. Deliberately *not* "-a":
 * that implies -o/-g, and preserving numeric owner across containers that
 * do not share a passwd database fails for no benefit. -rlptD is the rest
 * of it: recurse, keep symlinks, permissions, times and devices/specials. */
static char **base_rsync_argv(void)
{
    int argc = 0;
    char **argv = NULL;

    pmix_argv_append(&argc, &argv, prte_filem_rsync_path);
    pmix_argv_append(&argc, &argv, "-rlptD");
    return argv;
}

static void append_user_args(char ***argv)
{
    char **extra;
    int argc = PMIx_Argv_count(*argv);
    int i;

    if (NULL == prte_filem_rsync_args) {
        return;
    }
    extra = PMIx_Argv_split(prte_filem_rsync_args, ' ');
    for (i = 0; NULL != extra && NULL != extra[i]; i++) {
        pmix_argv_append(&argc, argv, extra[i]);
    }
    PMIx_Argv_free(extra);
}

/* ------------------------------------------------------------------ */
/* HNP: staging, the exporting daemon, and its address                 */
/* ------------------------------------------------------------------ */

/* Ask the OS for a port nothing is using, by binding and immediately
 * releasing it. There is an unavoidable window between that release and
 * rsync binding it; it is the same window every "pick a free port and
 * hand it to a child" does, and losing the race just fails the launch
 * with rsync's own "address already in use" rather than corrupting
 * anything. Set filem_rsync_port to take the choice into your own hands. */
static int pick_port(void)
{
    int sd, port = -1;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (0 == bind(sd, (struct sockaddr *) &addr, sizeof(addr))
        && 0 == getsockname(sd, (struct sockaddr *) &addr, &len)) {
        port = ntohs(addr.sin_port);
    }
    close(sd);
    return port;
}

/* The addresses a prted might reach us on, taken from the HNP URI - the
 * very contact information every daemon was launched knowing, so if any
 * address works for the OOB it is in this list. Loopback is appended for
 * the HNP's own pull, which never leaves the machine.
 *
 * The URI looks like "nspace.rank;tcp://10.0.0.1,192.168.1.5:35123:mask"
 * (and may carry a second, ';'-separated tcp6:// clause) - we want the
 * comma-separated address field of the tcp:// clause. */
static char **hnp_addresses(void)
{
    char **addrs = NULL;
    char *uri, *tcp, *end, *field;

    uri = prte_process_info.my_hnp_uri;
    if (NULL != uri && NULL != (tcp = strstr(uri, "tcp://"))) {
        tcp += strlen("tcp://");
        /* the address field runs to the ':' that starts the port list */
        end = strchr(tcp, ':');
        if (NULL != end) {
            field = strndup(tcp, (size_t) (end - tcp));
            if (NULL != field) {
                addrs = PMIx_Argv_split(field, ',');
                free(field);
            }
        }
    }
    PMIx_Argv_append_nosize(&addrs, "127.0.0.1");
    return addrs;
}

/* Hard-link every source into one tree, so the daemons need exactly one
 * transfer and the merge semantics are decided here rather than N times
 * on the far side:
 *
 *   a directory contributes its *contents* to the tree root, which is
 *   what makes "--preload-files $(pwd)" reproduce the current directory
 *   instead of nesting it one level deep;
 *
 *   a file contributes itself, by basename.
 *
 * --link-dest is what keeps this cheap: rsync hard-links anything
 * identical to the source instead of copying it (and silently falls back
 * to copying when the staging tree is on another filesystem). */
static int stage_sources(char **sources, const char *stage)
{
    int i, rc;
    struct stat st;

    if (PMIX_SUCCESS != pmix_os_dirpath_create(stage, S_IRWXU)) {
        pmix_show_help("help-filem-rsync.txt", "stage-dir-failed", true, stage, strerror(errno));
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }

    for (i = 0; NULL != sources[i]; i++) {
        char **argv;
        char *linkdest = NULL;
        char *src = NULL;
        bool isdir;

        if (0 != stat(sources[i], &st)) {
            pmix_show_help("help-filem-rsync.txt", "source-missing", true, sources[i],
                           strerror(errno));
            return PRTE_ERR_NOT_FOUND;
        }
        isdir = S_ISDIR(st.st_mode);

        if (isdir) {
            /* trailing slash: copy the contents, not the directory */
            pmix_asprintf(&src, "%s/", sources[i]);
            linkdest = strdup(sources[i]);
        } else {
            src = strdup(sources[i]);
            linkdest = pmix_dirname(sources[i]);
        }
        if (NULL == src || NULL == linkdest) {
            free(src);
            free(linkdest);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        argv = base_rsync_argv();
        {
            int argc = PMIx_Argv_count(argv);
            char *ld;
            pmix_asprintf(&ld, "--link-dest=%s", linkdest);
            if (NULL != ld) {
                pmix_argv_append(&argc, &argv, ld);
                free(ld);
            }
            pmix_argv_append(&argc, &argv, src);
            pmix_argv_append(&argc, &argv, stage);
        }
        free(src);
        free(linkdest);

        rc = run_rsync(argv);
        PMIx_Argv_free(argv);
        if (0 != rc) {
            pmix_show_help("help-filem-rsync.txt", "stage-failed", true, sources[i], stage, rc);
            return PRTE_ERROR;
        }
    }

    return PRTE_SUCCESS;
}

/* Write the daemon config and start "rsync --daemon" on `port`, exporting
 * `stage` read-only as one module. Returns PRTE_SUCCESS with rsyncd_pid
 * set. */
static int start_rsyncd(const char *stage, int port)
{
    char *dir = NULL, *conf = NULL;
    FILE *fp;
    char **argv;
    int argc, rc = PRTE_ERROR;
    char *portstr = NULL, *confarg = NULL;
    pid_t pid;

    pmix_asprintf(&dir, "%s/filem-rsync-daemon", prte_process_info.top_session_dir);
    if (NULL == dir) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (PMIX_SUCCESS != pmix_os_dirpath_create(dir, S_IRWXU)) {
        pmix_show_help("help-filem-rsync.txt", "stage-dir-failed", true, dir, strerror(errno));
        free(dir);
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }
    pmix_asprintf(&conf, "%s/rsyncd.conf", dir);
    if (NULL == conf) {
        free(dir);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    fp = fopen(conf, "w");
    if (NULL == fp) {
        pmix_show_help("help-filem-rsync.txt", "stage-dir-failed", true, conf, strerror(errno));
        free(dir);
        free(conf);
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }
    /* "use chroot = no" so the daemon does not need to be root; the
     * numeric uid/gid keep it reading as us rather than dropping to
     * "nobody", which is rsync's default when it *is* root and would
     * make the staged tree unreadable. */
    fprintf(fp, "use chroot = no\n");
    fprintf(fp, "uid = %u\n", (unsigned) geteuid());
    fprintf(fp, "gid = %u\n", (unsigned) getegid());
    fprintf(fp, "reverse lookup = no\n");
    fprintf(fp, "max verbosity = 1\n");
    fprintf(fp, "pid file = %s/rsyncd.pid\n", dir);
    fprintf(fp, "lock file = %s/rsyncd.lock\n", dir);
    fprintf(fp, "log file = %s/rsyncd.log\n", dir);
    fprintf(fp, "\n[%s]\n", PRTE_FILEM_RSYNC_MODULE);
    fprintf(fp, "path = %s\n", stage);
    fprintf(fp, "read only = yes\n");
    fprintf(fp, "list = no\n");
    fclose(fp);

    argv = NULL;
    argc = 0;
    pmix_argv_append(&argc, &argv, prte_filem_rsync_path);
    pmix_argv_append(&argc, &argv, "--daemon");
    pmix_argv_append(&argc, &argv, "--no-detach");
    pmix_asprintf(&confarg, "--config=%s", conf);
    pmix_asprintf(&portstr, "--port=%d", port);
    if (NULL == confarg || NULL == portstr) {
        goto done;
    }
    pmix_argv_append(&argc, &argv, confarg);
    pmix_argv_append(&argc, &argv, portstr);

    pid = fork();
    if (pid < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        goto done;
    }
    if (0 == pid) {
        rsync_child(argv);
    }
    rsyncd_pid = pid;

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:rsync: exporting %s as rsync://:%d/%s (pid %ld)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), stage, port,
                         PRTE_FILEM_RSYNC_MODULE, (long) pid));
    rc = PRTE_SUCCESS;

done:
    PMIx_Argv_free(argv);
    free(confarg);
    free(portstr);
    free(dir);
    free(conf);
    return rc;
}

/* Block until the daemon we just forked is accepting connections, so the
 * first prted to try does not lose a race with rsync's startup. Gives up
 * after ~5s and lets the transfer report the real error. */
static void await_rsyncd(int port)
{
    struct sockaddr_in addr;
    int i;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (i = 0; i < 500; i++) {
        int sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd < 0) {
            return;
        }
        if (0 == connect(sd, (struct sockaddr *) &addr, sizeof(addr))) {
            close(sd);
            return;
        }
        close(sd);
        usleep(10000);
    }
}

/* ------------------------------------------------------------------ */
/* HNP: preposition_files                                              */
/* ------------------------------------------------------------------ */

/* Absolute form of a --preload-files entry, resolved against our own cwd
 * the way the user typed it would be. Caller frees. */
static char *absolutize(const char *path)
{
    char cwd[PRTE_PATH_MAX];
    char *abs;

    if (pmix_path_is_absolute(path)) {
        return strdup(path);
    }
    if (NULL == getcwd(cwd, sizeof(cwd))) {
        return strdup(path);
    }
    pmix_asprintf(&abs, "%s/%s", cwd, path);
    return abs;
}

static int rsync_preposition_files(prte_job_t *jdata, prte_filem_completion_cbfunc_t cbfunc,
                                   void *cbdata)
{
    prte_app_context_t *app;
    char **sources = NULL, **files = NULL, **addrs = NULL;
    char *filestring = NULL, *stage = NULL, *addrlist = NULL;
    prte_filem_rsync_outbound_t *ob;
    pmix_data_buffer_t xcast;
    prte_grpcomm_signature_t *sig;
    uint16_t port16;
    char *modname = PRTE_FILEM_RSYNC_MODULE;
    char *nspace;
    int i, j, rc;

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:rsync: preposition files for job %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));

    /* collect what every app wants pre-positioned */
    for (i = 0; i < jdata->apps->size; i++) {
        if (NULL == (app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, NULL, PMIX_BOOL)) {
            char *abs = absolutize(app->app);
            char *bname;
            if (NULL == abs) {
                rc = PRTE_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            PMIx_Argv_append_nosize(&sources, abs);
            free(abs);
            /* the binary lands in the working directory alongside the
             * files, so the app has to be named relative to it */
            bname = pmix_basename(app->app);
            free(app->app);
            pmix_asprintf(&app->app, "./%s", bname);
            free(bname);
            free(app->argv[0]);
            app->argv[0] = strdup(app->app);
            /* what makes "./" resolve is the working-directory override in
             * link_local_files() - see the comment there for why it is done
             * that way and not with PRTE_APP_SSNDIR_CWD */
        }
        if (prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES, (void **) &filestring,
                               PMIX_STRING)) {
            files = PMIx_Argv_split(filestring, ',');
            free(filestring);
            filestring = NULL;
            for (j = 0; NULL != files && NULL != files[j]; j++) {
                char *abs = absolutize(files[j]);
                if (NULL == abs) {
                    PMIx_Argv_free(files);
                    rc = PRTE_ERR_OUT_OF_RESOURCE;
                    goto error;
                }
                PMIx_Argv_append_nosize(&sources, abs);
                free(abs);
            }
            PMIx_Argv_free(files);
            files = NULL;
        }
    }

    if (NULL == sources) {
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:rsync: nothing to preposition",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        if (NULL != cbfunc) {
            cbfunc(PRTE_SUCCESS, cbdata);
        }
        return PRTE_SUCCESS;
    }

    /* stage everything into one tree and export it */
    pmix_asprintf(&stage, "%s/filem-rsync-src/%s", prte_process_info.top_session_dir,
                  jdata->nspace);
    if (NULL == stage) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto error;
    }
    rc = stage_sources(sources, stage);
    if (PRTE_SUCCESS != rc) {
        goto error;
    }

    if (0 == rsyncd_pid) {
        rsyncd_port = (0 < prte_filem_rsync_port) ? prte_filem_rsync_port : pick_port();
        if (rsyncd_port <= 0) {
            rc = PRTE_ERR_SOCKET_NOT_AVAILABLE;
            goto error;
        }
        /* the exported root is the *parent* of the per-job staging dirs,
         * so one daemon serves every job this DVM ever pre-positions for;
         * each pull names its job's subdirectory within the module */
        pmix_asprintf(&rsyncd_stage, "%s/filem-rsync-src", prte_process_info.top_session_dir);
        if (NULL == rsyncd_stage) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto error;
        }
        rc = start_rsyncd(rsyncd_stage, rsyncd_port);
        if (PRTE_SUCCESS != rc) {
            goto error;
        }
        await_rsyncd(rsyncd_port);
    }

    addrs = hnp_addresses();
    addrlist = PMIx_Argv_join(addrs, ',');
    PMIx_Argv_free(addrs);
    if (NULL == addrlist) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto error;
    }

    /* track the acks before sending, so a fast daemon cannot answer
     * before there is anything to count its answer against */
    ob = PMIX_NEW(prte_filem_rsync_outbound_t);
    ob->nspace = strdup(jdata->nspace);
    ob->cbfunc = cbfunc;
    ob->cbdata = cbdata;
    pmix_list_append(&outbound, &ob->super);

    PMIX_DATA_BUFFER_CONSTRUCT(&xcast);
    nspace = jdata->nspace;
    port16 = (uint16_t) rsyncd_port;
    rc = PMIx_Data_pack(NULL, &xcast, &nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &xcast, &addrlist, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &xcast, &port16, 1, PMIX_UINT16);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &xcast, &modname, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_DESTRUCT(&xcast);
        pmix_list_remove_item(&outbound, &ob->super);
        PMIX_RELEASE(ob);
        rc = PRTE_ERROR;
        goto error;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:rsync: telling %d daemon(s) to pull %s from [%s]:%d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) prte_process_info.num_daemons,
                         jdata->nspace, addrlist, rsyncd_port));

    sig = PMIX_NEW(prte_grpcomm_signature_t);
    sig->signature = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    sig->sz = 1;
    PMIX_LOAD_PROCID(&sig->signature[0], PRTE_PROC_MY_NAME->nspace, PMIX_RANK_WILDCARD);
    rc = prte_grpcomm.xcast(sig, PRTE_RML_TAG_FILEM_BASE, &xcast);
    PMIX_RELEASE(sig);
    PMIX_DATA_BUFFER_DESTRUCT(&xcast);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        pmix_list_remove_item(&outbound, &ob->super);
        PMIX_RELEASE(ob);
        goto error;
    }

    PMIx_Argv_free(sources);
    free(stage);
    free(addrlist);
    return PRTE_SUCCESS;

error:
    PMIx_Argv_free(sources);
    free(stage);
    free(addrlist);
    return rc;
}

/* ------------------------------------------------------------------ */
/* HNP: collecting the acks                                            */
/* ------------------------------------------------------------------ */

static void recv_ack(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                     prte_rml_tag_t tag, void *cbdata)
{
    prte_filem_rsync_outbound_t *ob;
    pmix_list_item_t *item;
    char *nspace = NULL;
    int32_t st = 0;
    int n, rc;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &n, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(nspace);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:rsync: ack from %s for job %s status %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender), nspace,
                         (int) st));

    for (item = pmix_list_get_first(&outbound); item != pmix_list_get_end(&outbound);
         item = pmix_list_get_next(item)) {
        ob = (prte_filem_rsync_outbound_t *) item;
        if (0 != strcmp(ob->nspace, nspace)) {
            continue;
        }
        if (0 != st && 0 == ob->status) {
            ob->status = st;
        }
        ob->nrecvd++;
        if (!ob->complete && ob->nrecvd >= (int32_t) prte_process_info.num_daemons) {
            /* latch it: a daemon that acks twice (or one that joins late)
             * must not fire the launch callback a second time */
            ob->complete = true;
            PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                                 "%s filem:rsync: preposition complete for %s status %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nspace, (int) ob->status));
            if (NULL != ob->cbfunc) {
                ob->cbfunc((0 == ob->status) ? PRTE_SUCCESS : PRTE_ERROR, ob->cbdata);
            }
        }
        break;
    }
    free(nspace);
}

/* ------------------------------------------------------------------ */
/* daemon: pulling the tree                                            */
/* ------------------------------------------------------------------ */

static void send_ack(const char *nspace, int32_t status)
{
    pmix_data_buffer_t *buf;
    char *ns = (char *) nspace;
    int rc;

    PMIX_DATA_BUFFER_CREATE(buf);
    rc = PMIx_Data_pack(NULL, buf, &ns, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, &status, 1, PMIX_INT32);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, buf, PRTE_RML_TAG_FILEM_BASE_RESP);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
    }
}

static void recv_preposition(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                             prte_rml_tag_t tag, void *cbdata)
{
    char *nspace = NULL, *addrlist = NULL, *modname = NULL, *stash = NULL;
    char **addrs = NULL;
    uint16_t port = 0;
    prte_filem_rsync_stash_t *st;
    int n, rc, i;
    int32_t result = PRTE_ERROR;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nspace, &n, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    n = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &addrlist, &n, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &port, &n, PMIX_UINT16);
    }
    if (PMIX_SUCCESS == rc) {
        n = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &modname, &n, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    pmix_asprintf(&stash, "%s/filem-rsync/%s", prte_process_info.top_session_dir, nspace);
    if (NULL == stash) {
        goto done;
    }
    if (PMIX_SUCCESS != pmix_os_dirpath_create(stash, S_IRWXU)) {
        pmix_show_help("help-filem-rsync.txt", "stage-dir-failed", true, stash, strerror(errno));
        goto done;
    }

    /* Try the HNP's addresses in turn. Nothing here knows which of them
     * is routable from this node - that is exactly what the OOB worked
     * out for itself when this daemon phoned home - so we find out the
     * only way available, cheaply, with a short connect timeout. */
    addrs = PMIx_Argv_split(addrlist, ',');
    for (i = 0; NULL != addrs && NULL != addrs[i]; i++) {
        char **argv = base_rsync_argv();
        int argc = PMIx_Argv_count(argv);
        char *url = NULL, *tmo = NULL;

        pmix_argv_append(&argc, &argv, "--compress");
        pmix_argv_append(&argc, &argv, "--contimeout=10");
        pmix_asprintf(&tmo, "--timeout=%d", prte_filem_rsync_timeout);
        if (NULL != tmo) {
            pmix_argv_append(&argc, &argv, tmo);
            free(tmo);
        }
        append_user_args(&argv);
        argc = PMIx_Argv_count(argv);
        /* the module's root holds one subdirectory per job; the trailing
         * slash pulls that subdirectory's *contents* into the stash */
        pmix_asprintf(&url, "rsync://%s:%u/%s/%s/", addrs[i], (unsigned) port, modname, nspace);
        if (NULL == url) {
            PMIx_Argv_free(argv);
            continue;
        }
        pmix_argv_append(&argc, &argv, url);
        pmix_argv_append(&argc, &argv, stash);
        free(url);

        rc = run_rsync(argv);
        PMIx_Argv_free(argv);
        if (0 == rc) {
            result = PRTE_SUCCESS;
            break;
        }
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:rsync: pull from %s failed (rsync exit %d)%s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), addrs[i], rc,
                             (NULL != addrs[i + 1]) ? " - trying the next address" : ""));
    }

    if (PRTE_SUCCESS == result) {
        /* remember where it landed for link_local_files() */
        st = PMIX_NEW(prte_filem_rsync_stash_t);
        st->nspace = strdup(nspace);
        st->stash = strdup(stash);
        pmix_list_append(&stashes, &st->super);
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:rsync: pre-positioned %s into %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nspace, stash));
    } else {
        pmix_show_help("help-filem-rsync.txt", "pull-failed", true,
                       prte_process_info.nodename, nspace, addrlist, (unsigned) port);
    }

done:
    send_ack(nspace, (int32_t) result);
    PMIx_Argv_free(addrs);
    free(nspace);
    free(addrlist);
    free(modname);
    free(stash);
}

/* ------------------------------------------------------------------ */
/* daemon: pointing the job's processes at the tree                    */
/* ------------------------------------------------------------------ */

static const char *find_stash(const char *nspace)
{
    pmix_list_item_t *item;
    prte_filem_rsync_stash_t *st;

    for (item = pmix_list_get_first(&stashes); item != pmix_list_get_end(&stashes);
         item = pmix_list_get_next(item)) {
        st = (prte_filem_rsync_stash_t *) item;
        if (0 == strcmp(st->nspace, nspace)) {
            return st->stash;
        }
    }
    return NULL;
}

/* Point this app's processes at the directory the pre-positioned files
 * landed in - the stash itself, which is already per-job (keyed by
 * nspace) and per-node, and lives under the session directory PRRTE
 * cleans up on the way out. Running *in* it rather than linking it
 * somewhere else is what makes a bare "ls" print exactly what was
 * preloaded and nothing else; the obvious alternative, the job session
 * directory, has PRRTE's own per-rank subdirectories in it.
 *
 * The obvious way to set a working directory here is PRTE_APP_SSNDIR_CWD,
 * which is what filem/raw sets for --preload-binary and what odls's
 * setup_path() reads. It cannot be used: setup_path() resolves that flag
 * by dereferencing app->job, and app->job is only ever assigned on the HNP
 * (in pmix_server_dyn.c and ess/hnp) - an app context unpacked from the
 * launch message on a remote daemon has it NULL. Setting the flag
 * therefore segfaults every non-HNP prted, which is reproducible in stock
 * PRRTE 3.0.13 with "mpirun --host <remote> --preload-binary ..." and
 * nothing from this repository involved.
 *
 * So we set it the other way round. odls calls link_local_files() at a
 * useful moment: after setup_path() has filled in app->cwd, but before
 * pmix_util_check_context_app() resolves the executable against it and
 * well before the child is dispatched with "cd->wdir = strdup(app->cwd)".
 *
 * The chdir() is not redundant with assigning app->cwd:
 * pmix_util_check_context_app() resolves a *relative* executable -
 * "./script.sh", or the "./<binary>" that --preload-binary rewrites the
 * command to - with a plain access(), against the daemon's own working
 * directory rather than against the app->cwd it is handed. setup_path()
 * chdir()s for exactly the same reason, and odls returns to its base
 * directory a few lines later, so this is a borrowed cwd, not a leaked
 * one. PWD is updated to match, again as setup_path() does: getcwd() and
 * $PWD disagreeing surprises people. */
static int set_working_dir(prte_app_context_t *app, const char *dir)
{
    if (0 != chdir(dir)) {
        pmix_show_help("help-filem-rsync.txt", "workdir-failed", true, dir, strerror(errno));
        return PRTE_ERR_FILE_OPEN_FAILURE;
    }
    free(app->cwd);
    app->cwd = strdup(dir);
    PMIX_SETENV_COMPAT("PWD", dir, true, &app->env);
    return PRTE_SUCCESS;
}

static int rsync_link_local_files(prte_job_t *jdata, prte_app_context_t *app)
{
    const char *stash;
    char *filestring = NULL;
    bool wants_files, wants_bin;

    /* only jobs that actually asked for something */
    wants_files = prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_FILES,
                                     (void **) &filestring, PMIX_STRING);
    free(filestring);
    wants_bin = prte_get_attribute(&app->attributes, PRTE_APP_PRELOAD_BIN, NULL, PMIX_BOOL);
    if (!wants_files && !wants_bin) {
        return PRTE_SUCCESS;
    }

    stash = find_stash(jdata->nspace);
    if (NULL == stash) {
        /* the pre-position either failed or never happened; the launch has
         * already been told about that, so do not fail the app here too */
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "%s filem:rsync: no pre-positioned tree for %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_JOBID_PRINT(jdata->nspace)));
        return PRTE_SUCCESS;
    }

    /* --preload-binary rewrote the executable to "./<name>", so its
     * processes have to run in the stash whatever the user asked for; for
     * plain --preload-files it is what makes a bare "ls" show the files,
     * and filem_rsync_set_workdir turns it off for anyone who wants their
     * own working directory back. */
    if (!wants_bin && !prte_filem_rsync_set_workdir) {
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                         "%s filem:rsync: running %s in %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_JOBID_PRINT(jdata->nspace), stash));
    return set_working_dir(app, stash);
}
