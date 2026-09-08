/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * plm/k8s launch flow.
 *
 * The shape is plm/slurm's rather than plm/ssh's, and for the same
 * reason: this component starts every daemon with *one* child process -
 * a single "kubectl apply -f" of a single rendered manifest - the way
 * plm/slurm starts them all with a single "srun". So there is no
 * throttled launch list and no per-daemon fork here; there is one dummy
 * prte_proc_t tracking that one child through PRRTE's SIGCHLD machinery,
 * exactly as plm_slurm_start_proc() does.
 *
 * What the manifest contains is entirely the template's business. This
 * module's job is to hand the template every node the map says needs a
 * daemon - name, vpid, slots, and that daemon's env - render once, and
 * apply the result. See plm_k8s_template.h for the tag vocabulary and
 * templates/ for the two shipped shapes (one Indexed Job covering all
 * nodes; a "kind: List" of nodeName-pinned Jobs).
 *
 * The daemons' command lines collapse to a bare `prted` plus an env:
 * block, because PRTE's MCA variable system treats "--prtemca X Y" and
 * "PRTE_MCA_X=Y" as interchangeable - see prte_plm_k8s_argv_to_envars().
 *
 * There is no tree-spawn fan-out (remote_spawn stays NULL), same as
 * every non-ssh plm component.
 *
 * Shutdown cleanup - deleting what this DVM applied - is deliberately
 * not an MCA-configurable toggle. It always runs, via one label-selector
 * "kubectl delete" rather than per-object bookkeeping, which is also why
 * it works unchanged whether the template produced one object or twenty.
 * See PRTE_PLM_K8S_CLEANUP_SELECTOR below; it must keep matching the
 * labels the template stamps.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <fcntl.h>
#include <signal.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/event/event-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_fd.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/rmaps/rmaps.h"
#include "src/mca/state/state.h"
#include "src/rml/rml.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "plm_k8s.h"
#include "plm_k8s_template.h"

/* The kinds shutdown cleanup deletes, and the label selector it deletes
 * by. Both must keep matching what the templates stamp: the selector is
 * on every object either shipped template creates, and the kind list
 * covers both shapes (the single Indexed Job, and the per-node Jobs a
 * "kind: List" template expands to). "kubectl delete <kinds> -l ..."
 * silently finds nothing for a kind a template never created, so listing
 * a kind costs nothing but one API call.
 *
 * If you template something else - a JobSet, a LeaderWorkerSet - add its
 * kind here, or cleanup will leave it behind. Cleanup is not a toggle:
 * it always runs. */
#define PRTE_PLM_K8S_CLEANUP_KINDS "job,pod"
#define PRTE_PLM_K8S_CLEANUP_SELECTOR \
    "prrte.kubepmix.dev/managed-by=plm-k8s,prrte.kubepmix.dev/dvm=%s"

static int k8s_init(void);
static int k8s_launch(prte_job_t *jdata);
static int k8s_terminate_prteds(void);
static int k8s_finalize(void);

prte_plm_base_module_t prte_plm_k8s_module = {
    .init = k8s_init,
    .set_hnp_name = prte_plm_base_set_hnp_name,
    .spawn = k8s_launch,
    .remote_spawn = NULL, /* no tree-spawn - the HNP applies the manifest */
    .terminate_job = prte_plm_base_prted_terminate_job,
    .terminate_orteds = k8s_terminate_prteds,
    .terminate_procs = prte_plm_base_prted_kill_local_procs,
    .signal_job = prte_plm_base_prted_signal_local_procs,
    .finalize = k8s_finalize};

/* what the "kubectl apply" child's wait callback needs to report on, and
 * to clean up. Owned by that callback. */
typedef struct {
    pmix_list_item_t super;
    char *manifest_path; /* the rendered manifest on disk, owned */
    char *object_name;   /* "{{name}}", owned, for log/diagnostic text */
    char *nodes;         /* comma-joined node list, owned, ditto */
    prte_job_t *jdata;   /* retained - whose launch this was */
} prte_plm_k8s_caddy_t;

static void caddy_const(prte_plm_k8s_caddy_t *ptr)
{
    ptr->manifest_path = NULL;
    ptr->object_name = NULL;
    ptr->nodes = NULL;
    ptr->jdata = NULL;
}
static void caddy_dest(prte_plm_k8s_caddy_t *ptr)
{
    free(ptr->manifest_path);
    free(ptr->object_name);
    free(ptr->nodes);
    if (NULL != ptr->jdata) {
        PMIX_RELEASE(ptr->jdata);
    }
}
PMIX_CLASS_INSTANCE(prte_plm_k8s_caddy_t, pmix_list_item_t, caddy_const, caddy_dest);

/*
 * Local functions
 */
static void set_handler_default(int sig);
static void k8s_child(char **argv, bool quiet) __prte_attribute_noreturn__;
static void launch_daemons(int fd, short args, void *cbdata);
static void k8s_wait_apply(int sd, short flags, void *cbdata);
static char **build_kubectl_argv(const char *verb);
static int write_manifest_to_tmpfile(const char *manifest, char **path_out);
static char *get_base_name(void);
static void delete_applied_objects(void);
static int apply_manifest(prte_plm_k8s_caddy_t *caddy);

/**
 * Init the module
 */
static int k8s_init(void)
{
    int rc;

    if (PRTE_SUCCESS
        != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS, launch_daemons))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
    }

    /* Whether the daemon-to-node mapping is settled at launch time is a
     * property of the *template*, not of Kubernetes. Both shipped
     * templates settle it - one by pinning with nodeName, the other by
     * having each pod look its vpid up from the node it landed on - so
     * this defaults to true. See plm_k8s_component.c for what a template
     * that does neither costs you. */
    prte_plm_globals.daemon_nodes_assigned_at_launch
        = prte_mca_plm_k8s_component.assign_nodes;

    return rc;
}

/*
 * Launch a daemon (bootproxy) for each node needing one. The daemon will
 * be responsible for launching the application.
 */
static int k8s_launch(prte_job_t *jdata)
{
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    } else {
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_INIT);
    }
    return PRTE_SUCCESS;
}

/* the base name shared by every object of this DVM - the source for the
 * template's "{{name}}" and for the shutdown-cleanup label selector.
 * Kept short (50 bytes) because Kubernetes stamps a "job-name" *label*
 * with the full object name on each pod, and label values are capped at
 * 63 bytes - 50 bytes of base leaves room for the suffixes a Job adds to
 * its pods. Caller frees. */
static char *get_base_name(void)
{
    const char *raw = (NULL != prte_mca_plm_k8s_component.name_prefix)
                          ? prte_mca_plm_k8s_component.name_prefix
                          : prte_process_info.myproc.nspace;
    return prte_plm_k8s_sanitize_name(raw, 50);
}

static void free_tnodes(prte_plm_k8s_tnode_t *nodes, int n)
{
    int i;

    if (NULL == nodes) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(nodes[i].node);
        free(nodes[i].rank);
        PMIx_Argv_free(nodes[i].envars);
    }
    free(nodes);
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prte_job_t *daemons;
    prte_job_map_t *map = NULL;
    prte_node_t *node;
    int32_t nnode;
    int argc, proc_vpid_index = -1, rc;
    char **argv = NULL;
    char *rank_str = NULL;
    char *manifest = NULL;
    char *template_text = NULL;
    char *base_name = NULL;
    char **node_names = NULL;
    prte_plm_k8s_tnode_t *tnodes = NULL;
    int num_tnodes = 0;
    prte_plm_k8s_tctx_t tctx;
    prte_plm_k8s_caddy_t *caddy = NULL;
    prte_state_caddy_t *state = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(state);

    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
    if (PRTE_SUCCESS != (rc = prte_plm_base_setup_virtual_machine(state->jdata))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if we don't want to launch, then don't - the user just wants the
     * proposed process map */
    if (prte_get_attribute(&daemons->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
        return;
    }

    if (NULL == (map = daemons->map)) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (0 == map->num_new_daemons) {
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PMIX_RELEASE(state);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:k8s: launching %d daemon(s) with one \"kubectl apply\"",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), map->num_new_daemons));

    /* load the manifest template - the configured file (default
     * $HOME/.plm-k8s-template) wins if present; a missing file quietly
     * falls back to the built-in single-Indexed-Job default */
    template_text = prte_plm_k8s_load_template(prte_mca_plm_k8s_component.template_path, &rc);
    if (NULL == template_text) {
        if (PRTE_ERR_NOT_FOUND != rc) {
            PRTE_ERROR_LOG(rc);
            goto cleanup;
        }
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:k8s: no template at \"%s\" - using the built-in default",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             prte_mca_plm_k8s_component.template_path));
        template_text = strdup(prte_plm_k8s_default_template);
        if (NULL == template_text) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto cleanup;
        }
    }

    base_name = get_base_name();
    if (NULL == base_name) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    /* build the template prted command line once - identical for every
     * node except for the vpid substituted into proc_vpid_index, exactly
     * as ssh does with its own argv template */
    argc = 0;
    prte_plm_base_prted_append_basic_args(&argc, &argv, "env", &proc_vpid_index);
    pmix_argv_append(&argc, &argv, "--prtemca");
    pmix_argv_append(&argc, &argv, "plm");
    pmix_argv_append(&argc, &argv, "k8s");

    tnodes = (prte_plm_k8s_tnode_t *) calloc((size_t) map->num_new_daemons, sizeof(*tnodes));
    if (NULL == tnodes) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    for (nnode = 0; nnode < map->nodes->size; nnode++) {
        if (NULL == (node = (prte_node_t *) pmix_pointer_array_get_item(map->nodes, nnode))) {
            continue;
        }

        if (NULL == node->daemon) {
            PRTE_ERROR_LOG(PRTE_ERR_FATAL);
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:k8s: daemon not defined on node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), node->name));
            continue;
        }

        if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            continue;
        }

        if (num_tnodes >= map->num_new_daemons) {
            /* the map disagrees with its own num_new_daemons - refuse to
             * write past the array rather than trust the count */
            PRTE_ERROR_LOG(PRTE_ERR_FATAL);
            rc = PRTE_ERR_FATAL;
            goto cleanup;
        }

        free(argv[proc_vpid_index]);
        rc = prte_util_convert_vpid_to_string(&rank_str, node->daemon->name.rank);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            argv[proc_vpid_index] = strdup("");
            goto cleanup;
        }
        argv[proc_vpid_index] = rank_str;
        rank_str = NULL;

        /* the whole point of "{{node}}": tell the manifest which node the
         * allocation already picked for this daemon. A template that pins
         * (nodeName) makes that binding real; the default one uses it to
         * constrain scheduling to the allocated set. */
        tnodes[num_tnodes].node = strdup((NULL != node->rawname) ? node->rawname : node->name);
        tnodes[num_tnodes].rank = strdup(argv[proc_vpid_index]);
        tnodes[num_tnodes].slots = (int) node->slots;
        tnodes[num_tnodes].envars = prte_plm_k8s_argv_to_envars(argv, NULL);
        if (NULL == tnodes[num_tnodes].node || NULL == tnodes[num_tnodes].rank) {
            rc = PRTE_ERR_OUT_OF_RESOURCE;
            goto cleanup;
        }
        PMIx_Argv_append_nosize(&node_names, tnodes[num_tnodes].node);
        num_tnodes++;
    }

    if (0 == num_tnodes) {
        /* nothing actually needed launching after all */
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        goto done;
    }

    /* render once, with every node in scope. The common env block is the
     * same argv minus ess_base_vpid - the one value that differs per
     * daemon, and so the one a single shared pod template cannot carry;
     * see templates/single-job.yaml.tmpl for how it recovers it. */
    memset(&tctx, 0, sizeof(tctx));
    tctx.name = base_name;
    tctx.nodes = tnodes;
    tctx.num_nodes = num_tnodes;
    tctx.vpid_start = (int) map->daemon_vpid_start;
    tctx.common_envars = prte_plm_k8s_argv_to_envars(argv, "ess_base_vpid");

    manifest = prte_plm_k8s_render(template_text, &tctx);
    PMIx_Argv_free(tctx.common_envars);
    if (NULL == manifest) {
        rc = PRTE_ERR_OUT_OF_RESOURCE;
        goto cleanup;
    }

    caddy = PMIX_NEW(prte_plm_k8s_caddy_t);
    rc = write_manifest_to_tmpfile(manifest, &caddy->manifest_path);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }
    caddy->object_name = strdup(base_name);
    caddy->nodes = PMIx_Argv_join(node_names, ',');
    caddy->jdata = state->jdata;
    PMIX_RETAIN(caddy->jdata);

    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:k8s: applying %s for %d daemon(s) on [%s] from manifest %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), caddy->object_name, num_tnodes,
                         caddy->nodes, caddy->manifest_path));

    rc = apply_manifest(caddy);
    if (PRTE_SUCCESS != rc) {
        goto cleanup;
    }
    /* the wait callback owns the caddy from here */
    caddy = NULL;

    /* indicate that the daemons for this job were launched - the daemon
     * callback machinery decides when they are actually up */
    state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
    daemons->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;

done:
    PMIx_Argv_free(argv);
    PMIx_Argv_free(node_names);
    free_tnodes(tnodes, num_tnodes);
    free(base_name);
    free(template_text);
    free(manifest);
    PMIX_RELEASE(state);
    return;

cleanup:
    PMIx_Argv_free(argv);
    PMIx_Argv_free(node_names);
    free_tnodes(tnodes, num_tnodes);
    free(base_name);
    free(template_text);
    free(manifest);
    if (NULL != caddy) {
        if (NULL != caddy->manifest_path) {
            unlink(caddy->manifest_path);
        }
        PMIX_RELEASE(caddy);
    }
    PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_FAILED_TO_START);
    PMIX_RELEASE(state);
}

static void append_kubectl_args(int *argc, char ***argv)
{
    char **extra;
    int j;

    if (NULL == prte_mca_plm_k8s_component.kubectl_args) {
        return;
    }
    extra = PMIx_Argv_split(prte_mca_plm_k8s_component.kubectl_args, ' ');
    for (j = 0; NULL != extra && NULL != extra[j]; j++) {
        pmix_argv_append(argc, argv, extra[j]);
    }
    PMIx_Argv_free(extra);
}

/* "kubectl <verb> <user args...>" - the caller appends whatever else the
 * verb needs.
 *
 * Notably *without* "--request-timeout", which is the obvious way to stop
 * a wedged API server from wedging the DVM and does not work: on kubectl
 * v1.31, passing a non-zero --request-timeout makes it stop falling back
 * to in-cluster configuration, so a launcher running as a pod - the normal
 * way to use this component - suddenly tries http://localhost:8080 and
 * fails with "connection refused" despite a perfectly good service
 * account. ("--request-timeout=0" and every other global flag are fine;
 * it is that one value.) The timeout is enforced with alarm(2) in the
 * forked child instead - see k8s_child() - which is both kubectl-version
 * independent and impossible for a flag to interfere with. */
static char **build_kubectl_argv(const char *verb)
{
    int argc = 0;
    char **argv = NULL;

    pmix_argv_append(&argc, &argv, prte_mca_plm_k8s_component.kubectl_path);
    pmix_argv_append(&argc, &argv, verb);
    append_kubectl_args(&argc, &argv);
    return argv;
}

static int write_manifest_to_tmpfile(const char *manifest, char **path_out)
{
    char *tmpl;
    const char *tmpdir;
    int fd;
    FILE *fp;
    size_t len, written;

    if (NULL != prte_mca_plm_k8s_component.workdir) {
        tmpdir = prte_mca_plm_k8s_component.workdir;
    } else {
        tmpdir = getenv("TMPDIR");
        if (NULL == tmpdir) {
            tmpdir = "/tmp";
        }
    }
    pmix_asprintf(&tmpl, "%s/prte-plm-k8s-XXXXXX", tmpdir);
    if (NULL == tmpl) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    fd = mkstemp(tmpl);
    if (fd < 0) {
        pmix_show_help("help-plm-k8s.txt", "tmpfile-error", true, tmpl, strerror(errno));
        free(tmpl);
        return PRTE_ERR_IN_ERRNO;
    }

    fp = fdopen(fd, "w");
    if (NULL == fp) {
        close(fd);
        unlink(tmpl);
        free(tmpl);
        return PRTE_ERR_IN_ERRNO;
    }

    len = strlen(manifest);
    written = fwrite(manifest, 1, len, fp);
    fclose(fp);
    if (written != len) {
        unlink(tmpl);
        free(tmpl);
        return PRTE_ERR_IN_ERRNO;
    }

    *path_out = tmpl;
    return PRTE_SUCCESS;
}

static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *) 0);
}

/* actually exec kubectl. `quiet` discards its stdout, and every call this
 * component makes is quiet, because kubectl's stdout would otherwise land
 * in the middle of the *application's* output: "job.batch/prterun-... 
 * created" showing up in the middle of "mpirun --pernode ls" is noise the
 * user did not ask for and cannot filter, and the cleanup's list of
 * deleted objects gets printed twice on top of that (terminate_orteds runs
 * it, then finalize runs it again - idempotent by design). The object name
 * is available at "--prtemca plm_base_verbose 1", where output belongs.
 *
 * stderr is always kept: if an apply or a delete fails, that is exactly
 * what explains it, and k8s_wait_apply() prints its own diagnostic around
 * it. */
static void k8s_child(char **argv, bool quiet)
{
    int fdin;
    sigset_t sigs;

    /* don't let kubectl slurp our stdin */
    fdin = open("/dev/null", O_RDWR);
    if (0 <= fdin) {
        dup2(fdin, 0);
        if (quiet) {
            dup2(fdin, 1);
        }
        close(fdin);
    }

    /* close all file descriptors w/ exception of stdin/stdout/stderr */
    pmix_close_open_file_descriptors(-1);

    /* set signal handlers back to the default, close to the execve() -
     * see the identical comment in plm/ssh's ssh_child() for why */
    set_handler_default(SIGTERM);
    set_handler_default(SIGINT);
    set_handler_default(SIGHUP);
    set_handler_default(SIGPIPE);
    set_handler_default(SIGCHLD);
    set_handler_default(SIGALRM);

    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);

    /* The deadline. alarm() survives execve, and SIGALRM's default action
     * is to terminate, so an API server that never answers costs us this
     * many seconds and then shows up as a signalled child in
     * k8s_wait_apply() - no flag passed to kubectl, nothing for a future
     * kubectl release to reinterpret. See build_kubectl_argv() for what
     * this replaced and why. */
    alarm((unsigned) prte_mca_plm_k8s_component.apply_timeout);

    execv(prte_mca_plm_k8s_component.kubectl_path, argv);
    pmix_output(0, "plm:k8s: exec of %s failed with errno=%s(%d)\n",
                prte_mca_plm_k8s_component.kubectl_path, strerror(errno), errno);
    exit(-1);
}

/* Fork one "kubectl apply -f <manifest>" and hand `caddy` to its wait
 * callback. Same shape as plm_slurm_start_proc(): a dummy prte_proc_t
 * carries the pid into PRRTE's SIGCHLD machinery, since there is one
 * child for the whole launch rather than one per daemon. */
static int apply_manifest(prte_plm_k8s_caddy_t *caddy)
{
    char **argv;
    int argc;
    pid_t pid;
    prte_proc_t *dummy;

    argv = build_kubectl_argv("apply");
    argc = PMIx_Argv_count(argv);
    pmix_argv_append(&argc, &argv, "-f");
    pmix_argv_append(&argc, &argv, caddy->manifest_path);

    pid = fork();
    if (pid < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
        PMIx_Argv_free(argv);
        return PRTE_ERR_SYS_LIMITS_CHILDREN;
    }

    if (0 == pid) {
        /* child: put ourselves in our own process group, same rationale
         * as plm/ssh - so a Ctrl-C at the HNP's terminal does not SIGINT
         * a "kubectl apply" mid-flight */
#if HAVE_SETPGID
        if (0 != setpgid(0, 0)) {
            pmix_output(0, "plm:k8s: setpgid(0,0) failed in child with errno=%s(%d)\n",
                        strerror(errno), errno);
            exit(-1);
        }
#endif
        k8s_child(argv, true);
    }

    /* parent */
#if HAVE_SETPGID
    if (0 != setpgid(pid, pid)) {
        /* not fatal - the child is off and running, and we still track it
         * via prte_wait_cb regardless of process group */
        pmix_output(0, "plm:k8s: setpgid(%ld,%ld) failed in parent with errno=%s(%d)\n",
                    (long) pid, (long) pid, strerror(errno), errno);
    }
#endif
    PMIx_Argv_free(argv);

    dummy = PMIX_NEW(prte_proc_t);
    dummy->pid = pid;
    /* be sure to mark it as alive so we don't instantly fire */
    PRTE_FLAG_SET(dummy, PRTE_PROC_FLAG_ALIVE);
    prte_wait_cb(dummy, k8s_wait_apply, (void *) caddy);

    return PRTE_SUCCESS;
}

/*
 * Callback on "kubectl apply" exit. This module never tree-spawns (no
 * remote_spawn), so - unlike plm/ssh's wait callback - this only ever
 * runs on the HNP.
 */
static void k8s_wait_apply(int sd, short flags, void *cbdata)
{
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t *) cbdata;
    prte_plm_k8s_caddy_t *caddy = (prte_plm_k8s_caddy_t *) t2->cbdata;
    prte_proc_t *dummy = t2->child;
    int status = dummy->exit_code;
    PRTE_HIDE_UNUSED_PARAMS(sd, flags);

    if (prte_prteds_term_ordered || prte_abnormal_term_ordered) {
        /* ignore any such report - it will occur if we are already
         * tearing down */
        PMIX_RELEASE(caddy);
        PMIX_RELEASE(t2);
        return;
    }

    if (WIFEXITED(status) && 0 == WEXITSTATUS(status)) {
        unlink(caddy->manifest_path);
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:k8s: applied %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             caddy->object_name));
        /* the daemons themselves still have to start inside their pods
         * and phone home before the DVM considers them up; that part is
         * handled by the base's ordinary daemon-callback machinery, same
         * as any other plm component */
    } else {
        /* the manifest is deliberately left on disk here - it is the one
         * artifact that explains what was actually asked for */
        pmix_output(prte_clean_output,
                    "------------------------------------------------------------\n"
                    "\"%s apply -f ...\" failed while starting the PRRTE daemons in\n"
                    "Kubernetes.\n\n"
                    "  Object:            %s\n"
                    "  Target nodes:      %s\n"
                    "  Rendered manifest: %s\n"
                    "  Exit status:       %d\n\n"
                    "The manifest above was left on disk for inspection.\n"
                    "Common causes: the template's \"image:\" cannot be pulled, a\n"
                    "named node does not exist or cannot accept the pod (taints,\n"
                    "capacity), the cluster's admission policy rejects hostNetwork\n"
                    "pods, the rendered YAML is malformed (run the template through\n"
                    "\"kubectl apply --dry-run=client\" to see where), or the\n"
                    "kubeconfig kubectl resolved does not have permission to create\n"
                    "these objects in the target namespace.\n"
                    "------------------------------------------------------------",
                    prte_mca_plm_k8s_component.kubectl_path, caddy->object_name, caddy->nodes,
                    caddy->manifest_path,
                    WIFEXITED(status) ? WEXITSTATUS(status) : PRTE_ERROR_DEFAULT_EXIT_CODE);

        PRTE_UPDATE_EXIT_STATUS(WIFEXITED(status) ? WEXITSTATUS(status)
                                                  : PRTE_ERROR_DEFAULT_EXIT_CODE);
        /* one failed apply means *no* daemon started, so fail the whole
         * job rather than any one proc - there is no per-daemon child
         * here whose failure could be reported individually */
        PRTE_ACTIVATE_JOB_STATE(caddy->jdata, PRTE_JOB_STATE_FAILED_TO_START);
    }

    PMIX_RELEASE(caddy);
    PMIX_RELEASE(t2);
}

/**
 * Terminate the orteds for a given job
 */
static int k8s_terminate_prteds(void)
{
    int rc;

    /* Unlike ssh, we have no live local connection whose death would take
     * the remote daemon down with it - killing prterun does not touch a
     * pod that's already running. So delete everything this DVM applied
     * right here, as early in the termination sequence as this component
     * gets a hook: terminate_orteds() is what PRRTE calls to tear the
     * daemons down for both a normal shutdown (pterm) and an abort
     * (Ctrl-C), and unlike finalize() - which a second, forced Ctrl-C can
     * skip calling entirely - this one reliably runs and, since it's a
     * blocking fork+waitpid, actually completes before returning. It's
     * also idempotent (--ignore-not-found), so finalize() repeating it on
     * a clean shutdown costs nothing. */
    delete_applied_objects();

    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/* run kubectl to completion, blocking - used only for the delete-on-
 * shutdown cleanup below, which happens well outside the launch/progress
 * critical path (mirrors plm/ssh_finalize's own blocking waitpid loop
 * for its abnormal-termination ssh cleanup) */
static void run_kubectl_sync(char **argv)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        pmix_output(0, "plm:k8s: fork failed with errno=%s(%d)\n", strerror(errno), errno);
        return;
    }
    if (0 == pid) {
        k8s_child(argv, true);
    }
    while (0 > waitpid(pid, &status, 0) && EINTR == errno) {
        continue;
    }
}

/* delete everything this DVM applied, in one shot, via the labels every
 * object carries - no per-object bookkeeping needed, which is what lets
 * this work unchanged whether the template produced one object or many.
 * A template that dropped the "prrte.kubepmix.dev/*" labels simply leaves
 * nothing for this selector to find, which is a silent no-op rather than
 * an error. Not an MCA-configurable toggle - this always runs. */
static void delete_applied_objects(void)
{
    char *base_name, *selector;
    int argc;
    char **argv;

    /* HNP only. Every prted also runs this component - we forward
     * PRTE_MCA_plm=k8s into their environment so they have a plm module
     * like any other daemon - and each of them reaching finalize would
     * otherwise fire its own "kubectl delete" at the objects it is itself
     * running in. In a cluster that is at best a flurry of "Forbidden"
     * from a daemon service account that has no business deleting Jobs,
     * and at worst a daemon tearing the DVM down from underneath itself. */
    if (!PRTE_PROC_IS_MASTER) {
        return;
    }

    base_name = get_base_name();
    if (NULL == base_name) {
        return;
    }
    pmix_asprintf(&selector, PRTE_PLM_K8S_CLEANUP_SELECTOR, base_name);
    free(base_name);
    if (NULL == selector) {
        return;
    }

    argv = build_kubectl_argv("delete");
    argc = PMIx_Argv_count(argv);
    pmix_argv_append(&argc, &argv, PRTE_PLM_K8S_CLEANUP_KINDS);
    pmix_argv_append(&argc, &argv, "-l");
    pmix_argv_append(&argc, &argv, selector);
    pmix_argv_append(&argc, &argv, "--all-namespaces");
    pmix_argv_append(&argc, &argv, "--ignore-not-found");
    pmix_argv_append(&argc, &argv, "--wait=false");

    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:k8s: deleting this DVM's objects (selector \"%s\")",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), selector));
    free(selector);
    run_kubectl_sync(argv);
    PMIx_Argv_free(argv);
}

static int k8s_finalize(void)
{
    int rc = PRTE_SUCCESS;

    /* cleanup any pending recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    if (NULL != prte_mca_plm_k8s_component.kubectl_path) {
        delete_applied_objects();
    }

    free(prte_mca_plm_k8s_component.kubectl_path);
    prte_mca_plm_k8s_component.kubectl_path = NULL;

    return rc;
}
