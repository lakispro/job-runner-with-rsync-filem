/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * plm/k8s - launch PRTE daemons into Kubernetes instead of ssh'ing to a
 * remote host. See plm_k8s_module.c for the launch flow and
 * plm_k8s_template.c for the manifest templating/rendering.
 *
 * Unlike its ancestor in the prrte-k8s-plm repository, this component
 * renders the template *once for the whole launch*, with every node in
 * scope, and applies that one document - so what gets created can be a
 * single object covering all the daemons (the shipped default is one
 * Indexed Job) rather than one Job per daemon.
 *
 * This directory is copied into a PRRTE source tree (at
 * src/mca/plm/k8s/) by this repository's Dockerfile before autogen.pl
 * runs, since PRRTE's MCA components are discovered by that build step
 * scanning src/mca/<framework>/ for component directories.
 */

#ifndef PRTE_PLM_K8S_EXPORT_H
#define PRTE_PLM_K8S_EXPORT_H

#include "prte_config.h"

#include "src/mca/mca.h"

#include "src/mca/plm/base/base.h"
#include "src/mca/plm/plm.h"

BEGIN_C_DECLS

/**
 * PLM k8s component
 *
 * What the applied object *looks like* - kind, namespace, image,
 * resources, extra labels, whether it is one object or many - belongs in
 * the template, not in an MCA parameter. What follows are knobs over the
 * launch *mechanism* around that template: which kubectl, where the
 * manifest is staged, what the object is named. Selection priority and
 * shutdown cleanup are the two things still fixed in code
 * (plm_k8s_component.c / plm_k8s_module.c) rather than exposed here -
 * cleanup in particular is deliberately not a toggle: it always runs.
 */
struct prte_mca_plm_k8s_component_t {
    prte_plm_base_component_t super;

    /* path to the manifest template; defaults to the literal
     * "~/.plm-k8s-template" - the leading "~/" is expanded against
     * $HOME by prte_plm_k8s_load_template() when a template is actually
     * loaded, not here */
    char *template_path;

    /* the kubectl binary (looked up in PATH) and its resolved absolute
     * path, filled in by the component query once kubectl is found */
    char *kubectl;
    char *kubectl_path;

    /* optional override for the "{{name}}" template value; NULL derives
     * it from this DVM's nspace */
    char *name_prefix;

    /* optional extra arguments appended to every kubectl invocation,
     * e.g. "--context mycluster" */
    char *kubectl_args;

    /* directory used to stage the rendered manifest before "kubectl
     * apply -f"; NULL uses $TMPDIR or /tmp */
    char *workdir;

    /* replicate PMIX_MCA_/PRTE_MCA_ env vars found in our own
     * environment into the daemons' env, same idea as
     * plm_ssh_pass_environ_mca_params */
    bool pass_environ_mca_params;

    /* whether the template honours the allocation's node-to-daemon
     * assignment - by pinning with nodeName, or by having each pod look
     * its vpid up from the node it landed on. Sets
     * prte_plm_globals.daemon_nodes_assigned_at_launch - see k8s_init(). */
    bool assign_nodes;

    /* seconds to wait for "kubectl apply" to finish before giving up */
    int apply_timeout;
};
typedef struct prte_mca_plm_k8s_component_t prte_mca_plm_k8s_component_t;

PRTE_MODULE_EXPORT extern prte_mca_plm_k8s_component_t prte_mca_plm_k8s_component;
extern prte_plm_base_module_t prte_plm_k8s_module;

END_C_DECLS

#endif /* PRTE_PLM_K8S_EXPORT_H */
