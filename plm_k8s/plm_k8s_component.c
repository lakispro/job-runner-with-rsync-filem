/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Registration, open/close, and selection query for plm/k8s. Modeled on
 * plm/ssh's component file, but there is no remote-shell agent to hunt
 * for or batch-scheduler environment to probe here - we only need to
 * confirm a "kubectl" binary is reachable in PATH.
 *
 * What the applied object *looks like* (kind, namespace, image, labels,
 * one object or many) is entirely the template's business, not an MCA
 * parameter's - see plm_k8s_template.c. What is registered here are
 * knobs over the launch *mechanism* around that template. Selection
 * priority stays a fixed constant (below), and shutdown cleanup is not a
 * toggle at all (see plm_k8s_module.c) - it always runs.
 *
 * These symbols are kept in a file by themselves for the usual MCA
 * reason: linkers pull in symbols by object file, so keeping them alone
 * here means a tool that only wants to query component metadata does not
 * have to pull in the launch machinery in plm_k8s_module.c.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"
#include "plm_k8s.h"

/* selection priority - kept below ssh's default of 10 so a cluster that
 * happens to have both ssh and kubectl reachable is not launched into
 * Kubernetes by accident; select this component explicitly with
 * "--prtemca plm k8s" */
#define PRTE_PLM_K8S_PRIORITY 5

/*
 * Public string showing the plm k8s component version number
 */
const char *prte_mca_plm_k8s_component_version_string
    = "PRTE k8s plm MCA component version " PRTE_VERSION;

static int k8s_component_register(void);
static int k8s_component_open(void);
static int k8s_component_query(pmix_mca_base_module_t **module, int *priority);
static int k8s_component_close(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
prte_mca_plm_k8s_component_t prte_mca_plm_k8s_component = {
    .super = {
        PRTE_PLM_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "k8s",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = k8s_component_open,
        .pmix_mca_close_component = k8s_component_close,
        .pmix_mca_query_component = k8s_component_query,
        .pmix_mca_register_component_params = k8s_component_register,
    }
};

static int k8s_component_register(void)
{
    pmix_mca_base_component_t *c = &prte_mca_plm_k8s_component.super;

    /* A plain string literal, like every other default in this
     * function: the leading "~/" is expanded against $HOME by
     * prte_plm_k8s_load_template() at the point a template is actually
     * loaded, not here. That keeps registration free of getenv()/heap
     * allocation (which nothing else in this function needs either),
     * and it is also why --help/prte_info show the friendly "~/..."
     * form rather than one particular process's expanded path. */
    prte_mca_plm_k8s_component.template_path = "~/.plm-k8s-template";
    (void) pmix_mca_base_component_var_register(c, "template",
                                                "Path to the Kubernetes manifest template "
                                                "rendered once per launch with every node in "
                                                "scope (see plm_k8s_template.h for the tag "
                                                "vocabulary). A leading \"~/\" is expanded "
                                                "against $HOME. When the file does not exist, a "
                                                "built-in single-Indexed-Job template is used "
                                                "instead",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_k8s_component.template_path);

    /* string literal default: fine to leave as-is for the life of the
     * process, since nothing ever reassigns this field (kubectl_path,
     * the *resolved* binary, is the field that gets replaced/freed) */
    prte_mca_plm_k8s_component.kubectl = "kubectl";
    (void) pmix_mca_base_component_var_register(c, "kubectl",
                                                "The kubectl binary used to apply/delete the "
                                                "daemon manifest, looked up in PATH. It is "
                                                "invoked with no extra configuration of its own, "
                                                "so it resolves KUBECONFIG / ~/.kube/config / "
                                                "in-cluster service-account credentials exactly "
                                                "as it would from an interactive shell",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_k8s_component.kubectl);

    prte_mca_plm_k8s_component.name_prefix = NULL;
    (void) pmix_mca_base_component_var_register(c, "name_prefix",
                                                "Base name substituted for the template's "
                                                "\"{{name}}\" placeholder. Unset derives it from "
                                                "this DVM's nspace",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_k8s_component.name_prefix);

    prte_mca_plm_k8s_component.kubectl_args = NULL;
    (void) pmix_mca_base_component_var_register(c, "kubectl_args",
                                                "Extra space-separated arguments appended to "
                                                "every \"kubectl apply\"/\"kubectl delete\" "
                                                "invocation, e.g. \"--context mycluster\"",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_k8s_component.kubectl_args);

    prte_mca_plm_k8s_component.workdir = NULL;
    (void) pmix_mca_base_component_var_register(c, "workdir",
                                                "Directory used to stage the rendered manifest "
                                                "before \"kubectl apply -f\". Unset uses $TMPDIR, "
                                                "or /tmp if that is also unset",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_k8s_component.workdir);

    prte_mca_plm_k8s_component.pass_environ_mca_params = false;
    (void) pmix_mca_base_component_var_register(c, "pass_environ_mca_params",
                                                "If set to false, do not include MCA params found "
                                                "in our own environment (PMIX_MCA_*/PRTE_MCA_*) "
                                                "in a launched daemon's env",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_plm_k8s_component.pass_environ_mca_params);

    /* True, because both shipped templates honour the allocation's
     * node-to-daemon assignment - the pinned one with nodeName, the
     * default one by having each pod look its own vpid up by the node it
     * landed on. A template that does neither, and lets daemons take
     * whatever identity the scheduler implies, must set this false; be
     * aware that PRRTE then renames nodes from what the daemons report,
     * and if those names are a permutation of the allocated ones its node
     * table ends up inconsistent. */
    prte_mca_plm_k8s_component.assign_nodes = true;
    (void) pmix_mca_base_component_var_register(c, "assign_nodes",
                                                "Whether the template honours the allocation's "
                                                "node-to-daemon assignment - by pinning (nodeName) "
                                                "or by having each daemon look up its vpid from "
                                                "the node it landed on, as both shipped templates "
                                                "do. Set false only for a template that leaves "
                                                "daemon identity to the scheduler",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_plm_k8s_component.assign_nodes);

    prte_mca_plm_k8s_component.apply_timeout = 120;
    (void) pmix_mca_base_component_var_register(c, "apply_timeout",
                                                "Seconds to wait for \"kubectl apply\" or "
                                                "\"kubectl delete\" to return before treating it "
                                                "as failed (must be > 0). Enforced with alarm(2) "
                                                "in the forked child rather than kubectl's own "
                                                "--request-timeout, which on kubectl v1.31 "
                                                "disables in-cluster config resolution",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_plm_k8s_component.apply_timeout);

    return PRTE_SUCCESS;
}

static int k8s_component_open(void)
{
    prte_mca_plm_k8s_component.kubectl_path = NULL;

    if (prte_mca_plm_k8s_component.apply_timeout <= 0) {
        pmix_show_help("help-plm-k8s.txt", "timeout-less-than-zero", true,
                       prte_mca_plm_k8s_component.apply_timeout);
        prte_mca_plm_k8s_component.apply_timeout = 120;
    }

    return PRTE_SUCCESS;
}

static int k8s_component_query(pmix_mca_base_module_t **module, int *priority)
{
    char *path;

    path = pmix_path_findv(prte_mca_plm_k8s_component.kubectl, X_OK, environ, NULL);
    if (NULL == path) {
        /* not an error - we just cannot be selected */
        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:k8s: unable to be used: cannot find \"%s\" in the PATH",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             prte_mca_plm_k8s_component.kubectl));
        *module = NULL;
        return PRTE_ERROR;
    }
    free(prte_mca_plm_k8s_component.kubectl_path);
    prte_mca_plm_k8s_component.kubectl_path = path;

    *priority = PRTE_PLM_K8S_PRIORITY;
    *module = (pmix_mca_base_module_t *) &prte_plm_k8s_module;
    return PRTE_SUCCESS;
}

static int k8s_component_close(void)
{
    return PRTE_SUCCESS;
}
