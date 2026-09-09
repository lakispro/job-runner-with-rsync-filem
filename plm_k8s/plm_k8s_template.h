/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Manifest templating for plm/k8s.
 *
 * The difference from the one-Job-per-daemon ancestor of this component
 * (the prrte-k8s-plm repository) is the shape of the data the template
 * sees: instead of rendering the template once per daemon with that one
 * daemon's values, plm/k8s here renders it *once for the whole launch*
 * with the entire node list in scope, and applies the single rendered
 * document. Whatever the template makes of that list - one Indexed Job
 * covering every node (the shipped default), a JobSet/LeaderWorkerSet,
 * a "kind: List" of per-node Jobs - is the template's business, not the
 * component's.
 *
 * That means the engine needs one thing the old placeholder-substituter
 * did not have: a repetition construct. It is still deliberately not a
 * real Jinja/Go template engine - PRRTE has no dependency on either -
 * but it is now a small Mustache-shaped one: scalar tags plus a single
 * "repeat this body once per node" section. Anything that is not one of
 * the recognized spellings below passes through untouched, so a
 * template that needs more than this can be pre-rendered with a real
 * engine and pointed at with plm_k8s_template.
 *
 * ----------------------------------------------------------------------
 * Tags
 * ----------------------------------------------------------------------
 *
 * Scalars, in any of the spellings "{{x}}", "{{ x }}", "{{.x}}",
 * "{{ .x }}" (so both Jinja- and Go-flavored templates read naturally):
 *
 *   global scope (and inside a section, unless shadowed):
 *     {{name}}        sanitized base name shared by this whole DVM
 *     {{numnodes}}    how many daemons this launch is starting
 *     {{vpid_start}}  the vpid of the first of them
 *     {{nodes_csv}}   every node name, comma separated
 *     {{node_vpids}}  "<node>:<vpid>,<node>:<vpid>,..." - the allocation's
 *                     node-to-daemon assignment, in a form a container
 *                     entrypoint can look itself up in. This is how a
 *                     single-pod-template resource stays correct without
 *                     per-pod placement: the pod reads its own node name
 *                     from the downward API and takes the vpid PRRTE gave
 *                     *that* node, so wherever the scheduler puts it, the
 *                     map PRRTE built still describes reality.
 *     {{env}}         the env block *common* to every daemon: the whole
 *                     prted command line as PRTE_MCA_* / PMIX_MCA_*
 *                     assignments, minus ess_base_vpid, which is the one
 *                     value that differs per daemon. A single-pod-template
 *                     resource pairs this with a command that derives
 *                     ess_base_vpid from its own index - see
 *                     templates/single-job.yaml.tmpl.
 *
 *   node scope (only inside a section; {{env}} is shadowed there):
 *     {{node}}        the node PRRTE's allocation picked for this daemon
 *     {{rank}}        this daemon's vpid
 *     {{index}}       0-based position of this node within the section
 *     {{slots}}       slots the allocation gave this node
 *     {{env}}         this daemon's *complete* env block, ess_base_vpid
 *                     included - what a per-node resource wants
 *
 * A multi-line value ({{env}}) is re-indented to the column its tag sat
 * at, provided the tag is preceded on its line only by whitespace.
 *
 * Sections, repeating their body once per node, in either spelling:
 *
 *     {{#nodes}} ... {{/nodes}}
 *     {% for node in nodes %} ... {% endfor %}
 *
 * A section tag alone on its line (only whitespace around it) takes that
 * whole line with it, so the surrounding YAML keeps its indentation -
 * the usual Mustache "standalone tag" rule. Sections do not nest.
 * ----------------------------------------------------------------------
 */

#ifndef PRTE_PLM_K8S_TEMPLATE_H
#define PRTE_PLM_K8S_TEMPLATE_H

#include "prte_config.h"

BEGIN_C_DECLS

/* one daemon-to-be, as the template sees it inside a section */
typedef struct {
    char *node;     /* node name, owned */
    char *rank;     /* vpid, decimal, owned */
    char **envars;  /* complete env for this daemon, owned */
    int slots;
} prte_plm_k8s_tnode_t;

/* everything the template can see */
typedef struct {
    const char *name;
    prte_plm_k8s_tnode_t *nodes;
    int num_nodes;
    char **common_envars; /* every daemon's env minus ess_base_vpid */
    int vpid_start;
} prte_plm_k8s_tctx_t;

/* Read the manifest template from `path` into a NUL-terminated heap
 * buffer the caller frees with free(). A leading "~/" (or a bare "~") in
 * `path` is expanded against $HOME first - this is where
 * plm_k8s_template's default ("~/.plm-k8s-template") actually gets
 * resolved, rather than at MCA var registration time.
 *
 * A missing file is not an error: NULL is returned with *rc set to
 * PRTE_ERR_NOT_FOUND so the caller can fall back to
 * prte_plm_k8s_default_template. That also covers "~/..." was given but
 * $HOME is unset. Any other failure (exists but unreadable, out of
 * memory, ...) also returns NULL, with *rc set to the underlying PRTE
 * error code. */
PRTE_EXPORT char *prte_plm_k8s_load_template(const char *path, int *rc);

/* The built-in default: the single Indexed Job described above, used
 * whenever the configured template file does not exist. Defined in the
 * generated plm_k8s_default_template.c, which contrib/embed-default-template.py
 * produces from templates/single-job.yaml.tmpl - so the file people copy
 * and the string linked into the component cannot drift apart. */
PRTE_EXPORT extern const char prte_plm_k8s_default_template[];

/* Render `template_text` against `ctx` per the tag table above. Returns
 * a new heap buffer the caller frees with free(), or NULL on error. */
PRTE_EXPORT char *prte_plm_k8s_render(const char *template_text,
                                      const prte_plm_k8s_tctx_t *ctx);

/* Sanitize `raw` into a string safe to use as a Kubernetes object name /
 * DNS-1123 label: lowercased, anything outside [a-z0-9-] becomes '-',
 * runs of '-' collapsed to one, leading/trailing '-' trimmed, and
 * truncated to at most `maxlen` bytes (re-trimming a trailing '-' left by
 * the cut). Falls back to "prte" if `raw` is NULL, empty, or sanitizes to
 * nothing. Returns a new heap buffer the caller frees with free(), or
 * NULL only on allocation failure. */
PRTE_EXPORT char *prte_plm_k8s_sanitize_name(const char *raw, size_t maxlen);

/* Convert an argv built by prte_plm_base_prted_append_basic_args() - a run
 * of "--prtemca"/"--pmixmca" <name> <value> triples, with a handful of
 * bare leading debug flags - into PRTE_MCA_* / PMIX_MCA_* environment
 * assignments. This relies on PRTE's MCA variable system treating
 * "--prtemca <name> <value>" and "PRTE_MCA_<name>=<value>" (and the
 * pmixmca/PMIX_MCA_ equivalent) as interchangeable, which is exactly what
 * lets a bare `prted` (no argv at all) pick up its whole configuration
 * from its container's env: block.
 *
 * Three things are filtered out:
 *
 *   - an MCA parameter named by `skip` (e.g. "ess_base_vpid") - that is
 *     how the common env block gets built out of the same argv as the
 *     per-node ones;
 *   - always, "plm" and everything under "plm_k8s_": they are the
 *     launcher's business and forwarding them would make this component a
 *     requirement of the *worker* image (see is_launcher_only_param());
 *   - unless `pass_environ`, any parameter that is on the command line
 *     only because prte_plm_base_prted_append_basic_args() copied it
 *     there out of our own environment. That is what
 *     plm_k8s_pass_environ_mca_params selects between.
 *
 * A bare flag (e.g. "--debug-daemons") has no env-var equivalent this
 * component can infer; it is logged at output-verbosity 1 and dropped.
 *
 * Returns a NULL-terminated PMIx_Argv-style array the caller frees with
 * PMIx_Argv_free(), or NULL if `argv` is NULL or empty. */
PRTE_EXPORT char **prte_plm_k8s_argv_to_envars(char **argv, const char *skip,
                                               bool pass_environ);

END_C_DECLS

#endif /* PRTE_PLM_K8S_TEMPLATE_H */
