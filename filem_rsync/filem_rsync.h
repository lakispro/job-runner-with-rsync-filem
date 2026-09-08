/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * filem/rsync - pre-position files with rsync(1) instead of streaming
 * them through the PRRTE out-of-band.
 *
 * The stock filem/raw component reads each preloaded file on the HNP and
 * xcasts it to every daemon in 4 KiB chunks over the OOB, which works but
 * is a poor fit for "ship me this whole directory": it is one transfer
 * per *file*, it re-sends bytes that are already on the far side, and
 * everything it delivers lands flattened in the session directory. This
 * component keeps the same framework contract and the same HNP-xcasts /
 * daemons-ack shape, but moves the bytes with rsync, which gets
 * directory trees, permissions, deltas and compression for free.
 *
 * The transport is a short-lived rsync daemon on the HNP rather than
 * "rsync -e ssh": the whole point of running under plm/k8s is that there
 * is no ssh to the place the daemons are, but every prted can already
 * reach the HNP over TCP - that is how it phoned home. So the HNP starts
 * "rsync --daemon" on an ephemeral port, exports one read-only module,
 * and tells the daemons where to pull from; they connect back over the
 * same network path the OOB uses.
 *
 * See filem_rsync_module.c for the flow, and this repository's README for
 * the resulting "mpirun --preload-files $(pwd) --pernode ls" behaviour.
 */

#ifndef PRTE_FILEM_RSYNC_EXPORT_H
#define PRTE_FILEM_RSYNC_EXPORT_H

#include "prte_config.h"

#include "src/mca/mca.h"

#include "src/mca/filem/filem.h"

BEGIN_C_DECLS

/* the rsync binary, looked up in PATH, and its resolved absolute path
 * (filled in by the component query once rsync is found) */
extern char *prte_filem_rsync_binary;
extern char *prte_filem_rsync_path;

/* TCP port for the HNP's rsync daemon; 0 asks the OS for a free one */
extern int prte_filem_rsync_port;

/* extra arguments appended to every rsync *client* invocation, e.g.
 * "--exclude=.git" */
extern char *prte_filem_rsync_args;

/* per-transfer rsync --timeout, in seconds */
extern int prte_filem_rsync_timeout;

/* run the job's processes in the directory the pre-positioned files
 * landed in, so a bare "ls" shows them */
extern bool prte_filem_rsync_set_workdir;

/* selection priority - above filem/raw's 0, so this component is the
 * default wherever rsync exists */
extern int prte_filem_rsync_priority;

PRTE_MODULE_EXPORT extern prte_filem_base_component_t prte_mca_filem_rsync_component;
extern prte_filem_base_module_t prte_filem_rsync_module;

END_C_DECLS

#endif /* PRTE_FILEM_RSYNC_EXPORT_H */
