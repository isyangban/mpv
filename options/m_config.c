/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/// \file
/// \ingroup Config

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>

#include "libmpv/client.h"

#include "mpv_talloc.h"

#include "m_config.h"
#include "options/m_option.h"
#include "common/global.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "misc/node.h"
#include "osdep/atomics.h"

static const union m_option_value default_value;

// Profiles allow to predefine some sets of options that can then
// be applied later on with the internal -profile option.
#define MAX_PROFILE_DEPTH 20
// Maximal include depth.
#define MAX_RECURSION_DEPTH 8

struct m_config_group {
    const struct m_sub_options *group; // or NULL for top-level options
    int parent_group;   // index of parent group in m_config.groups
    void *opts;         // pointer to group user option struct
    atomic_llong ts;    // incremented on every write access
};

struct m_profile {
    struct m_profile *next;
    char *name;
    char *desc;
    int num_opts;
    // Option/value pair array.
    char **opts;
};

// In the file local case, this contains the old global value.
struct m_opt_backup {
    struct m_opt_backup *next;
    struct m_config_option *co;
    void *backup;
};

static int parse_include(struct m_config *config, struct bstr param, bool set,
                         int flags)
{
    if (param.len == 0)
        return M_OPT_MISSING_PARAM;
    if (!set)
        return 1;
    if (config->recursion_depth >= MAX_RECURSION_DEPTH) {
        MP_ERR(config, "Maximum 'include' nesting depth exceeded.\n");
        return M_OPT_INVALID;
    }
    char *filename = bstrdup0(NULL, param);
    config->recursion_depth += 1;
    config->includefunc(config->includefunc_ctx, filename, flags);
    config->recursion_depth -= 1;
    talloc_free(filename);
    return 1;
}

static int parse_profile(struct m_config *config, const struct m_option *opt,
                         struct bstr name, struct bstr param, bool set, int flags)
{
    if (!bstrcmp0(param, "help")) {
        struct m_profile *p;
        if (!config->profiles) {
            MP_INFO(config, "No profiles have been defined.\n");
            return M_OPT_EXIT - 1;
        }
        MP_INFO(config, "Available profiles:\n");
        for (p = config->profiles; p; p = p->next)
            MP_INFO(config, "\t%s\t%s\n", p->name, p->desc ? p->desc : "");
        MP_INFO(config, "\n");
        return M_OPT_EXIT - 1;
    }

    char **list = NULL;
    int r = m_option_type_string_list.parse(config->log, opt, name, param, &list);
    if (r < 0)
        return r;
    if (!list || !list[0])
        return M_OPT_INVALID;
    for (int i = 0; list[i]; i++) {
        if (set)
            r = m_config_set_profile(config, list[i], flags);
        if (r < 0)
            break;
    }
    m_option_free(opt, &list);
    return r;
}

static int show_profile(struct m_config *config, bstr param)
{
    struct m_profile *p;
    if (!param.len)
        return M_OPT_MISSING_PARAM;
    if (!(p = m_config_get_profile(config, param))) {
        MP_ERR(config, "Unknown profile '%.*s'.\n", BSTR_P(param));
        return M_OPT_EXIT - 1;
    }
    if (!config->profile_depth)
        MP_INFO(config, "Profile %s: %s\n", p->name,
                p->desc ? p->desc : "");
    config->profile_depth++;
    for (int i = 0; i < p->num_opts; i++) {
        MP_INFO(config, "%*s%s=%s\n", config->profile_depth, "",
                p->opts[2 * i], p->opts[2 * i + 1]);

        if (config->profile_depth < MAX_PROFILE_DEPTH
            && !strcmp(p->opts[2*i], "profile")) {
            char *e, *list = p->opts[2 * i + 1];
            while ((e = strchr(list, ','))) {
                int l = e - list;
                if (!l)
                    continue;
                show_profile(config, (bstr){list, e - list});
                list = e + 1;
            }
            if (list[0] != '\0')
                show_profile(config, bstr0(list));
        }
    }
    config->profile_depth--;
    if (!config->profile_depth)
        MP_INFO(config, "\n");
    return M_OPT_EXIT - 1;
}

static int list_options(struct m_config *config)
{
    m_config_print_option_list(config);
    return M_OPT_EXIT;
}

// The memcpys are supposed to work around the strict aliasing violation,
// that would result if we just dereferenced a void** (where the void** is
// actually casted from struct some_type* ). The dummy struct type is in
// theory needed, because void* and struct pointers could have different
// representations, while pointers to different struct types don't.
static void *substruct_read_ptr(const void *ptr)
{
    struct mp_dummy_ *res;
    memcpy(&res, ptr, sizeof(res));
    return res;
}
static void substruct_write_ptr(void *ptr, void *val)
{
    struct mp_dummy_ *src = val;
    memcpy(ptr, &src, sizeof(src));
}

static void add_options(struct m_config *config,
                        struct m_config_option *parent,
                        void *optstruct,
                        const void *optstruct_def,
                        const struct m_option *defs);

// For use with m_config_cache.
struct m_config_shadow {
    pthread_mutex_t lock;
    struct m_config *root;
    char *data;
};

static void config_destroy(void *p)
{
    struct m_config *config = p;
    m_config_restore_backups(config);
    for (int n = 0; n < config->num_opts; n++) {
        struct m_config_option *co = &config->opts[n];

        m_option_free(co->opt, co->data);

        if (config->shadow && co->shadow_offset >= 0)
            m_option_free(co->opt, config->shadow->data + co->shadow_offset);
    }

    if (config->shadow)
        pthread_mutex_destroy(&config->shadow->lock);
}

struct m_config *m_config_new(void *talloc_ctx, struct mp_log *log,
                              size_t size, const void *defaults,
                              const struct m_option *options)
{
    struct m_config *config = talloc(talloc_ctx, struct m_config);
    talloc_set_destructor(config, config_destroy);
    *config = (struct m_config)
        {.log = log, .size = size, .defaults = defaults, .options = options};

    // size==0 means a dummy object is created
    if (size) {
        config->optstruct = talloc_zero_size(config, size);
        if (defaults)
            memcpy(config->optstruct, defaults, size);
    }

    config->num_groups = 1;
    MP_TARRAY_GROW(config, config->groups, 1);
    config->groups[0] = (struct m_config_group){
        .parent_group = -1,
        .opts = config->optstruct,
    };

    if (options)
        add_options(config, NULL, config->optstruct, defaults, options);
    return config;
}

void m_config_create_shadow(struct m_config *config)
{
    assert(config->global && config->options && config->size);
    assert(!config->shadow && !config->global->config);

    config->shadow = talloc_zero(config, struct m_config_shadow);
    config->shadow->data = talloc_zero_size(config->shadow, config->shadow_size);

    config->shadow->root = config;
    pthread_mutex_init(&config->shadow->lock, NULL);

    config->global->config = config->shadow;

    for (int n = 0; n < config->num_opts; n++) {
        struct m_config_option *co = &config->opts[n];
        if (co->shadow_offset < 0)
            continue;
        m_option_copy(co->opt, config->shadow->data + co->shadow_offset, co->data);
    }
}

// Return whether parent is a parent of group. Also returns true if they're equal.
static bool is_group_included(struct m_config *config, int group, int parent)
{
    for (;;) {
        if (group == parent)
            return true;
        if (group < 0)
            break;
        group = config->groups[group].parent_group;
    }
    return false;
}

struct m_config_cache *m_config_cache_alloc(void *ta_parent, struct mp_log *log,
                                            struct mpv_global *global,
                                            const struct m_sub_options *group)
{
    struct m_config_shadow *shadow = global->config;
    struct m_config *root = shadow->root;

    struct m_config_cache *cache = talloc_zero(ta_parent, struct m_config_cache);
    cache->shadow = shadow;
    cache->shadow_config = m_config_new(cache, log ? log : mp_null_log,
                                        root->size, root->defaults,
                                        root->options);

    struct m_config *config = cache->shadow_config;

    assert(config->num_opts == root->num_opts);
    for (int n = 0; n < root->num_opts; n++) {
        assert(config->opts[n].opt->type == root->opts[n].opt->type);
        assert(config->opts[n].shadow_offset == root->opts[n].shadow_offset);
    }

    cache->ts = -1;
    cache->group = -1;

    for (int n = 0; n < config->num_groups; n++) {
        if (config->groups[n].group == group) {
            cache->opts = config->groups[n].opts;
            cache->group = n;
            break;
        }
    }

    assert(cache->group >= 0);
    assert(cache->opts);

    // If we're not on the top-level, restrict set of options to the sub-group
    // to reduce update costs. (It would be better not to add them in the first
    // place.)
    if (cache->group > 0) {
        int num_opts = config->num_opts;
        config->num_opts = 0;
        for (int n = 0; n < num_opts; n++) {
            struct m_config_option *co = &config->opts[n];
            if (is_group_included(config, co->group, cache->group)) {
                config->opts[config->num_opts++] = *co;
            } else {
                m_option_free(co->opt, co->data);
            }
        }
        for (int n = 0; n < config->num_groups; n++) {
            if (!is_group_included(config, n, cache->group))
                TA_FREEP(&config->groups[n].opts);
        }
    }

    m_config_cache_update(cache);

    return cache;
}

bool m_config_cache_update(struct m_config_cache *cache)
{
    struct m_config_shadow *shadow = cache->shadow;

    // Using atomics and checking outside of the lock - it's unknown whether
    // this makes it faster or slower. Just cargo culting it.
    if (atomic_load(&shadow->root->groups[cache->group].ts) <= cache->ts)
        return false;

    pthread_mutex_lock(&shadow->lock);
    cache->ts = atomic_load(&shadow->root->groups[cache->group].ts);
    for (int n = 0; n < cache->shadow_config->num_opts; n++) {
        struct m_config_option *co = &cache->shadow_config->opts[n];
        if (co->shadow_offset >= 0)
            m_option_copy(co->opt, co->data, shadow->data + co->shadow_offset);
    }
    pthread_mutex_unlock(&shadow->lock);
    return true;
}

void m_config_notify_change_co(struct m_config *config,
                               struct m_config_option *co)
{
    struct m_config_shadow *shadow = config->shadow;

    if (shadow) {
        pthread_mutex_lock(&shadow->lock);
        if (co->shadow_offset >= 0)
            m_option_copy(co->opt, shadow->data + co->shadow_offset, co->data);
        pthread_mutex_unlock(&shadow->lock);

        int group = co->group;
        while (group >= 0) {
            atomic_fetch_add(&config->groups[group].ts, 1);
            group = config->groups[group].parent_group;
        }
    }

    if (config->global && (co->opt->flags & M_OPT_TERM))
        mp_msg_update_msglevels(config->global);
}

struct m_config *mp_get_root_config(struct mpv_global *global)
{
    return global->config->root;
}

struct m_config *m_config_from_obj_desc(void *talloc_ctx, struct mp_log *log,
                                        struct m_obj_desc *desc)
{
    return m_config_new(talloc_ctx, log, desc->priv_size, desc->priv_defaults,
                        desc->options);
}

// Like m_config_from_obj_desc(), but don't allocate option struct.
struct m_config *m_config_from_obj_desc_noalloc(void *talloc_ctx,
                                                struct mp_log *log,
                                                struct m_obj_desc *desc)
{
    return m_config_new(talloc_ctx, log, 0, desc->priv_defaults, desc->options);
}

int m_config_set_obj_params(struct m_config *conf, char **args)
{
    for (int n = 0; args && args[n * 2 + 0]; n++) {
        int r = m_config_set_option(conf, bstr0(args[n * 2 + 0]),
                                    bstr0(args[n * 2 + 1]));
        if (r < 0)
            return r;
    }
    return 0;
}

int m_config_apply_defaults(struct m_config *config, const char *name,
                            struct m_obj_settings *defaults)
{
    int r = 0;
    for (int n = 0; defaults && defaults[n].name; n++) {
        struct m_obj_settings *entry = &defaults[n];
        if (name && strcmp(entry->name, name) == 0) {
            r = m_config_set_obj_params(config, entry->attribs);
            break;
        }
    }
    return r;
}

struct m_config *m_config_from_obj_desc_and_args(void *ta_parent,
    struct mp_log *log, struct mpv_global *global, struct m_obj_desc *desc,
    const char *name, struct m_obj_settings *defaults, char **args)
{
    struct m_config *config = m_config_from_obj_desc(ta_parent, log, desc);
    if (m_config_apply_defaults(config, name, defaults) < 0)
        goto error;
    if (m_config_set_obj_params(config, args) < 0)
        goto error;

    return config;
error:
    talloc_free(config);
    return NULL;
}

static void ensure_backup(struct m_config *config, struct m_config_option *co)
{
    if (co->opt->type->flags & M_OPT_TYPE_HAS_CHILD)
        return;
    if (co->opt->flags & M_OPT_GLOBAL)
        return;
    if (!co->data)
        return;
    for (struct m_opt_backup *cur = config->backup_opts; cur; cur = cur->next) {
        if (cur->co->data == co->data) // comparing data ptr catches aliases
            return;
    }
    struct m_opt_backup *bc = talloc_ptrtype(NULL, bc);
    *bc = (struct m_opt_backup) {
        .co = co,
        .backup = talloc_zero_size(bc, co->opt->type->size),
    };
    m_option_copy(co->opt, bc->backup, co->data);
    bc->next = config->backup_opts;
    config->backup_opts = bc;
    co->is_set_locally = true;
}

void m_config_restore_backups(struct m_config *config)
{
    while (config->backup_opts) {
        struct m_opt_backup *bc = config->backup_opts;
        config->backup_opts = bc->next;

        m_option_copy(bc->co->opt, bc->co->data, bc->backup);
        m_option_free(bc->co->opt, bc->backup);
        bc->co->is_set_locally = false;
        talloc_free(bc);
    }
}

void m_config_backup_opt(struct m_config *config, const char *opt)
{
    struct m_config_option *co = m_config_get_co(config, bstr0(opt));
    if (co) {
        ensure_backup(config, co);
    } else {
        MP_ERR(config, "Option %s not found.\n", opt);
    }
}

void m_config_backup_all_opts(struct m_config *config)
{
    for (int n = 0; n < config->num_opts; n++)
        ensure_backup(config, &config->opts[n]);
}

static void m_config_add_option(struct m_config *config,
                                struct m_config_option *parent,
                                void *optstruct,
                                const void *optstruct_def,
                                const struct m_option *arg);

static void add_options(struct m_config *config,
                        struct m_config_option *parent,
                        void *optstruct,
                        const void *optstruct_def,
                        const struct m_option *defs)
{
    for (int i = 0; defs && defs[i].name; i++)
        m_config_add_option(config, parent, optstruct, optstruct_def, &defs[i]);
}

// Initialize a field with a given value. In case this is dynamic data, it has
// to be allocated and copied. src can alias dst, also can be NULL.
static void init_opt_inplace(const struct m_option *opt, void *dst,
                             const void *src)
{
    union m_option_value temp = {0};
    if (src)
        memcpy(&temp, src, opt->type->size);
    memset(dst, 0, opt->type->size);
    m_option_copy(opt, dst, &temp);
}

static void m_config_add_option(struct m_config *config,
                                struct m_config_option *parent,
                                void *optstruct,
                                const void *optstruct_def,
                                const struct m_option *arg)
{
    assert(config != NULL);
    assert(arg != NULL);

    const char *parent_name = parent ? parent->name : "";

    struct m_config_option co = {
        .opt = arg,
        .name = arg->name,
        .shadow_offset = -1,
        .group = parent ? parent->group : 0,
    };

    if (arg->offset >= 0) {
        if (optstruct)
            co.data = (char *)optstruct + arg->offset;
        if (optstruct_def)
            co.default_data = (char *)optstruct_def + arg->offset;
        int size = arg->type->size;
        if (optstruct && size) {
            // The required alignment is unknown, so go with the minimum C
            // could require. Slightly wasteful, but not that much.
            int align = (size - config->shadow_size % size) % size;
            co.shadow_offset = config->shadow_size + align;
            config->shadow_size = co.shadow_offset + size;
        }
    }

    if (arg->defval)
        co.default_data = arg->defval;

    if (!co.default_data)
        co.default_data = &default_value;

    // Fill in the full name
    if (!co.name[0]) {
        co.name = parent_name;
    } else if (parent_name[0]) {
        co.name = talloc_asprintf(config, "%s-%s", parent_name, co.name);
    }

    if (co.opt->deprecation_message)
        co.is_hidden = true;

    // Option with children -> add them
    if (arg->type->flags & M_OPT_TYPE_HAS_CHILD) {
        const struct m_sub_options *subopts = arg->priv;

        // Can't be used multiple times.
        for (int n = 0; n < config->num_groups; n++)
            assert(config->groups[n].group != subopts);

        void *new_optstruct = NULL;
        if (co.data) {
            new_optstruct = m_config_alloc_struct(config, subopts);
            substruct_write_ptr(co.data, new_optstruct);
        }

        const void *new_optstruct_def = substruct_read_ptr(co.default_data);
        if (!new_optstruct_def)
            new_optstruct_def = subopts->defaults;

        int parent_group = co.group;
        co.group = config->num_groups++;
        MP_TARRAY_GROW(config, config->groups, co.group);
        struct m_config_group *group = &config->groups[co.group];
        *group = (struct m_config_group){
            .group = subopts,
            .parent_group = parent_group,
            .opts = new_optstruct,
        };

        add_options(config, &co, new_optstruct, new_optstruct_def, subopts->opts);
    } else {
        // Initialize options
        if (co.data && co.default_data)
            init_opt_inplace(arg, co.data, co.default_data);
    }

    if (arg->name[0]) // no own name -> hidden
        MP_TARRAY_APPEND(config, config->opts, config->num_opts, co);
}

struct m_config_option *m_config_get_co(const struct m_config *config,
                                        struct bstr name)
{
    if (!name.len)
        return NULL;

    for (int n = 0; n < config->num_opts; n++) {
        struct m_config_option *co = &config->opts[n];
        struct bstr coname = bstr0(co->name);
        bool matches = false;
        if ((co->opt->type->flags & M_OPT_TYPE_ALLOW_WILDCARD)
                && bstr_endswith0(coname, "*")) {
            coname.len--;
            if (bstrcmp(bstr_splice(name, 0, coname.len), coname) == 0)
                matches = true;
        } else if (bstrcmp(coname, name) == 0)
            matches = true;
        if (matches) {
            const char *prefix = config->is_toplevel ? "--" : "";
            if (co->opt->type == &m_option_type_alias) {
                const char *alias = (const char *)co->opt->priv;
                // deprecation_message is not used, but decides whether it's a
                // proper or deprecated alias.
                if (co->opt->deprecation_message && !co->warning_was_printed) {
                    MP_WARN(config, "Warning: option %s%s was replaced with "
                            "%s%s and might be removed in the future.\n",
                            prefix, co->name, prefix, alias);
                    co->warning_was_printed = true;
                }
                return m_config_get_co(config, bstr0(alias));
            } else if (co->opt->type == &m_option_type_removed) {
                if (!co->warning_was_printed) {
                    char *msg = co->opt->priv;
                    if (msg) {
                        MP_FATAL(config, "Option %s%s was removed: %s\n",
                                 prefix, co->name, msg);
                    } else {
                        MP_FATAL(config, "Option %s%s was removed.\n",
                                 prefix, co->name);
                    }
                    co->warning_was_printed = true;
                }
                return NULL;
            } else if (co->opt->deprecation_message) {
                if (!co->warning_was_printed) {
                    MP_WARN(config, "Warning: option %s%s is deprecated "
                            "and might be removed in the future (%s).\n",
                            prefix, co->name, co->opt->deprecation_message);
                    co->warning_was_printed = true;
                }
            }
            return co;
        }
    }
    return NULL;
}

int m_config_get_co_count(struct m_config *config)
{
    return config->num_opts;
}

struct m_config_option *m_config_get_co_index(struct m_config *config, int index)
{
    return &config->opts[index];
}

const char *m_config_get_positional_option(const struct m_config *config, int p)
{
    int pos = 0;
    for (int n = 0; n < config->num_opts; n++) {
        struct m_config_option *co = &config->opts[n];
        if (!co->is_hidden) {
            if (pos == p)
                return co->name;
            pos++;
        }
    }
    return NULL;
}

// return: <0: M_OPT_ error, 0: skip, 1: check, 2: set
static int handle_set_opt_flags(struct m_config *config,
                                struct m_config_option *co, int flags)
{
    int optflags = co->opt->flags;
    bool set = !(flags & M_SETOPT_CHECK_ONLY);

    if ((flags & M_SETOPT_PRE_PARSE_ONLY) && !(optflags & M_OPT_PRE_PARSE))
        return 0;

    if ((flags & M_SETOPT_PRESERVE_CMDLINE) && co->is_set_from_cmdline)
        set = false;

    if ((flags & M_SETOPT_NO_FIXED) && (optflags & M_OPT_FIXED))
        return M_OPT_INVALID;

    if ((flags & M_SETOPT_NO_PRE_PARSE) && (optflags & M_OPT_PRE_PARSE))
        return M_OPT_INVALID;

    // Check if this option isn't forbidden in the current mode
    if ((flags & M_SETOPT_FROM_CONFIG_FILE) && (optflags & M_OPT_NOCFG)) {
        MP_ERR(config, "The %s option can't be used in a config file.\n",
               co->name);
        return M_OPT_INVALID;
    }
    if (flags & M_SETOPT_BACKUP) {
        if (optflags & M_OPT_GLOBAL) {
            MP_ERR(config, "The %s option is global and can't be set per-file.\n",
                   co->name);
            return M_OPT_INVALID;
        }
        if (set)
            ensure_backup(config, co);
    }

    return set ? 2 : 1;
}

static void handle_on_set(struct m_config *config, struct m_config_option *co,
                          int flags)
{
    if (flags & M_SETOPT_FROM_CMDLINE)
        co->is_set_from_cmdline = true;

    m_config_notify_change_co(config, co);
}

// The type data points to is as in: co->opt
int m_config_set_option_raw(struct m_config *config, struct m_config_option *co,
                            void *data, int flags)
{
    if (!co)
        return M_OPT_UNKNOWN;

    // This affects some special options like "include", "profile". Maybe these
    // should work, or maybe not. For now they would require special code.
    if (!co->data)
        return M_OPT_UNKNOWN;

    int r = handle_set_opt_flags(config, co, flags);
    if (r <= 1)
        return r;

    m_option_copy(co->opt, co->data, data);
    handle_on_set(config, co, flags);
    return 0;
}

static int parse_subopts(struct m_config *config, char *name, char *prefix,
                         struct bstr param, int flags);

// Used to turn "--no-foo" into "--foo=no".
static struct m_config_option *m_config_find_negation_opt(struct m_config *config,
                                                          struct bstr *name)
{
    assert(!m_config_get_co(config, *name));

    if (!bstr_eatstart0(name, "no-"))
        return NULL;

    struct m_config_option *co = m_config_get_co(config, *name);

    // Not all choice types have this value - if they don't, then parsing them
    // will simply result in an error. Good enough.
    if (co && co->opt->type != CONF_TYPE_FLAG &&
              co->opt->type != CONF_TYPE_CHOICE &&
              co->opt->type != &m_option_type_aspect)
        co = NULL;

    return co;
}

static int m_config_parse_option(struct m_config *config, struct bstr name,
                                 struct bstr param, int flags)
{
    assert(config != NULL);

    struct m_config_option *co = m_config_get_co(config, name);
    if (!co) {
        co = m_config_find_negation_opt(config, &name);
        if (!co)
            return M_OPT_UNKNOWN;

        if (param.len)
            return M_OPT_DISALLOW_PARAM;

        param = bstr0("no");
    }

    // This is the only mandatory function
    assert(co->opt->type->parse);

    int r = handle_set_opt_flags(config, co, flags);
    if (r <= 0)
        return r;
    bool set = r == 2;

    if (set) {
        MP_VERBOSE(config, "Setting option '%.*s' = '%.*s' (flags = %d)\n",
                   BSTR_P(name), BSTR_P(param), flags);
    }

    if (config->includefunc && bstr_equals0(name, "include"))
        return parse_include(config, param, set, flags);
    if (config->use_profiles && bstr_equals0(name, "profile"))
        return parse_profile(config, co->opt, name, param, set, flags);
    if (config->use_profiles && bstr_equals0(name, "show-profile"))
        return show_profile(config, param);
    if (bstr_equals0(name, "list-options"))
        return list_options(config);

    // Option with children are a bit different to parse
    if (co->opt->type->flags & M_OPT_TYPE_HAS_CHILD) {
        char prefix[110];
        if (!config->subopt_deprecation_warning) {
            MP_WARN(config, "Suboptions (--%.*s=...) are deprecated. Use "
                    "flat options instead.\n", BSTR_P(name));
            config->subopt_deprecation_warning = true;
        }
        assert(strlen(co->name) < 100);
        sprintf(prefix, "%s-", co->name);
        return parse_subopts(config, (char *)co->name, prefix, param, flags);
    }

    r = m_option_parse(config->log, co->opt, name, param, set ? co->data : NULL);

    if (r >= 0 && set)
        handle_on_set(config, co, flags);

    return r;
}

static int parse_subopts(struct m_config *config, char *name, char *prefix,
                         struct bstr param, int flags)
{
    char **lst = NULL;
    // Split the argument into child options
    int r = m_option_type_subconfig.parse(config->log, NULL, bstr0(""), param, &lst);
    if (r < 0)
        return r;
    // Parse the child options
    for (int i = 0; lst && lst[2 * i]; i++) {
        // Build the full name
        char n[110];
        if (snprintf(n, 110, "%s%s", prefix, lst[2 * i]) > 100)
            abort();
        r = m_config_parse_option(config,bstr0(n), bstr0(lst[2 * i + 1]), flags);
        if (r < 0) {
            if (r > M_OPT_EXIT) {
                MP_ERR(config, "Error parsing suboption %s/%s (%s)\n",
                       name, lst[2 * i], m_option_strerror(r));
                r = M_OPT_INVALID;
            }
            break;
        }
    }
    talloc_free(lst);
    return r;
}

int m_config_parse_suboptions(struct m_config *config, char *name,
                              char *subopts)
{
    if (!subopts || !*subopts)
        return 0;
    int r = parse_subopts(config, name, "", bstr0(subopts), 0);
    if (r < 0 && r > M_OPT_EXIT) {
        MP_ERR(config, "Error parsing suboption %s (%s)\n",
               name, m_option_strerror(r));
        r = M_OPT_INVALID;
    }
    return r;
}

int m_config_set_option_ext(struct m_config *config, struct bstr name,
                            struct bstr param, int flags)
{
    int r = m_config_parse_option(config, name, param, flags);
    if (r < 0 && r > M_OPT_EXIT) {
        MP_ERR(config, "Error parsing option %.*s (%s)\n",
               BSTR_P(name), m_option_strerror(r));
        r = M_OPT_INVALID;
    }
    return r;
}

int m_config_set_option(struct m_config *config, struct bstr name,
                                 struct bstr param)
{
    return m_config_set_option_ext(config, name, param, 0);
}

int m_config_set_option_node(struct m_config *config, bstr name,
                             struct mpv_node *data, int flags)
{
    struct mpv_node tmp;
    int r;

    struct m_config_option *co = m_config_get_co(config, name);
    if (!co) {
        bstr orig_name = name;
        co = m_config_find_negation_opt(config, &name);
        if (!co)
            return M_OPT_UNKNOWN;
        if (!(data->format == MPV_FORMAT_STRING && !bstr0(data->u.string).len) &&
            !(data->format == MPV_FORMAT_FLAG && data->u.flag == 1))
            return M_OPT_INVALID;
        tmp.format = MPV_FORMAT_STRING;
        tmp.u.string = "no";
        data = &tmp;

        if (!co->warning_was_printed) {
            MP_WARN(config, "Option '%.*s': setting 'no-' option via API is "
                    "deprecated and will stop working.\n", BSTR_P(orig_name));
            co->warning_was_printed = true;
        }
    }

    // Do this on an "empty" type to make setting the option strictly overwrite
    // the old value, as opposed to e.g. appending to lists.
    union m_option_value val = {0};

    if (data->format == MPV_FORMAT_STRING) {
        bstr param = bstr0(data->u.string);
        r = m_option_parse(mp_null_log, co->opt, name, param, &val);
    } else {
        r = m_option_set_node(co->opt, &val, data);
    }

    if (r >= 0)
        r = m_config_set_option_raw(config, co, &val, flags);

    if (mp_msg_test(config->log, MSGL_V)) {
        char *s = m_option_type_node.print(NULL, data);
        MP_VERBOSE(config, "Setting option '%.*s' = %s (flags = %d) -> %d\n",
                   BSTR_P(name), s ? s : "?", flags, r);
        talloc_free(s);
    }

    m_option_free(co->opt, &val);
    return r;
}

int m_config_option_requires_param(struct m_config *config, bstr name)
{
    struct m_config_option *co = m_config_get_co(config, name);
    if (!co)
        return m_config_find_negation_opt(config, &name) ? 0 : M_OPT_UNKNOWN;
    if (bstr_endswith0(name, "-clr"))
        return 0;
    return m_option_required_params(co->opt);
}

static int sort_opt_compare(const void *pa, const void *pb)
{
    const struct m_config_option *a = pa;
    const struct m_config_option *b = pb;
    return strcasecmp(a->name, b->name);
}

void m_config_print_option_list(const struct m_config *config)
{
    char min[50], max[50];
    int count = 0;
    const char *prefix = config->is_toplevel ? "--" : "";

    struct m_config_option *sorted =
        talloc_memdup(NULL, config->opts, config->num_opts * sizeof(sorted[0]));
    if (config->is_toplevel)
        qsort(sorted, config->num_opts, sizeof(sorted[0]), sort_opt_compare);

    MP_INFO(config, "Options:\n\n");
    for (int i = 0; i < config->num_opts; i++) {
        struct m_config_option *co = &sorted[i];
        const struct m_option *opt = co->opt;
        if (opt->type->flags & M_OPT_TYPE_HAS_CHILD)
            continue;
        if (co->is_hidden)
            continue;
        MP_INFO(config, " %s%-30s", prefix, co->name);
        if (opt->type == &m_option_type_choice) {
            MP_INFO(config, " Choices:");
            struct m_opt_choice_alternatives *alt = opt->priv;
            for (int n = 0; alt[n].name; n++)
                MP_INFO(config, " %s", alt[n].name);
            if (opt->flags & (M_OPT_MIN | M_OPT_MAX))
                MP_INFO(config, " (or an integer)");
        } else {
            MP_INFO(config, " %s", co->opt->type->name);
        }
        if (opt->flags & (M_OPT_MIN | M_OPT_MAX)) {
            snprintf(min, sizeof(min), "any");
            snprintf(max, sizeof(max), "any");
            if (opt->flags & M_OPT_MIN)
                snprintf(min, sizeof(min), "%.14g", opt->min);
            if (opt->flags & M_OPT_MAX)
                snprintf(max, sizeof(max), "%.14g", opt->max);
            MP_INFO(config, " (%s to %s)", min, max);
        }
        char *def = NULL;
        if (co->default_data)
            def = m_option_print(co->opt, co->default_data);
        if (def) {
            MP_INFO(config, " (default: %s)", def);
            talloc_free(def);
        }
        if (opt->flags & M_OPT_GLOBAL)
            MP_INFO(config, " [global]");
        if (opt->flags & M_OPT_NOCFG)
            MP_INFO(config, " [nocfg]");
        if (opt->flags & M_OPT_FILE)
            MP_INFO(config, " [file]");
        MP_INFO(config, "\n");
        count++;
    }
    MP_INFO(config, "\nTotal: %d options\n", count);
    talloc_free(sorted);
}

char **m_config_list_options(void *ta_parent, const struct m_config *config)
{
    char **list = talloc_new(ta_parent);
    int count = 0;
    for (int i = 0; i < config->num_opts; i++) {
        struct m_config_option *co = &config->opts[i];
        const struct m_option *opt = co->opt;
        if (opt->type->flags & M_OPT_TYPE_HAS_CHILD)
            continue;
        if (co->is_hidden)
            continue;
        // For use with CONF_TYPE_STRING_LIST, it's important not to set list
        // as allocation parent.
        char *s = talloc_strdup(ta_parent, co->name);
        MP_TARRAY_APPEND(ta_parent, list, count, s);
    }
    MP_TARRAY_APPEND(ta_parent, list, count, NULL);
    return list;
}

struct m_profile *m_config_get_profile(const struct m_config *config, bstr name)
{
    for (struct m_profile *p = config->profiles; p; p = p->next) {
        if (bstr_equals0(name, p->name))
            return p;
    }
    return NULL;
}

struct m_profile *m_config_get_profile0(const struct m_config *config,
                                        char *name)
{
    return m_config_get_profile(config, bstr0(name));
}

struct m_profile *m_config_add_profile(struct m_config *config, char *name)
{
    if (!name || !name[0] || strcmp(name, "default") == 0)
        return NULL; // never a real profile
    struct m_profile *p = m_config_get_profile0(config, name);
    if (p)
        return p;
    p = talloc_zero(config, struct m_profile);
    p->name = talloc_strdup(p, name);
    p->next = config->profiles;
    config->profiles = p;
    return p;
}

void m_profile_set_desc(struct m_profile *p, bstr desc)
{
    talloc_free(p->desc);
    p->desc = bstrdup0(p, desc);
}

int m_config_set_profile_option(struct m_config *config, struct m_profile *p,
                                bstr name, bstr val)
{
    int i = m_config_set_option_ext(config, name, val,
                                    M_SETOPT_CHECK_ONLY |
                                    M_SETOPT_FROM_CONFIG_FILE);
    if (i < 0)
        return i;
    p->opts = talloc_realloc(p, p->opts, char *, 2 * (p->num_opts + 2));
    p->opts[p->num_opts * 2] = bstrdup0(p, name);
    p->opts[p->num_opts * 2 + 1] = bstrdup0(p, val);
    p->num_opts++;
    p->opts[p->num_opts * 2] = p->opts[p->num_opts * 2 + 1] = NULL;
    return 1;
}

int m_config_set_profile(struct m_config *config, char *name, int flags)
{
    struct m_profile *p = m_config_get_profile0(config, name);
    if (!p) {
        MP_WARN(config, "Unknown profile '%s'.\n", name);
        return M_OPT_INVALID;
    }

    if (config->profile_depth > MAX_PROFILE_DEPTH) {
        MP_WARN(config, "WARNING: Profile inclusion too deep.\n");
        return M_OPT_UNKNOWN;
    }
    config->profile_depth++;
    for (int i = 0; i < p->num_opts; i++) {
        m_config_set_option_ext(config,
                                bstr0(p->opts[2 * i]),
                                bstr0(p->opts[2 * i + 1]),
                                flags | M_SETOPT_FROM_CONFIG_FILE);
    }
    config->profile_depth--;

    return 0;
}

struct mpv_node m_config_get_profiles(struct m_config *config)
{
    struct mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_ARRAY, NULL);

    for (m_profile_t *profile = config->profiles; profile; profile = profile->next)
    {
        struct mpv_node *entry = node_array_add(&root, MPV_FORMAT_NODE_MAP);

        node_map_add_string(entry, "name", profile->name);
        if (profile->desc)
            node_map_add_string(entry, "profile-desc", profile->desc);

        struct mpv_node *opts =
            node_map_add(entry, "options", MPV_FORMAT_NODE_ARRAY);

        for (int n = 0; n < profile->num_opts; n++) {
            struct mpv_node *opt_entry = node_array_add(opts, MPV_FORMAT_NODE_MAP);
            node_map_add_string(opt_entry, "key", profile->opts[n * 2 + 0]);
            if (profile->opts[n * 2 + 1])
                node_map_add_string(opt_entry, "value", profile->opts[n * 2 + 1]);
        }
    }

    return root;
}

void *m_config_alloc_struct(void *talloc_ctx,
                            const struct m_sub_options *subopts)
{
    void *substruct = talloc_zero_size(talloc_ctx, subopts->size);
    if (subopts->defaults)
        memcpy(substruct, subopts->defaults, subopts->size);
    return substruct;
}

struct dtor_info {
    const struct m_sub_options *opts;
    void *ptr;
};

static void free_substruct(void *ptr)
{
    struct dtor_info *d = ptr;
    for (int n = 0; d->opts->opts && d->opts->opts[n].type; n++) {
        const struct m_option *opt = &d->opts->opts[n];
        void *dst = (char *)d->ptr + opt->offset;
        m_option_free(opt, dst);
    }
}

// Passing ptr==NULL initializes it from proper defaults.
void *m_sub_options_copy(void *talloc_ctx, const struct m_sub_options *opts,
                         const void *ptr)
{
    void *new = m_config_alloc_struct(talloc_ctx, opts);
    struct dtor_info *dtor = talloc_ptrtype(new, dtor);
    *dtor = (struct dtor_info){opts, new};
    talloc_set_destructor(dtor, free_substruct);
    for (int n = 0; opts->opts && opts->opts[n].type; n++) {
        const struct m_option *opt = &opts->opts[n];
        if (opt->offset < 0)
            continue;
        void *src = ptr ? (char *)ptr + opt->offset : NULL;
        void *dst = (char *)new + opt->offset;
        if (opt->type->flags  & M_OPT_TYPE_HAS_CHILD) {
            // Specifying a default struct for a sub-option field in the
            // containing struct's default struct is not supported here.
            // (Out of laziness. Could possibly be supported.)
            assert(!substruct_read_ptr(dst));

            const struct m_sub_options *subopts = opt->priv;

            const void *sub_src = NULL;
            if (src)
                sub_src = substruct_read_ptr(src);
            if (!sub_src)
                sub_src = subopts->defaults;

            void *sub_dst = m_sub_options_copy(new, subopts, sub_src);
            substruct_write_ptr(dst, sub_dst);
        } else {
            init_opt_inplace(opt, dst, src);
        }
    }
    return new;
}

struct m_config *m_config_dup(void *talloc_ctx, struct m_config *config)
{
    struct m_config *new = m_config_new(talloc_ctx, config->log, config->size,
                                        config->defaults, config->options);
    assert(new->num_opts == config->num_opts);
    for (int n = 0; n < new->num_opts; n++) {
        assert(new->opts[n].opt->type == config->opts[n].opt->type);
        m_option_copy(new->opts[n].opt, new->opts[n].data, config->opts[n].data);
    }
    return new;
}
