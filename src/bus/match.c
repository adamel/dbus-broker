/*
 * D-Bus Match Rules
 */

#include <c-dvar.h>
#include <c-list.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include "bus/match.h"
#include "dbus/address.h"
#include "dbus/protocol.h"
#include "util/error.h"

static bool match_key_equal(const char *key1, const char *key2, size_t n_key2) {
        if (strlen(key1) != n_key2)
                return false;

        return !strncmp(key1, key2, n_key2);
}

static int match_keys_assign(MatchKeys *keys, const char *key, size_t n_key, const char *value) {
        if (match_key_equal("type", key, n_key)) {
                if (keys->filter.type != DBUS_MESSAGE_TYPE_INVALID)
                        return MATCH_E_INVALID;

                if (strcmp(value, "signal") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_SIGNAL;
                else if (strcmp(value, "method_call") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_CALL;
                else if (strcmp(value, "method_return") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_METHOD_RETURN;
                else if (strcmp(value, "error") == 0)
                        keys->filter.type = DBUS_MESSAGE_TYPE_ERROR;
                else
                        return MATCH_E_INVALID;
        } else if (match_key_equal("sender", key, n_key)) {
                if (keys->sender)
                        return MATCH_E_INVALID;
                if (!dbus_validate_name(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->sender = value;
        } else if (match_key_equal("destination", key, n_key)) {
                if (keys->destination)
                        return MATCH_E_INVALID;
                if (!dbus_validate_name(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->destination = value;
        } else if (match_key_equal("interface", key, n_key)) {
                if (keys->filter.interface)
                        return MATCH_E_INVALID;
                if (!dbus_validate_interface(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->filter.interface = value;
        } else if (match_key_equal("member", key, n_key)) {
                if (keys->filter.member)
                        return MATCH_E_INVALID;
                if (!dbus_validate_member(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->filter.member = value;
        } else if (match_key_equal("path", key, n_key)) {
                if (keys->filter.path || keys->path_namespace)
                        return MATCH_E_INVALID;
                if (!c_dvar_is_path(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->filter.path = value;
        } else if (match_key_equal("path_namespace", key, n_key)) {
                if (keys->path_namespace || keys->filter.path)
                        return MATCH_E_INVALID;
                if (!c_dvar_is_path(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->path_namespace = value;
        } else if (match_key_equal("arg0namespace", key, n_key)) {
                if (keys->arg0namespace || keys->filter.args[0] || keys->filter.argpaths[0])
                        return MATCH_E_INVALID;
                if (!dbus_validate_namespace(value, strlen(value)))
                        return MATCH_E_INVALID;
                keys->arg0namespace = value;
        } else if (n_key >= strlen("arg") && match_key_equal("arg", key, strlen("arg"))) {
                unsigned int i = 0;

                key += strlen("arg");
                n_key -= strlen("arg");

                for (unsigned int j = 0; j < 2 && n_key; ++j, ++key, --n_key) {
                        if (*key < '0' || *key > '9')
                                break;

                        i = i * 10 + *key - '0';
                }

                if (i == 0 && keys->arg0namespace)
                        return MATCH_E_INVALID;
                if (i > 63)
                        return MATCH_E_INVALID;

                if (keys->filter.args[i] || keys->filter.argpaths[i])
                        return MATCH_E_INVALID;

                if (match_key_equal("", key, n_key)) {
                        keys->filter.args[i] = value;
                        if (i + 1 > keys->filter.n_args)
                                keys->filter.n_args = i + 1;
                } else if (match_key_equal("path", key, n_key)) {
                        keys->filter.argpaths[i] = value;
                        if (i + 1 > keys->filter.n_argpaths)
                                keys->filter.n_argpaths = i + 1;
                } else
                        return MATCH_E_INVALID;
        } else {
                return MATCH_E_INVALID;
        }

        return 0;
}

static int match_copy_value(const char **match, char **p) {
        const char *m = *match;
        bool quoted = false;
        char c;

        /*
         * Within single quotes (apostrophe), a backslash represents itself,
         * and an apostrophe ends the quoted section. Outside single quotes, \'
         * (backslash, apostrophe) represents an apostrophe, and any backslash
         * not followed by an apostrophe represents itself.
         *
         * Note that this is quite counter-intuitive to everyone used to
         * shell-style quoting. However, we strictly follow the D-Bus
         * specification and reference-implementation here!
         */

        do {
                for ( ; *m == '\''; ++m)
                        quoted = !quoted;

                switch ((c = *m++)) {
                case 0:
                        --m; /* leave zero-terminating for caller */
                        break;
                case ',':
                        c = quoted ? ',' : 0;
                        break;
                case '\\':
                        c = (!quoted && *m == '\'') ? *m++ : '\\';
                        break;
                }


                **p = c;
        } while (*(*p)++);

        if (quoted)
                return MATCH_E_INVALID;

        *match = m;
        return 0;
}

static int match_parse_key(const char **match, const char **keyp, size_t *n_keyp) {
        const char *key;
        size_t n_key = 0;

        /* skip any leading whitespace and stray equal signs */
        *match += strspn(*match, " \t\n\r=");
        if (!**match)
                return MATCH_E_EOF;

        /* skip over the key, recording its length */
        n_key = strcspn(*match, " \t\n\r=");
        key = *match;
        *match += n_key;
        if (!**match)
                return MATCH_E_INVALID;

        /* drop trailing whitespace */
        *match += strspn(*match, " \t\n\r");

        /* skip over the equals sign between the key and the value */
        if (**match != '=')
                return MATCH_E_INVALID;
        else
                ++*match;

        *keyp = key;
        *n_keyp = n_key;
        return 0;
}

static int match_keys_parse(MatchKeys *keys, const char *string) {
        const char *key, *value;
        size_t n_key;
        char *p;
        int r;

        /*
         * Parse the rule-string @string into @keys. We repeatedly pop off a
         * key from @string and copy over the value into @keys, remembering the
         * pointer to it. If anything fails, we simply bail out.
         *
         * Note that we rely on @string to be zero-terminated!
         */

        p = keys->buffer;

        for (;;) {
                r = match_parse_key(&string, &key, &n_key);
                if (r)
                        break;

                value = p;
                r = match_copy_value(&string, &p);
                if (r)
                        break;

                r = match_keys_assign(keys, key, n_key, value);
                if (r)
                        break;
        }

        return (r == MATCH_E_EOF) ? 0 : error_trace(r);
}

static void match_keys_deinit(MatchKeys *keys) {
        *keys = (MatchKeys)MATCH_KEYS_NULL;
}

C_DEFINE_CLEANUP(MatchKeys *, match_keys_deinit);

static int match_keys_init(MatchKeys *k, const char *string, size_t n_string) {
        _c_cleanup_(match_keys_deinitp) MatchKeys *keys = k;
        int r;

        assert(n_string > 0);
        assert(n_string - 1 <= MATCH_RULE_LENGTH_MAX);

        *keys = (MatchKeys)MATCH_KEYS_NULL;

        r = match_keys_parse(keys, string);
        if (r)
                return error_trace(r);

        keys = NULL;
        return 0;
}

static MatchKeys *match_keys_free(MatchKeys *keys) {
        if (keys) {
                match_keys_deinit(keys);
                free(keys);
        }
        return NULL;
}

C_DEFINE_CLEANUP(MatchKeys *, match_keys_free);

static int match_keys_new(MatchKeys **keysp, const char *string) {
        _c_cleanup_(match_keys_freep) MatchKeys *keys = NULL;
        size_t n_string;
        int r;

        n_string = strlen(string) + 1;
        if (n_string - 1 > MATCH_RULE_LENGTH_MAX)
                return MATCH_E_INVALID;

        keys = calloc(1, sizeof(*keys) + n_string);
        if (!keys)
                return error_origin(-ENOMEM);

        r = match_keys_init(keys, string, n_string);
        if (r)
                return error_trace(r);

        *keysp = keys;
        keys = NULL;
        return 0;
}

static bool match_string_prefix(const char *string, const char *prefix, char delimiter, bool delimiter_included) {
        char *tail;

        if (string == prefix)
                return true;

        if (!string || !prefix)
                return false;

        tail = c_string_prefix(string, prefix);
        if (!tail)
                return false;

        if (delimiter_included) {
                if (tail == string || (*tail != '\0' && *(tail - 1) != delimiter))
                        return false;
        } else {
                if (*tail != '\0' && *tail != delimiter)
                        return false;
        }

        return true;
}

static bool match_keys_match_filter(MatchKeys *keys, MatchFilter *filter) {
        if (keys->filter.interface && !c_string_equal(keys->filter.interface, filter->interface))
                return false;

        if (keys->filter.member && !c_string_equal(keys->filter.member, filter->member))
                return false;

        if (keys->filter.path && !c_string_equal(keys->filter.path, filter->path))
                return false;

        if (keys->path_namespace && !match_string_prefix(filter->path, keys->path_namespace, '/', false))
                return false;

        if (keys->arg0namespace && !match_string_prefix(filter->args[0], keys->arg0namespace, '.', false))
                return false;

        if (keys->filter.n_args > filter->n_args)
                return false;

        for (unsigned int i = 0; i < keys->filter.n_args || i < keys->filter.n_argpaths; i ++) {
                if (keys->filter.args[i] && !c_string_equal(keys->filter.args[i], filter->args[i]))
                        return false;

                if (keys->filter.argpaths[i]) {
                        if (!match_string_prefix(filter->argpaths[i], keys->filter.argpaths[i], '/', true) &&
                            !match_string_prefix(keys->filter.argpaths[i], filter->argpaths[i], '/', true))
                                return false;
                }
        }

        if (keys->filter.type != DBUS_MESSAGE_TYPE_INVALID && keys->filter.type != filter->type)
                return false;

        if (keys->filter.sender != ADDRESS_ID_INVALID && keys->filter.sender != filter->sender)
                return false;

        return true;
}

static int match_registry_by_path_compare(CRBTree *tree, void *k, CRBNode *rb) {
        MatchRegistryByPath *registry = c_container_of(rb, MatchRegistryByPath, registry_node);
        const char *path1 = k ?: "", *path2 = registry->path;

        return strcmp(path1, path2);
}

static int match_registry_by_path_new(MatchRegistryByPath **registryp, const char *path) {
        MatchRegistryByPath *registry;
        size_t n_path;

        if (!path)
                path = "";

        n_path = strlen(path);

        registry = malloc(sizeof(*registry) + n_path + 1);
        if (!registry)
                return error_origin(-ENOMEM);

        *registry = (MatchRegistryByPath)MATCH_REGISTRY_BY_PATH_INIT(*registry);
        strcpy(registry->path, path);

        *registryp = registry;
        return 0;
}

static MatchRegistryByPath *match_registry_by_path_ref(MatchRegistryByPath *registry) {
        if (!registry)
                return NULL;

        assert(registry->n_refs > 0);

        ++registry->n_refs;

        return registry;
}

static MatchRegistryByPath *match_registry_by_path_unref(MatchRegistryByPath *registry) {
        if (!registry || --registry->n_refs > 0)
                return NULL;

        assert(c_rbtree_is_empty(&registry->interface_tree));

        c_rbnode_unlink(&registry->registry_node);
        free(registry);

        return NULL;
}

C_DEFINE_CLEANUP(MatchRegistryByPath *, match_registry_by_path_unref);

static void match_registry_by_path_link(MatchRegistryByPath *registry, CRBTree *tree, CRBNode *parent, CRBNode **slot) {
        c_rbtree_add(tree, parent, slot, &registry->registry_node);
}

static int match_registry_by_interface_compare(CRBTree *tree, void *k, CRBNode *rb) {
        MatchRegistryByInterface *registry = c_container_of(rb, MatchRegistryByInterface, registry_node);
        const char *interface1 = k ?: "", *interface2 = registry->interface;

        return strcmp(interface1, interface2);
}

static int match_registry_by_interface_new(MatchRegistryByInterface **registryp, const char *interface) {
        MatchRegistryByInterface *registry;
        size_t n_interface;

        if (!interface)
                interface = "";

        n_interface = strlen(interface);

        registry = malloc(sizeof(*registry) + n_interface + 1);
        if (!registry)
                return error_origin(-ENOMEM);

        *registry = (MatchRegistryByInterface)MATCH_REGISTRY_BY_INTERFACE_INIT(*registry);
        strcpy(registry->interface, interface);

        *registryp = registry;
        return 0;
}

static MatchRegistryByInterface *match_registry_by_interface_ref(MatchRegistryByInterface *registry) {
        if (!registry)
                return NULL;

        assert(registry->n_refs > 0);

        ++registry->n_refs;

        return registry;
}

static MatchRegistryByInterface *match_registry_by_interface_unref(MatchRegistryByInterface *registry) {
        if (!registry || --registry->n_refs > 0)
                return NULL;

        assert(c_list_is_empty(&registry->rule_list));

        c_rbnode_unlink(&registry->registry_node);
        match_registry_by_path_unref(registry->registry_by_path);
        free(registry);

        return NULL;
}

C_DEFINE_CLEANUP(MatchRegistryByInterface *, match_registry_by_interface_unref);

static void match_registry_by_interface_link(MatchRegistryByInterface *registry, MatchRegistryByPath *registry_by_path, CRBNode *parent, CRBNode **slot) {
        c_rbtree_add(&registry_by_path->interface_tree, parent, slot, &registry->registry_node);
        registry->registry_by_path = match_registry_by_path_ref(registry_by_path);
}

static int match_rule_compare(CRBTree *tree, void *k, CRBNode *rb) {
        MatchRule *rule = c_container_of(rb, MatchRule, owner_node);
        MatchKeys *key1 = k, *key2 = &rule->keys;
        int r;

        if ((r = c_string_compare(key1->sender, key2->sender)) ||
            (r = c_string_compare(key1->destination, key2->destination)) ||
            (r = c_string_compare(key1->filter.interface, key2->filter.interface)) ||
            (r = c_string_compare(key1->filter.member, key2->filter.member)) ||
            (r = c_string_compare(key1->filter.path, key2->filter.path)) ||
            (r = c_string_compare(key1->path_namespace, key2->path_namespace)) ||
            (r = c_string_compare(key1->arg0namespace, key2->arg0namespace)))
                return r;

        if (key1->filter.type > key2->filter.type)
                return 1;
        if (key1->filter.type < key2->filter.type)
                return -1;

        if (key1->filter.n_args < key2->filter.n_args)
                return -1;
        if (key1->filter.n_args > key2->filter.n_args)
                return 1;

        if (key1->filter.n_argpaths < key2->filter.n_argpaths)
                return -1;
        if (key1->filter.n_argpaths > key2->filter.n_argpaths)
                return 1;

        for (size_t i = 0; i < key1->filter.n_args || i < key1->filter.n_argpaths; i ++) {
                if ((r = c_string_compare(key1->filter.args[i], key2->filter.args[i])) ||
                    (r = c_string_compare(key1->filter.argpaths[i], key2->filter.argpaths[i])))
                        return r;
        }

        return 0;
}

static MatchRule *match_rule_free(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(!rule->n_user_refs);

        match_keys_deinit(&rule->keys);
        user_charge_deinit(&rule->charge[1]);
        user_charge_deinit(&rule->charge[0]);
        c_rbnode_unlink(&rule->owner_node);
        match_rule_unlink(rule);
        free(rule);

        return NULL;
}

C_DEFINE_CLEANUP(MatchRule *, match_rule_free);

static int match_rule_new(MatchRule **rulep, MatchOwner *owner, User *user, const char *string) {
        _c_cleanup_(match_rule_freep) MatchRule *rule = NULL;
        size_t n_string;
        int r;

        n_string = strlen(string) + 1;
        if (n_string - 1 > MATCH_RULE_LENGTH_MAX)
                return MATCH_E_INVALID;

        rule = calloc(1, sizeof(*rule) + n_string);
        if (!rule)
                return error_origin(-ENOMEM);

        *rule = (MatchRule)MATCH_RULE_NULL(*rule);
        rule->owner = owner;

        r = user_charge(user, &rule->charge[0], NULL, USER_SLOT_BYTES, sizeof(*rule) + n_string);
        r = r ?: user_charge(user, &rule->charge[1], NULL, USER_SLOT_MATCHES, 1);
        if (r)
                return (r == USER_E_QUOTA) ? MATCH_E_QUOTA : error_fold(r);

        r = match_keys_init(&rule->keys, string, n_string);
        if (r)
                return error_trace(r);

        *rulep = rule;
        rule = NULL;
        return 0;
}

/**
 * match_rule_user_ref() - XXX
 */
MatchRule *match_rule_user_ref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        ++rule->n_user_refs;

        return rule;
}

/**
 * match_rule_user_unref() - XXX
 */
MatchRule *match_rule_user_unref(MatchRule *rule) {
        if (!rule)
                return NULL;

        assert(rule->n_user_refs > 0);

        --rule->n_user_refs;

        if (rule->n_user_refs == 0)
                match_rule_free(rule);

        return NULL;
}

static int match_rule_link_by_interface(MatchRule *rule, MatchRegistryByInterface *registry) {
        c_list_link_tail(&registry->rule_list, &rule->registry_link);
        rule->registry_by_interface = match_registry_by_interface_ref(registry);

        return 0;
}

static int match_rule_link_by_path(MatchRule *rule, MatchRegistryByPath *registry) {
        _c_cleanup_(match_registry_by_interface_unrefp) MatchRegistryByInterface *registry_by_interface = NULL;
        CRBNode **slot, *parent;
        int r;

        slot = c_rbtree_find_slot(&registry->interface_tree, match_registry_by_interface_compare, rule->keys.filter.interface, &parent);
        if (!slot) {
                registry_by_interface = match_registry_by_interface_ref(c_rbnode_entry(parent, MatchRegistryByInterface, registry_node));
        } else {
                r = match_registry_by_interface_new(&registry_by_interface, rule->keys.filter.interface);
                if (r)
                        return error_trace(r);

                match_registry_by_interface_link(registry_by_interface, registry, parent, slot);
        }

        r = match_rule_link_by_interface(rule, registry_by_interface);
        if (r)
                return error_trace(r);

        return 0;
}

/**
 * match_rule_link() - XXX
 */
int match_rule_link(MatchRule *rule, MatchRegistry *registry, bool monitor) {
        _c_cleanup_(match_registry_by_path_unrefp) MatchRegistryByPath *registry_by_path = NULL;
        CRBTree *tree;
        CRBNode **slot, *parent;
        int r;

        if (rule->registry) {
                assert(registry == rule->registry);
                assert(c_list_is_linked(&rule->registry_link));

                return 0;
        }

        if (monitor)
                tree = &registry->monitor_tree;
        else
                tree = &registry->subscription_tree;

        slot = c_rbtree_find_slot(tree, match_registry_by_path_compare, rule->keys.filter.path, &parent);
        if (!slot) {
                registry_by_path = match_registry_by_path_ref(c_rbnode_entry(parent, MatchRegistryByPath, registry_node));
        } else {
                r = match_registry_by_path_new(&registry_by_path, rule->keys.filter.path);
                if (r)
                        return error_trace(r);

                match_registry_by_path_link(registry_by_path, tree, parent, slot);
        }

        r = match_rule_link_by_path(rule, registry_by_path);
        if (r)
                return error_trace(r);
        rule->registry = registry;

        return 0;
}

/**
 * match_rule_unlink() - XXX
 */
void match_rule_unlink(MatchRule *rule) {
        if (rule->registry) {
                c_list_unlink(&rule->registry_link);
                rule->registry_by_interface = match_registry_by_interface_unref(rule->registry_by_interface);
                rule->registry = NULL;
        }
}

bool match_rule_match_filter(MatchRule *rule, MatchFilter *filter) {
        return match_keys_match_filter(&rule->keys, filter);
}

MatchRule *match_rule_next_match_by_interface(MatchRegistryByInterface *registry, MatchRule *rule, MatchFilter *filter) {
        if (!registry)
                return NULL;

        c_list_for_each_entry_continue(rule, &registry->rule_list, registry_link)
                if (match_rule_match_filter(rule, filter))
                        return rule;

        return NULL;
}

static MatchRule *match_rule_next_match_by_path(MatchRegistryByPath *registry, MatchRule *rule, MatchFilter *filter) {
        MatchRegistryByInterface *registry_by_interface;
        bool wildcard;

        if (!registry)
                return NULL;

        if (rule) {
                registry_by_interface = rule->registry_by_interface;
                if (registry_by_interface->interface[0] == '\0')
                        wildcard = true;
                else
                        wildcard = false;
        } else {
                registry_by_interface = c_rbtree_find_entry(&registry->interface_tree, match_registry_by_interface_compare, NULL, MatchRegistryByInterface, registry_node);
                wildcard = true;
        }

        rule = match_rule_next_match_by_interface(registry_by_interface, rule, filter);
        if (!rule && wildcard && filter->interface) {
                registry_by_interface = c_rbtree_find_entry(&registry->interface_tree, match_registry_by_interface_compare, filter->interface, MatchRegistryByInterface, registry_node);
                rule = match_rule_next_match_by_interface(registry_by_interface, NULL, filter);
        }

        return rule;
}

static MatchRule *match_rule_next_match(CRBTree *tree, MatchRule *rule, MatchFilter *filter) {
        MatchRegistryByPath *registry_by_path;
        bool wildcard;

        if (rule) {
                registry_by_path = rule->registry_by_interface->registry_by_path;
                if (registry_by_path->path[0] == '\0')
                        wildcard = true;
                else
                        wildcard = false;
        } else {
                registry_by_path = c_rbtree_find_entry(tree, match_registry_by_path_compare, NULL, MatchRegistryByPath, registry_node);
                wildcard = true;
        }

        rule = match_rule_next_match_by_path(registry_by_path, rule, filter);
        if (!rule && wildcard && filter->path) {
                registry_by_path = c_rbtree_find_entry(tree, match_registry_by_path_compare, filter->path, MatchRegistryByPath, registry_node);
                rule = match_rule_next_match_by_path(registry_by_path, NULL, filter);
        }

        return rule;
}

MatchRule *match_rule_next_subscription_match(MatchRegistry *registry, MatchRule *rule, MatchFilter *filter) {
        return match_rule_next_match(&registry->subscription_tree, rule, filter);
}

MatchRule *match_rule_next_monitor_match(MatchRegistry *registry, MatchRule *rule, MatchFilter *filter) {
        return match_rule_next_match(&registry->monitor_tree, rule, filter);
}

/**
 * match_owner_init() - XXX
 */
void match_owner_init(MatchOwner *owner) {
        *owner = (MatchOwner)MATCH_OWNER_INIT;
}

/**
 * match_owner_deinit() - XXX
 */
void match_owner_deinit(MatchOwner *owner) {
        assert(c_rbtree_is_empty(&owner->rule_tree));
}

/**
 * match_owner_move() - XXX
 */
void match_owner_move(MatchOwner *to, MatchOwner *from) {
        c_rbtree_move(&to->rule_tree, &from->rule_tree);
}

/**
 * match_owner_ref_rule() - XXX
 */
int match_owner_ref_rule(MatchOwner *owner, MatchRule **rulep, User *user, const char *rule_string) {
        CRBNode **slot, *parent;
        MatchRule *rule;
        int r;

        r = match_rule_new(&rule, owner, user, rule_string);
        if (r)
                return error_trace(r);

        ++rule->n_user_refs;

        slot = c_rbtree_find_slot(&owner->rule_tree, match_rule_compare, &rule->keys, &parent);
        if (!slot) {
                /* one already exists, take a ref on that instead and drop the one we created */
                match_rule_user_unref(rule);
                rule = match_rule_user_ref(c_container_of(parent, MatchRule, owner_node));
        } else {
                /* link the new rule into the rbtree */
                c_rbtree_add(&owner->rule_tree, parent, slot, &rule->owner_node);
        }

        if (rulep)
                *rulep = rule;
        return 0;
}

/**
 * match_owner_find_rule() - XXX
 */
int match_owner_find_rule(MatchOwner *owner, MatchRule **rulep, const char *rule_string) {
        _c_cleanup_(match_keys_freep) MatchKeys *keys = NULL;
        int r;

        r = match_keys_new(&keys, rule_string);
        if (r)
                return error_trace(r);

        *rulep = c_rbtree_find_entry(&owner->rule_tree, match_rule_compare, keys, MatchRule, owner_node);
        return 0;
}

/**
 * match_registry_init() - XXX
 */
void match_registry_init(MatchRegistry *registry) {
        *registry = (MatchRegistry)MATCH_REGISTRY_INIT(*registry);
}

/**
 * match_registry_deinit() - XXX
 */
void match_registry_deinit(MatchRegistry *registry) {
        assert(c_rbtree_is_empty(&registry->subscription_tree));
        assert(c_rbtree_is_empty(&registry->monitor_tree));
}

static void match_registry_by_interface_flush(MatchRegistryByInterface *registry) {
        MatchRule *rule, *rule_safe;

        c_list_for_each_entry_safe(rule, rule_safe, &registry->rule_list, registry_link)
                match_rule_unlink(rule);
}

static void match_registry_by_path_flush(MatchRegistryByPath *registry) {
        MatchRegistryByInterface *registry_by_interface, *registry_by_interface_safe;

        c_rbtree_for_each_entry_safe_postorder(registry_by_interface, registry_by_interface_safe, &registry->interface_tree, registry_node) {
                match_registry_by_interface_ref(registry_by_interface);
                match_registry_by_interface_flush(registry_by_interface);
                match_registry_by_interface_unref(registry_by_interface);
        }
}

/**
 * mach_registry_flush() - XXX
 */
void match_registry_flush(MatchRegistry *registry) {
        MatchRegistryByPath *registry_by_path, *registry_by_path_safe;

        c_rbtree_for_each_entry_safe_postorder(registry_by_path, registry_by_path_safe, &registry->subscription_tree, registry_node) {
                match_registry_by_path_ref(registry_by_path);
                match_registry_by_path_flush(registry_by_path);
                match_registry_by_path_unref(registry_by_path);
        }
}
