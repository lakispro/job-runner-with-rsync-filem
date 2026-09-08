/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Registration, open/close and selection query for filem/rsync. Like
 * plm/k8s's component file, the only thing the query has to establish is
 * that the external binary this component shells out to - rsync - is
 * reachable in PATH; everything else is an MCA knob.
 *
 * Priority defaults to 5, above filem/raw's 0, so on an installation
 * that has rsync this component is what "--preload-files" uses without
 * anyone asking for it. Drop it to a negative value (or select raw
 * explicitly with "--prtemca filem raw") to get the stock
 * chunks-over-the-OOB behaviour back.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

#include "filem_rsync.h"

/*
 * Public string for version number
 */
const char *prte_mca_filem_rsync_component_version_string
    = "PRTE FILEM rsync MCA component version " PRTE_VERSION;

char *prte_filem_rsync_binary = NULL;
char *prte_filem_rsync_path = NULL;
int prte_filem_rsync_port = 0;
char *prte_filem_rsync_args = NULL;
int prte_filem_rsync_timeout = 300;
bool prte_filem_rsync_set_workdir = true;
int prte_filem_rsync_priority = 5;

static int filem_rsync_register(void);
static int filem_rsync_open(void);
static int filem_rsync_close(void);
static int filem_rsync_query(pmix_mca_base_module_t **module, int *priority);

prte_filem_base_component_t prte_mca_filem_rsync_component = {
    PRTE_FILEM_BASE_VERSION_2_0_0,
    /* Component name and version */
    .pmix_mca_component_name = "rsync",
    PMIX_MCA_BASE_MAKE_VERSION(component,
                               PRTE_MAJOR_VERSION,
                               PRTE_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_open_component = filem_rsync_open,
    .pmix_mca_close_component = filem_rsync_close,
    .pmix_mca_query_component = filem_rsync_query,
    .pmix_mca_register_component_params = filem_rsync_register,
};

static int filem_rsync_register(void)
{
    pmix_mca_base_component_t *c = &prte_mca_filem_rsync_component;

    /* a string literal, never reassigned - prte_filem_rsync_path holds
     * the *resolved* binary and is the one that gets freed */
    prte_filem_rsync_binary = "rsync";
    (void) pmix_mca_base_component_var_register(c, "rsync",
                                                "The rsync binary, looked up in PATH. It is used "
                                                "both as the short-lived daemon the HNP exports "
                                                "the files from and as the client each prted pulls "
                                                "with, so it must exist on the HNP *and* in the "
                                                "daemons' image",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_filem_rsync_binary);

    prte_filem_rsync_port = 0;
    (void) pmix_mca_base_component_var_register(c, "port",
                                                "TCP port the HNP's rsync daemon listens on. The "
                                                "default (0) asks the OS for a free ephemeral "
                                                "port, which is what you want unless a firewall "
                                                "between the HNP and the daemons needs a fixed one",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_filem_rsync_port);

    prte_filem_rsync_args = NULL;
    (void) pmix_mca_base_component_var_register(c, "args",
                                                "Extra space-separated arguments appended to every "
                                                "rsync *client* invocation, e.g. "
                                                "\"--exclude=.git --exclude=*.o\"",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_filem_rsync_args);

    prte_filem_rsync_timeout = 300;
    (void) pmix_mca_base_component_var_register(c, "timeout",
                                                "Seconds of I/O inactivity after which a transfer "
                                                "is abandoned (rsync --timeout). This is what "
                                                "bounds how long a launch can stall on a wedged "
                                                "transfer, since every daemon acknowledges the "
                                                "pre-position either way",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_filem_rsync_timeout);

    prte_filem_rsync_set_workdir = true;
    (void) pmix_mca_base_component_var_register(c, "set_workdir",
                                                "Run the job's processes in the directory the "
                                                "pre-positioned files landed in, so that a bare "
                                                "\"ls\" shows them. Set false to leave the working "
                                                "directory alone, in which case the files are "
                                                "still delivered but the app has to find them "
                                                "under the daemon's session directory. Ignored "
                                                "for --preload-binary, whose rewritten "
                                                "\"./<name>\" command only resolves from there",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_filem_rsync_set_workdir);

    prte_filem_rsync_priority = 5;
    (void) pmix_mca_base_component_var_register(c, "priority",
                                                "Selection priority. The default (5) beats "
                                                "filem/raw's 0, so rsync is used whenever it is "
                                                "available; lower it, or select raw explicitly, to "
                                                "go back to streaming files over the OOB",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_filem_rsync_priority);

    return PRTE_SUCCESS;
}

static int filem_rsync_open(void)
{
    prte_filem_rsync_path = NULL;
    return PRTE_SUCCESS;
}

static int filem_rsync_close(void)
{
    free(prte_filem_rsync_path);
    prte_filem_rsync_path = NULL;
    return PRTE_SUCCESS;
}

static int filem_rsync_query(pmix_mca_base_module_t **module, int *priority)
{
    char *path;

    path = pmix_path_findv(prte_filem_rsync_binary, X_OK, environ, NULL);
    if (NULL == path) {
        /* not an error - filem/raw will pick the job up */
        PMIX_OUTPUT_VERBOSE((1, prte_filem_base_framework.framework_output,
                             "filem:rsync: unable to be used: cannot find \"%s\" in the PATH",
                             prte_filem_rsync_binary));
        *module = NULL;
        return PRTE_ERROR;
    }
    free(prte_filem_rsync_path);
    prte_filem_rsync_path = path;

    *priority = prte_filem_rsync_priority;
    *module = (pmix_mca_base_module_t *) &prte_filem_rsync_module;
    return PRTE_SUCCESS;
}
