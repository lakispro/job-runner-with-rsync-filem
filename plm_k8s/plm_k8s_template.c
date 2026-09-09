/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See plm_k8s_template.h for the contract this file implements.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/mca/plm/base/plm_private.h"

#include "plm_k8s_template.h"

/* -------------------------------------------------------------------- */
/* a tiny growable byte buffer - avoids pulling in a dependency just to  */
/* stitch a handful of strings together                                 */
/* -------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} k8s_strbuf_t;

static void sb_init(k8s_strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->failed = false;
}

static void sb_reserve(k8s_strbuf_t *sb, size_t extra)
{
    char *newdata;
    size_t newcap;

    if (sb->failed) {
        return;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return;
    }
    newcap = (0 == sb->cap) ? 256 : sb->cap;
    while (newcap < sb->len + extra + 1) {
        newcap *= 2;
    }
    newdata = (char *) realloc(sb->data, newcap);
    if (NULL == newdata) {
        free(sb->data);
        sb->data = NULL;
        sb->len = 0;
        sb->cap = 0;
        sb->failed = true;
        return;
    }
    sb->data = newdata;
    sb->cap = newcap;
}

static void sb_append_n(k8s_strbuf_t *sb, const char *s, size_t n)
{
    if (sb->failed || 0 == n || NULL == s) {
        return;
    }
    sb_reserve(sb, n);
    if (sb->failed) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(k8s_strbuf_t *sb, const char *s)
{
    if (NULL != s) {
        sb_append_n(sb, s, strlen(s));
    }
}

static void sb_append_char(k8s_strbuf_t *sb, char c)
{
    sb_append_n(sb, &c, 1);
}

/* hands ownership of the buffer to the caller; NULL on OOM */
static char *sb_release(k8s_strbuf_t *sb)
{
    char *ret;

    if (sb->failed) {
        return NULL;
    }
    if (NULL == sb->data) {
        return strdup("");
    }
    ret = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return ret;
}

static char *dupn(const char *s, size_t n)
{
    char *d = (char *) malloc(n + 1);
    if (NULL != d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* -------------------------------------------------------------------- */
/* template loading                                                     */
/* -------------------------------------------------------------------- */

/* Expand a leading "~/" (or a bare "~") against $HOME; anything else -
 * an absolute or relative path, as given explicitly via
 * -mca plm_k8s_template - passes through unchanged. Returns a new heap
 * buffer, or NULL if the path needs $HOME and $HOME is unset/empty.
 *
 * This is done here, at load time, rather than baking $HOME into the MCA
 * var's default at registration time: that keeps the registered default
 * a plain literal ("~/.plm-k8s-template"), which is what every other
 * default in this component is, and what --help/prte_info show.
 *
 * Only "~/" and a bare "~" are recognized - "~otheruser/..." is left as
 * literal text (and will simply fail to stat()), since resolving another
 * account's home directory needs getpwnam(), which is more machinery
 * than this one default is worth. */
static char *expand_home(const char *path)
{
    const char *home;
    char *expanded;

    if ('~' != path[0]) {
        return strdup(path);
    }
    if ('\0' != path[1] && '/' != path[1]) {
        return strdup(path);
    }
    home = getenv("HOME");
    if (NULL == home || '\0' == home[0]) {
        return NULL;
    }
    if ('\0' == path[1]) {
        return strdup(home);
    }
    /* path[1] == '/' here, so path+1 keeps the leading slash */
    pmix_asprintf(&expanded, "%s%s", home, path + 1);
    return expanded;
}

char *prte_plm_k8s_load_template(const char *path, int *rc)
{
    FILE *fp;
    struct stat st;
    char *buf;
    char *resolved;
    size_t total, n;

    *rc = PRTE_SUCCESS;
    if (NULL == path || '\0' == path[0]) {
        *rc = PRTE_ERR_NOT_FOUND;
        return NULL;
    }
    resolved = expand_home(path);
    if (NULL == resolved) {
        /* "~/..." was asked for but $HOME is unset - nowhere to look */
        *rc = PRTE_ERR_NOT_FOUND;
        return NULL;
    }
    if (0 != stat(resolved, &st)) {
        free(resolved);
        *rc = PRTE_ERR_NOT_FOUND;
        return NULL;
    }
    fp = fopen(resolved, "r");
    if (NULL == fp) {
        free(resolved);
        *rc = PRTE_ERR_IN_ERRNO;
        return NULL;
    }
    free(resolved);
    buf = (char *) malloc((size_t) st.st_size + 1);
    if (NULL == buf) {
        fclose(fp);
        *rc = PRTE_ERR_OUT_OF_RESOURCE;
        return NULL;
    }
    total = 0;
    while (total < (size_t) st.st_size) {
        n = fread(buf + total, 1, (size_t) st.st_size - total, fp);
        if (0 == n) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';
    fclose(fp);
    return buf;
}

/* -------------------------------------------------------------------- */
/* name sanitization                                                    */
/* -------------------------------------------------------------------- */

char *prte_plm_k8s_sanitize_name(const char *raw, size_t maxlen)
{
    size_t rawlen, i, out;
    char *buf;
    bool last_dash;

    if (NULL == raw || '\0' == raw[0]) {
        return strdup("prte");
    }
    rawlen = strlen(raw);
    buf = (char *) malloc(((rawlen < maxlen) ? rawlen : maxlen) + 1);
    if (NULL == buf) {
        return NULL;
    }
    out = 0;
    last_dash = true; /* swallow any leading run of non-alnum chars */
    for (i = 0; i < rawlen && out < maxlen; i++) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            buf[out++] = c;
            last_dash = false;
        } else if (!last_dash) {
            buf[out++] = '-';
            last_dash = true;
        }
        /* else: another non-alnum right after one already emitted as
         * '-' (or at the very start) - collapse it away */
    }
    while (out > 0 && '-' == buf[out - 1]) {
        out--;
    }
    if (0 == out) {
        free(buf);
        return strdup("prte");
    }
    buf[out] = '\0';
    return buf;
}

/* -------------------------------------------------------------------- */
/* argv -> env conversion                                               */
/* -------------------------------------------------------------------- */

/* Append "KEY=VALUE" unless KEY is already there.
 *
 * Duplicates are not hypothetical: prte_plm_base_prted_append_basic_args()
 * forwards MCA settings it finds in our own environment onto the command
 * line, so "--prtemca plm k8s" typed by the user comes back to us there
 * *and* is appended again by launch_daemons(). One "KEY=VALUE" twice in a
 * shell environment is harmless; two entries with the same "name:" in a
 * Kubernetes container spec is not. First occurrence wins, which is the
 * one PRRTE itself put on the command line. */
static void append_unique_envar(char ***envars, const char *key, const char *value)
{
    size_t keylen = strlen(key);
    char *entry;
    int i;

    for (i = 0; NULL != *envars && NULL != (*envars)[i]; i++) {
        if (0 == strncmp((*envars)[i], key, keylen) && '=' == (*envars)[i][keylen]) {
            return;
        }
    }
    pmix_asprintf(&entry, "%s=%s", key, value);
    if (NULL != entry) {
        PMIx_Argv_append_nosize(envars, entry);
        free(entry);
    }
}

/* MCA parameters that must never reach a daemon.
 *
 * "plm" and everything under "plm_k8s_" are ours and ours alone: this
 * component never tree-spawns, so no daemon launches anything, and a
 * prted only opens the plm framework at all if PRTE_MCA_plm is set in its
 * environment (ess_base_std_prted.c gates it on exactly that, with the
 * comment "the prted has no need of the proxy PLM at all"). Forwarding
 * them would make every daemon load a launcher it cannot use, and - worse
 * - would make *this component* a requirement of the worker image rather
 * than only the launcher's, which is the opposite of what a k8s launch
 * should need. The daemon pods should be able to run a stock Open MPI.
 *
 * This is not a hypothetical leak: prte_plm_base_prted_append_basic_args()
 * copies every PRTE_MCA_* / PMIX_MCA_* it finds in *our* environment onto
 * the daemon command line, so a launcher that selects this component with
 * "export PRTE_MCA_plm=k8s" - the natural thing to do in a pod spec -
 * hands it straight to the daemons unless it is filtered here. */
static bool is_launcher_only_param(const char *name)
{
    return (0 == strcmp(name, "plm") || 0 == strncmp(name, "plm_k8s_", 8));
}

/* True if this "--prtemca <name> <value>" is here only because
 * prte_plm_base_prted_append_basic_args() found PRTE_MCA_<name>=<value> in
 * our own environment and copied it onto the command line. That is the
 * distinction plm_k8s_pass_environ_mca_params is about, and by the time we
 * see the argv it is the only way left to draw it. */
static bool came_from_our_environ(const char *prefix, const char *name, const char *value)
{
    char *key;
    const char *ours;
    bool match;

    pmix_asprintf(&key, "%s%s", prefix, name);
    if (NULL == key) {
        return false;
    }
    ours = getenv(key);
    free(key);
    match = (NULL != ours && 0 == strcmp(ours, value));
    return match;
}

char **prte_plm_k8s_argv_to_envars(char **argv, const char *skip, bool pass_environ)
{
    char **envars = NULL;
    char *key;
    int i;

    if (NULL == argv || NULL == argv[0]) {
        return NULL;
    }
    for (i = 0; NULL != argv[i]; i++) {
        const char *prefix = NULL;

        if (0 == strcmp(argv[i], "--prtemca") && NULL != argv[i + 1] && NULL != argv[i + 2]) {
            prefix = "PRTE_MCA_";
        } else if (0 == strcmp(argv[i], "--pmixmca") && NULL != argv[i + 1]
                   && NULL != argv[i + 2]) {
            prefix = "PMIX_MCA_";
        }

        if (NULL != prefix) {
            const char *name = argv[i + 1];
            const char *value = argv[i + 2];

            if ((NULL == skip || 0 != strcmp(name, skip))
                && !is_launcher_only_param(name)
                && (pass_environ || !came_from_our_environ(prefix, name, value))) {
                pmix_asprintf(&key, "%s%s", prefix, name);
                if (NULL != key) {
                    append_unique_envar(&envars, key, value);
                    free(key);
                }
            }
            i += 2;
        } else {
            PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "plm:k8s: dropping daemon flag \"%s\" - no environment-variable "
                                 "equivalent for a k8s launch",
                                 argv[i]));
        }
    }
    return envars;
}

/* -------------------------------------------------------------------- */
/* rendering                                                             */
/* -------------------------------------------------------------------- */

/* Scalar tags. "which" indexes into the resolver below; anything not
 * listed here (including real Jinja/Go control flow) is left alone. */
enum {
    TAG_NAME = 0,
    TAG_RANK,
    TAG_ENV,
    TAG_NODE,
    TAG_INDEX,
    TAG_NUMNODES,
    TAG_VPID_START,
    TAG_NODES_CSV,
    TAG_NODE_VPIDS,
    TAG_SLOTS
};

typedef struct {
    const char *token;
    int which;
} spelling_t;

/* every scalar in all four spellings - kept as a flat table rather than
 * parsed generically because the whole tag vocabulary is fixed and this
 * stays trivially greppable */
#define SPELL4(base, w)                                                   \
    {"{{" base "}}", w}, {"{{ " base " }}", w}, {"{{." base "}}", w},      \
    {"{{ ." base " }}", w}

static const spelling_t spellings[] = {
    SPELL4("name", TAG_NAME),          SPELL4("rank", TAG_RANK),
    SPELL4("env", TAG_ENV),            SPELL4("node", TAG_NODE),
    SPELL4("index", TAG_INDEX),        SPELL4("numnodes", TAG_NUMNODES),
    SPELL4("vpid_start", TAG_VPID_START), SPELL4("nodes_csv", TAG_NODES_CSV),
    SPELL4("node_vpids", TAG_NODE_VPIDS),  SPELL4("slots", TAG_SLOTS),
};
#define NUM_SPELLINGS (sizeof(spellings) / sizeof(spellings[0]))

/* Section delimiters, in both supported spellings. Index i of "starts"
 * pairs with index i of "ends". */
static const char *const section_starts[] = {"{{#nodes}}", "{% for node in nodes %}"};
static const char *const section_ends[] = {"{{/nodes}}", "{% endfor %}"};
#define NUM_SECTION_SPELLINGS (sizeof(section_starts) / sizeof(section_starts[0]))

static const char *find_next_placeholder(const char *s, int *which, size_t *toklen)
{
    const char *best = NULL;
    size_t i;

    for (i = 0; i < NUM_SPELLINGS; i++) {
        const char *p = strstr(s, spellings[i].token);
        if (NULL != p && (NULL == best || p < best)) {
            best = p;
            *which = spellings[i].which;
            *toklen = strlen(spellings[i].token);
        }
    }
    return best;
}

/* the indentation of the line `pos` sits on, but only if `pos` is
 * preceded on that line solely by spaces/tabs - otherwise "" (no
 * reindent) */
static char *line_indent(const char *buf_start, const char *pos)
{
    const char *line_start = pos;
    const char *p;

    while (line_start > buf_start && '\n' != *(line_start - 1)) {
        line_start--;
    }
    p = line_start;
    while (p < pos && (' ' == *p || '\t' == *p)) {
        p++;
    }
    if (p != pos) {
        return strdup("");
    }
    return dupn(line_start, (size_t) (pos - line_start));
}

static void append_reindented(k8s_strbuf_t *sb, const char *block, const char *indent)
{
    const char *p = block;
    bool first = true;

    while ('\0' != *p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = (NULL != nl) ? (size_t) (nl - p) : strlen(p);

        if (!first) {
            sb_append_char(sb, '\n');
            sb_append(sb, indent);
        }
        sb_append_n(sb, p, linelen);
        first = false;
        if (NULL == nl) {
            break;
        }
        p = nl + 1;
    }
}

static char *yaml_dquote(const char *raw)
{
    k8s_strbuf_t sb;
    const char *p;

    sb_init(&sb);
    sb_append_char(&sb, '"');
    for (p = raw; '\0' != *p; p++) {
        switch (*p) {
            case '"':
                sb_append(&sb, "\\\"");
                break;
            case '\\':
                sb_append(&sb, "\\\\");
                break;
            case '\n':
                sb_append(&sb, "\\n");
                break;
            case '\t':
                sb_append(&sb, "\\t");
                break;
            default:
                sb_append_char(&sb, *p);
                break;
        }
    }
    sb_append_char(&sb, '"');
    return sb_release(&sb);
}

/* renders envars as a YAML sequence of {name, value} entries, e.g.:
 *   - name: PRTE_MCA_ess_base_vpid
 *     value: "3"
 *   - name: PRTE_MCA_ess_base_nspace
 *     value: "prte-hnp-1234"
 * or "[]" if there is nothing to render. Never returns NULL except on
 * allocation failure. */
static char *build_env_block(char **envars)
{
    k8s_strbuf_t sb;
    int i;
    bool first;

    sb_init(&sb);
    if (NULL == envars || NULL == envars[0]) {
        sb_append(&sb, "[]");
        return sb_release(&sb);
    }
    first = true;
    for (i = 0; NULL != envars[i]; i++) {
        char *eq = strchr(envars[i], '=');
        char *key, *val, *qvalue;

        if (NULL == eq) {
            continue;
        }
        key = dupn(envars[i], (size_t) (eq - envars[i]));
        val = strdup(eq + 1);
        if (NULL == key || NULL == val) {
            free(key);
            free(val);
            continue;
        }
        qvalue = yaml_dquote(val);
        if (NULL == qvalue) {
            free(key);
            free(val);
            continue;
        }
        if (!first) {
            sb_append_char(&sb, '\n');
        }
        sb_append(&sb, "- name: ");
        sb_append(&sb, key);
        sb_append(&sb, "\n  value: ");
        sb_append(&sb, qvalue);
        first = false;
        free(key);
        free(val);
        free(qvalue);
    }
    if (first) {
        /* every entry was malformed (should not happen - we generate
         * envars ourselves) */
        sb_append(&sb, "[]");
    }
    return sb_release(&sb);
}

/* the values a scalar tag can resolve to in one scope. `node` is NULL in
 * global scope, in which case node-only tags render as empty and
 * {{env}} resolves to the common block. */
typedef struct {
    const prte_plm_k8s_tctx_t *ctx;
    const prte_plm_k8s_tnode_t *node;
    int index;
    const char *common_env_block;
    const char *node_env_block;
    const char *nodes_csv;
    const char *node_vpids;
} scope_t;

/* Render one span of template text - which never contains a section tag,
 * the caller having split those out - into `sb`. */
static void render_span(k8s_strbuf_t *sb, const char *text, const scope_t *scope)
{
    const char *cursor = text;
    char scratch[32];

    for (;;) {
        int which = -1;
        size_t toklen = 0;
        const char *match = find_next_placeholder(cursor, &which, &toklen);
        const char *multiline = NULL;

        if (NULL == match) {
            sb_append(sb, cursor);
            return;
        }
        sb_append_n(sb, cursor, (size_t) (match - cursor));

        switch (which) {
        case TAG_NAME:
            sb_append(sb, scope->ctx->name);
            break;
        case TAG_NUMNODES:
            snprintf(scratch, sizeof(scratch), "%d", scope->ctx->num_nodes);
            sb_append(sb, scratch);
            break;
        case TAG_VPID_START:
            snprintf(scratch, sizeof(scratch), "%d", scope->ctx->vpid_start);
            sb_append(sb, scratch);
            break;
        case TAG_NODES_CSV:
            sb_append(sb, scope->nodes_csv);
            break;
        case TAG_NODE_VPIDS:
            sb_append(sb, scope->node_vpids);
            break;
        case TAG_NODE:
            if (NULL != scope->node) {
                sb_append(sb, scope->node->node);
            }
            break;
        case TAG_RANK:
            if (NULL != scope->node) {
                sb_append(sb, scope->node->rank);
            }
            break;
        case TAG_SLOTS:
            if (NULL != scope->node) {
                snprintf(scratch, sizeof(scratch), "%d", scope->node->slots);
                sb_append(sb, scratch);
            }
            break;
        case TAG_INDEX:
            if (NULL != scope->node) {
                snprintf(scratch, sizeof(scratch), "%d", scope->index);
                sb_append(sb, scratch);
            }
            break;
        case TAG_ENV:
            /* the only multi-line value, and the only tag the node scope
             * shadows: inside a section it is that daemon's complete env,
             * outside it is the block common to all of them */
            multiline = (NULL != scope->node) ? scope->node_env_block
                                              : scope->common_env_block;
            break;
        default:
            break;
        }

        if (NULL != multiline) {
            char *indent = line_indent(text, match);
            append_reindented(sb, multiline, indent);
            free(indent);
        }
        cursor = match + toklen;
    }
}

/* Mustache's standalone-tag rule: a section tag whose line holds nothing
 * but whitespace around it takes the whole line with it, so
 *
 *     values:
 *     {{#nodes}}
 *       - "{{node}}"
 *     {{/nodes}}
 *
 * emits just the list items and not three lines of stray indentation.
 * `open` points at the tag, `close` just past it; on return *open and
 * *close have been widened to swallow the line if the rule applies. */
static void widen_standalone(const char *buf_start, const char **open, const char **close)
{
    const char *p = *open;
    const char *q = *close;

    while (p > buf_start && (' ' == *(p - 1) || '\t' == *(p - 1))) {
        p--;
    }
    if (p != buf_start && '\n' != *(p - 1)) {
        return; /* something other than whitespace precedes it */
    }
    while (' ' == *q || '\t' == *q) {
        q++;
    }
    if ('\n' != *q && '\0' != *q) {
        return; /* something other than whitespace follows it */
    }
    if ('\n' == *q) {
        q++;
    }
    *open = p;
    *close = q;
}

/* find the first section-opening tag at or after `s`; returns NULL if
 * there is none. On success *spelling says which pair it was. */
static const char *find_section_start(const char *s, size_t *spelling)
{
    const char *best = NULL;
    size_t i;

    for (i = 0; i < NUM_SECTION_SPELLINGS; i++) {
        const char *p = strstr(s, section_starts[i]);
        if (NULL != p && (NULL == best || p < best)) {
            best = p;
            *spelling = i;
        }
    }
    return best;
}

char *prte_plm_k8s_render(const char *template_text, const prte_plm_k8s_tctx_t *ctx)
{
    k8s_strbuf_t sb;
    const char *cursor;
    char *common_env_block = NULL;
    char *nodes_csv = NULL;
    char *node_vpids = NULL;
    char **csv = NULL;
    char **pairs = NULL;
    scope_t scope;
    char *result;
    int i;

    if (NULL == template_text || NULL == ctx) {
        return NULL;
    }

    common_env_block = build_env_block(ctx->common_envars);
    if (NULL == common_env_block) {
        return NULL;
    }
    for (i = 0; i < ctx->num_nodes; i++) {
        char *pair;
        PMIx_Argv_append_nosize(&csv, ctx->nodes[i].node);
        pmix_asprintf(&pair, "%s:%s", ctx->nodes[i].node, ctx->nodes[i].rank);
        if (NULL != pair) {
            PMIx_Argv_append_nosize(&pairs, pair);
            free(pair);
        }
    }
    nodes_csv = (NULL == csv) ? strdup("") : PMIx_Argv_join(csv, ',');
    node_vpids = (NULL == pairs) ? strdup("") : PMIx_Argv_join(pairs, ',');
    PMIx_Argv_free(csv);
    PMIx_Argv_free(pairs);
    if (NULL == nodes_csv || NULL == node_vpids) {
        free(common_env_block);
        free(nodes_csv);
        free(node_vpids);
        return NULL;
    }

    memset(&scope, 0, sizeof(scope));
    scope.ctx = ctx;
    scope.common_env_block = common_env_block;
    scope.nodes_csv = nodes_csv;
    scope.node_vpids = node_vpids;

    sb_init(&sb);
    cursor = template_text;
    for (;;) {
        size_t spelling = 0;
        const char *open = find_section_start(cursor, &spelling);
        const char *open_end, *close, *close_end, *body;
        char *body_text;

        if (NULL == open) {
            /* no section left - the tail renders in global scope */
            render_span(&sb, cursor, &scope);
            break;
        }
        open_end = open + strlen(section_starts[spelling]);
        close = strstr(open_end, section_ends[spelling]);
        if (NULL == close) {
            /* unterminated section - pass the rest through verbatim
             * rather than guessing where the author meant it to end */
            render_span(&sb, cursor, &scope);
            break;
        }
        close_end = close + strlen(section_ends[spelling]);

        widen_standalone(template_text, &open, &open_end);
        widen_standalone(template_text, &close, &close_end);

        /* text before the section, in global scope */
        body_text = dupn(cursor, (size_t) (open - cursor));
        if (NULL == body_text) {
            goto oom;
        }
        render_span(&sb, body_text, &scope);
        free(body_text);

        /* the body, once per node, in node scope */
        body = open_end;
        body_text = dupn(body, (size_t) (close - body));
        if (NULL == body_text) {
            goto oom;
        }
        for (i = 0; i < ctx->num_nodes; i++) {
            char *node_env_block = build_env_block(ctx->nodes[i].envars);
            if (NULL == node_env_block) {
                free(body_text);
                goto oom;
            }
            scope.node = &ctx->nodes[i];
            scope.index = i;
            scope.node_env_block = node_env_block;
            render_span(&sb, body_text, &scope);
            free(node_env_block);
        }
        scope.node = NULL;
        scope.node_env_block = NULL;
        free(body_text);

        cursor = close_end;
    }

    free(common_env_block);
    free(nodes_csv);
    free(node_vpids);
    result = sb_release(&sb);
    return result;

oom:
    free(common_env_block);
    free(nodes_csv);
    free(node_vpids);
    free(sb_release(&sb));
    return NULL;
}
