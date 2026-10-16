/*
 * extract_lombok.c — the members Lombok generates, as Java definitions.
 *
 * Lombok writes accessors and builders into the class file at compile time,
 * so the source has no declaration for `invoice.getNumber()` or
 * `Invoice.builder().number(..).build()`. Without one, those calls bind by
 * name to any project method that happens to share it. This file adds a
 * Method definition for each generated member, next to the field (or class)
 * that produces it, so the cross-file resolver can type the chain:
 *
 *   @Getter/@Setter (class or field), @Data, @Value  → getX/isX, setX
 *   @With (class or field)                           → withX
 *   @Builder, @SuperBuilder (class)                  → builder(), toBuilder(),
 *                                                      XBuilder.<field>(..),
 *                                                      XBuilder.build()
 *   @Accessors(fluent = true / chain = true)         → names and returns
 *
 * Each generated node carries `lombok` (the annotation) and, when it comes
 * from one field, `field` (that field's QN), and spans the field's lines, so
 * a jump to the getter lands on the field. A member the class already
 * declares by name is not generated, as Lombok itself skips it. Lombok on
 * constructors and methods (@Builder on a static factory) is not modelled.
 *
 * An annotation is Lombok's only when the file says so: `@lombok.Getter`, or a
 * bare `@Getter` with `import lombok.Getter;` or `import lombok.*;` — Spring's
 * `@Value("${...}")` generates nothing. The generated methods are
 * CBMDefinition.is_generated: the registry files them by QN only, so a call
 * reaches one through the type-aware resolver and never by a bare-name guess.
 */
#include "cbm.h"
#include "arena.h"
#include "helpers.h"
#include "tree_sitter/api.h"
#include "foundation/constants.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

enum { LOMBOK_PROPS = 5, LOMBOK_MAX_PREFIXES = 4 };

typedef struct {
    bool getter, setter, with, builder, to_builder, fluent, chain;
    bool getter_none, setter_none, with_none; /* AccessLevel.NONE */
    bool accessors;                /* @Accessors present: a field's replaces its class's */
    bool builder_default, non_final; /* @Builder.Default, @NonFinal (fields) */
    const char *prefix[LOMBOK_MAX_PREFIXES]; /* @Accessors(prefix = {"m", "f"}) */
    int nprefix;
    const char *builder_method;    /* builderMethodName, default "builder" */
    const char *build_method;      /* buildMethodName, default "build" */
    const char *builder_class;     /* builderClassName, default <Class>Builder */
    const char *setter_prefix;     /* @Builder(setterPrefix = "with") */
} lombok_flags_t;

typedef struct {
    const char *name;
    const char *type;
    const char *qn;
    uint32_t start_line;
    uint32_t end_line;
    bool is_static, is_final, has_init;
    lombok_flags_t own; /* field-level annotations */
} lombok_field_t;

static const char *lombok_skip_package(const char *p) {
    if (strncmp(p, "lombok.experimental.", strlen("lombok.experimental.")) == 0) {
        return p + strlen("lombok.experimental.");
    }
    if (strncmp(p, "lombok.", strlen("lombok.")) == 0) {
        return p + strlen("lombok.");
    }
    return NULL;
}

/* Whether the file's imports make the simple annotation name `name` (its
 * first `len` bytes) Lombok's: `import lombok.Getter;`, or `import lombok.*;`
 * with no single-type import of that name from elsewhere (Spring's
 * `org.springframework.beans.factory.annotation.Value`). Read from the tree:
 * definitions are extracted before the file's imports are. */
static bool lombok_imported(const CBMExtractCtx *ctx, const char *name, size_t len) {
    bool wildcard = false;
    uint32_t n = ts_node_named_child_count(ctx->root);
    for (uint32_t i = 0; i < n; i++) {
        TSNode imp = ts_node_named_child(ctx->root, i);
        if (strcmp(ts_node_type(imp), "import_declaration") != 0) {
            continue;
        }
        TSNode path = ts_node_named_child(imp, 0);
        bool star = false;
        uint32_t c = ts_node_child_count(imp);
        for (uint32_t k = 0; k < c; k++) {
            star |= strcmp(ts_node_type(ts_node_child(imp, k)), "asterisk") == 0;
        }
        if (ts_node_is_null(path)) {
            continue;
        }
        const char *text = ctx->source + ts_node_start_byte(path);
        size_t tlen = ts_node_end_byte(path) - ts_node_start_byte(path);
        if (star) {
            wildcard |= (tlen == strlen("lombok") && strncmp(text, "lombok", tlen) == 0) ||
                        (tlen == strlen("lombok.experimental") &&
                         strncmp(text, "lombok.experimental", tlen) == 0);
            continue;
        }
        if (tlen <= len || text[tlen - len - SKIP_ONE] != '.' ||
            strncmp(text + tlen - len, name, len) != 0) {
            continue;
        }
        size_t pkg = tlen - len - SKIP_ONE; /* the import of `name`: is it Lombok's? */
        return (pkg == strlen("lombok") && strncmp(text, "lombok", pkg) == 0) ||
               (pkg == strlen("lombok.experimental") &&
                strncmp(text, "lombok.experimental", pkg) == 0);
    }
    return wildcard;
}

/* "@lombok.Getter(AccessLevel.NONE)" → name "Getter", args "AccessLevel.NONE";
 * NULL when the annotation is not Lombok's: a bare name the file does not
 * import from `lombok`, or a name qualified by another package. */
static const char *lombok_annotation(const CBMExtractCtx *ctx, const char *text,
                                     const char **args) {
    CBMArena *a = ctx->arena;
    *args = "";
    if (!text || text[0] != '@') {
        return NULL;
    }
    const char *p = text + SKIP_ONE;
    const char *end = p;
    while (*end && (isalnum((unsigned char)*end) || *end == '_' || *end == '.')) {
        end++;
    }
    const char *bare = lombok_skip_package(p);
    if (bare) {
        p = bare;
    } else {
        /* `Builder.Default` is a member of the imported `Builder`. */
        const char *dot = memchr(p, '.', (size_t)(end - p));
        size_t first = dot ? (size_t)(dot - p) : (size_t)(end - p);
        if (first == 0 || !isupper((unsigned char)p[0]) || !lombok_imported(ctx, p, first)) {
            return NULL;
        }
    }
    if (*end == '(') {
        const char *close = strrchr(end, ')');
        *args = close ? cbm_arena_strndup(a, end + SKIP_ONE, (size_t)(close - end - SKIP_ONE))
                      : end + SKIP_ONE;
    }
    return cbm_arena_strndup(a, p, (size_t)(end - p));
}

/* `key = true` / `key = false` inside an annotation's arguments: true when the
 * key is written, with its value in *value. */
static bool lombok_arg_bool(const char *args, const char *key, bool *value) {
    const char *k = strstr(args, key);
    if (!k) {
        return false;
    }
    const char *v = strchr(k, '=');
    if (!v) {
        return false;
    }
    v++;
    while (*v == ' ') {
        v++;
    }
    *value = strncmp(v, "true", strlen("true")) == 0;
    return true;
}

static bool lombok_arg_true(const char *args, const char *key) {
    bool value = false;
    return lombok_arg_bool(args, key, &value) && value;
}

/* `prefix = "m"` or `prefix = {"m", "f"}` → the quoted strings, in order. */
static void lombok_arg_prefixes(CBMArena *a, const char *args, lombok_flags_t *f) {
    const char *k = strstr(args, "prefix");
    const char *v = k ? strchr(k, '=') : NULL;
    if (!v) {
        return;
    }
    v++;
    while (*v == ' ') {
        v++;
    }
    bool list = *v == '{';
    const char *stop = list ? strchr(v, '}') : NULL;
    for (const char *q = strchr(v, '"'); q && (!stop || q < stop); q = strchr(q, '"')) {
        const char *e = strchr(q + SKIP_ONE, '"');
        if (!e || f->nprefix == LOMBOK_MAX_PREFIXES) {
            break;
        }
        f->prefix[f->nprefix++] = cbm_arena_strndup(a, q + SKIP_ONE, (size_t)(e - q - SKIP_ONE));
        q = e + SKIP_ONE;
        if (!list) {
            break;
        }
    }
}

/* `key = "value"` inside an annotation's arguments, or NULL. */
static const char *lombok_arg_string(CBMArena *a, const char *args, const char *key) {
    const char *k = strstr(args, key);
    if (!k) {
        return NULL;
    }
    const char *q = strchr(k, '"');
    if (!q) {
        return NULL;
    }
    const char *e = strchr(q + SKIP_ONE, '"');
    if (!e || e == q + SKIP_ONE) {
        return NULL;
    }
    return cbm_arena_strndup(a, q + SKIP_ONE, (size_t)(e - q - SKIP_ONE));
}

/* One annotation's effect on `f`. On a field only the per-field annotations
 * count: Lombok has no field-level @Data, @Value or @Builder. */
static void lombok_read(const CBMExtractCtx *ctx, const char *text, lombok_flags_t *f,
                        bool on_field) {
    CBMArena *a = ctx->arena;
    const char *args = "";
    const char *name = lombok_annotation(ctx, text, &args);
    if (!name) {
        return;
    }
    if (on_field) {
        if (strcmp(name, "Builder.Default") == 0) {
            f->builder_default = true;
            return;
        }
        if (strcmp(name, "NonFinal") == 0) {
            f->non_final = true;
            return;
        }
        if (strcmp(name, "Getter") != 0 && strcmp(name, "Setter") != 0 &&
            strcmp(name, "With") != 0 && strcmp(name, "Wither") != 0 &&
            strcmp(name, "Accessors") != 0) {
            return;
        }
    }
    bool none = strstr(args, "NONE") != NULL;
    if (strcmp(name, "Getter") == 0) {
        f->getter = !none;
        f->getter_none = none;
    } else if (strcmp(name, "Setter") == 0) {
        f->setter = !none;
        f->setter_none = none;
    } else if (strcmp(name, "Data") == 0) {
        f->getter = true;
        f->setter = true;
    } else if (strcmp(name, "Value") == 0) {
        f->getter = true;
    } else if (strcmp(name, "With") == 0 || strcmp(name, "Wither") == 0) {
        f->with = !none;
        f->with_none = none;
    } else if (strcmp(name, "Builder") == 0 || strcmp(name, "SuperBuilder") == 0) {
        f->builder = true;
        f->to_builder = lombok_arg_true(args, "toBuilder");
        f->builder_method = lombok_arg_string(a, args, "builderMethodName");
        f->build_method = lombok_arg_string(a, args, "buildMethodName");
        f->builder_class = lombok_arg_string(a, args, "builderClassName");
        f->setter_prefix = lombok_arg_string(a, args, "setterPrefix");
    } else if (strcmp(name, "Accessors") == 0) {
        /* `chain` defaults to `fluent`, unless written out. */
        f->accessors = true;
        f->fluent = lombok_arg_true(args, "fluent");
        bool chain = false;
        f->chain = lombok_arg_bool(args, "chain", &chain) ? chain : f->fluent;
        lombok_arg_prefixes(a, args, f);
    }
}

static bool lombok_is_value(const CBMExtractCtx *ctx, const char **decorators) {
    for (int i = 0; decorators && decorators[i]; i++) {
        const char *args = "";
        const char *name = lombok_annotation(ctx, decorators[i], &args);
        if (name && strcmp(name, "Value") == 0) {
            return true;
        }
    }
    return false;
}

static const char *lombok_cap(CBMArena *a, const char *prefix, const char *name) {
    char *s = cbm_arena_sprintf(a, "%s%s", prefix, name);
    size_t pl = strlen(prefix);
    if (pl > 0 && s[pl]) {
        s[pl] = (char)toupper((unsigned char)s[pl]);
    }
    return s;
}

/* `isActive` on a primitive boolean keeps its name as the getter and drops
 * the `is` for setX/withX, as Lombok does. */
static bool lombok_is_prefixed(const lombok_field_t *fl) {
    return strcmp(fl->type, "boolean") == 0 && strncmp(fl->name, "is", 2) == 0 &&
           isupper((unsigned char)fl->name[2]);
}

static const char *lombok_stem(const lombok_field_t *fl) {
    return lombok_is_prefixed(fl) ? fl->name + 2 : fl->name;
}

static bool lombok_declared(const CBMFileResult *r, int from, int to, const char *parent,
                            const char *name) {
    for (int i = from; i < to && i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->name && d->parent_class && d->label && strcmp(d->label, "Method") == 0 &&
            strcmp(d->name, name) == 0 && strcmp(d->parent_class, parent) == 0) {
            return true;
        }
    }
    return false;
}

static void lombok_push(CBMExtractCtx *ctx, const char *parent, const char *name,
                        const char *ret, const char *param_type, const char *param_name,
                        uint32_t start_line, uint32_t end_line, const char *annotation,
                        const char *field_qn, int declared_to) {
    CBMArena *a = ctx->arena;
    if (lombok_declared(ctx->result, 0, declared_to, parent, name)) {
        return;
    }
    CBMDefinition def;
    memset(&def, 0, sizeof(def));
    def.name = name;
    def.qualified_name = cbm_arena_sprintf(a, "%s.%s", parent, name);
    def.label = "Method";
    def.file_path = ctx->rel_path;
    def.parent_class = parent;
    def.return_type = ret;
    def.start_line = start_line;
    def.end_line = end_line;
    def.lines = (int)(end_line - start_line + SKIP_ONE);
    def.is_exported = true;
    def.is_generated = true;
    if (param_type) {
        const char **types = (const char **)cbm_arena_alloc(a, 2 * sizeof(*types));
        const char **names = (const char **)cbm_arena_alloc(a, 2 * sizeof(*names));
        types[0] = param_type;
        types[1] = NULL;
        names[0] = param_name;
        names[1] = NULL;
        def.param_types = types;
        def.param_names = names;
        def.signature_param_types = types;
        def.signature_param_count = 1;
        def.param_count = 1;
        def.signature = cbm_arena_sprintf(a, "(%s %s)", param_type, param_name);
    } else {
        def.signature = "()";
    }
    const char **kv = (const char **)cbm_arena_alloc(a, LOMBOK_PROPS * sizeof(*kv));
    int k = 0;
    kv[k++] = "lombok";
    kv[k++] = annotation;
    if (field_qn) {
        kv[k++] = "field";
        kv[k++] = field_qn;
    }
    kv[k] = NULL;
    def.extra_props = kv;
    cbm_defs_push(&ctx->result->defs, a, def);
}

static bool lombok_modifier(TSNode mods, const char *kind) {
    uint32_t n = ts_node_child_count(mods);
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(ts_node_type(ts_node_child(mods, i)), kind) == 0) {
            return true;
        }
    }
    return false;
}

static const char *lombok_field_qn(const CBMFileResult *r, int from, const char *class_qn,
                                   const char *name, CBMArena *a) {
    for (int i = from; i < r->defs.count; i++) {
        const CBMDefinition *d = &r->defs.items[i];
        if (d->label && strcmp(d->label, "Field") == 0 && d->name && d->parent_class &&
            strcmp(d->name, name) == 0 && strcmp(d->parent_class, class_qn) == 0) {
            return d->qualified_name;
        }
    }
    return cbm_arena_sprintf(a, "%s.%s", class_qn, name);
}

/* How many rows lombok_collect_fields can write for `body`. */
static int lombok_count_fields(TSNode body) {
    int n = 0;
    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++) {
        TSNode member = ts_node_named_child(body, i);
        const char *kind = ts_node_type(member);
        if (strcmp(kind, "enum_body_declarations") == 0) {
            n += lombok_count_fields(member);
        } else if (strcmp(kind, "field_declaration") == 0) {
            uint32_t mc = ts_node_named_child_count(member);
            for (uint32_t d = 0; d < mc; d++) {
                const char *dk = ts_node_type(ts_node_named_child(member, d));
                n += strcmp(dk, "variable_declarator") == 0;
            }
        }
    }
    return n;
}

/* A record's components, as the final fields Lombok sees ("@Builder record
 * R(String a)" generates R.RBuilder.a); none for a class. */
static int lombok_collect_components(CBMExtractCtx *ctx, TSNode class_node, const char *class_qn,
                                     int fields_from, lombok_field_t *out, int max) {
    CBMArena *a = ctx->arena;
    TSNode params = ts_node_child_by_field_name(class_node, TS_FIELD("parameters"));
    if (ts_node_is_null(params)) {
        return 0;
    }
    int n = 0;
    uint32_t count = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < count && n < max; i++) {
        TSNode param = ts_node_named_child(params, i);
        if (strcmp(ts_node_type(param), "formal_parameter") != 0) {
            continue;
        }
        TSNode type = ts_node_child_by_field_name(param, TS_FIELD("type"));
        TSNode name = ts_node_child_by_field_name(param, TS_FIELD("name"));
        if (ts_node_is_null(type) || ts_node_is_null(name)) {
            continue;
        }
        lombok_field_t fl;
        memset(&fl, 0, sizeof(fl));
        fl.type = cbm_node_text(a, type, ctx->source);
        fl.name = cbm_node_text(a, name, ctx->source);
        fl.start_line = ts_node_start_point(param).row + TS_LINE_OFFSET;
        fl.end_line = ts_node_end_point(param).row + TS_LINE_OFFSET;
        fl.is_final = true;
        fl.qn = lombok_field_qn(ctx->result, fields_from, class_qn, fl.name, a);
        out[n++] = fl;
    }
    return n;
}

/* The field declarations of a class or enum body, one row per declarator. */
static int lombok_collect_fields(CBMExtractCtx *ctx, TSNode body, const char *class_qn,
                                 int fields_from, lombok_field_t *out, int max) {
    CBMArena *a = ctx->arena;
    int n = 0;
    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count && n < max; i++) {
        TSNode member = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(member), "enum_body_declarations") == 0) {
            n += lombok_collect_fields(ctx, member, class_qn, fields_from, out + n, max - n);
            continue;
        }
        if (strcmp(ts_node_type(member), "field_declaration") != 0) {
            continue;
        }
        TSNode type = ts_node_child_by_field_name(member, TS_FIELD("type"));
        if (ts_node_is_null(type)) {
            continue;
        }
        lombok_field_t base;
        memset(&base, 0, sizeof(base));
        base.type = cbm_node_text(a, type, ctx->source);
        base.start_line = ts_node_start_point(member).row + TS_LINE_OFFSET;
        base.end_line = ts_node_end_point(member).row + TS_LINE_OFFSET;
        uint32_t mc = ts_node_child_count(member);
        for (uint32_t m = 0; m < mc; m++) {
            TSNode mods = ts_node_child(member, m);
            if (strcmp(ts_node_type(mods), "modifiers") != 0) {
                continue;
            }
            base.is_static = lombok_modifier(mods, "static");
            base.is_final = lombok_modifier(mods, "final");
            uint32_t ac = ts_node_named_child_count(mods);
            for (uint32_t k = 0; k < ac; k++) {
                TSNode ann = ts_node_named_child(mods, k);
                const char *kind = ts_node_type(ann);
                if (strcmp(kind, "marker_annotation") == 0 || strcmp(kind, "annotation") == 0) {
                    lombok_read(ctx, cbm_node_text(a, ann, ctx->source), &base.own, true);
                }
            }
        }
        for (uint32_t d = 0; d < mc && n < max; d++) {
            TSNode decl = ts_node_child(member, d);
            if (strcmp(ts_node_type(decl), "variable_declarator") != 0) {
                continue;
            }
            TSNode name = ts_node_child_by_field_name(decl, TS_FIELD("name"));
            if (ts_node_is_null(name)) {
                continue;
            }
            lombok_field_t fl = base;
            fl.name = cbm_node_text(a, name, ctx->source);
            fl.has_init = !ts_node_is_null(ts_node_child_by_field_name(decl, TS_FIELD("value")));
            fl.qn = lombok_field_qn(ctx->result, fields_from, class_qn, fl.name, a);
            out[n++] = fl;
        }
    }
    return n;
}

/* The field's name with its @Accessors prefix removed and the next letter
 * lowered, as Lombok does ("mName" → "name"); NULL when prefixes are set and
 * none matches, for which Lombok generates nothing. */
static const char *lombok_unprefixed(CBMArena *a, const lombok_flags_t *acc, const char *name) {
    if (acc->nprefix == 0) {
        return name;
    }
    for (int i = 0; i < acc->nprefix; i++) {
        const char *px = acc->prefix[i];
        size_t n = strlen(px);
        if (n == 0) {
            return name;
        }
        if (strncmp(name, px, n) != 0 || !name[n] ||
            (isalpha((unsigned char)px[n - SKIP_ONE]) && islower((unsigned char)name[n]))) {
            continue;
        }
        char *s = cbm_arena_strdup(a, name + n);
        s[0] = (char)tolower((unsigned char)s[0]);
        return s;
    }
    return NULL;
}

static void lombok_accessors(CBMExtractCtx *ctx, const char *class_qn, const char *class_name,
                             const lombok_flags_t *cls, const lombok_field_t *field,
                             int declared_to) {
    CBMArena *a = ctx->arena;
    const lombok_flags_t *acc = field->own.accessors ? &field->own : cls;
    lombok_field_t named = *field; /* the field as its accessors name it */
    named.name = lombok_unprefixed(a, acc, field->name);
    if (!named.name) {
        return;
    }
    const lombok_field_t *fl = &named;
    bool fluent = acc->fluent;
    bool chain = acc->chain;
    bool getter = fl->own.getter || (cls->getter && !fl->is_static && !fl->own.getter_none);
    bool setter = (fl->own.setter || (cls->setter && !fl->is_static && !fl->own.setter_none)) &&
                  !fl->is_final;
    bool with = (fl->own.with || (cls->with && !fl->own.with_none)) && !fl->is_static;
    bool boolean = strcmp(fl->type, "boolean") == 0;
    if (getter) {
        const char *name = fluent                     ? fl->name
                           : lombok_is_prefixed(fl)   ? fl->name
                           : boolean                  ? lombok_cap(a, "is", fl->name)
                                                      : lombok_cap(a, "get", fl->name);
        lombok_push(ctx, class_qn, name, fl->type, NULL, NULL, fl->start_line, fl->end_line,
                    "Getter", fl->qn, declared_to);
    }
    if (setter) {
        const char *name = fluent ? fl->name : lombok_cap(a, "set", lombok_stem(fl));
        lombok_push(ctx, class_qn, name, chain ? class_name : "void", fl->type, field->name,
                    fl->start_line, fl->end_line, "Setter", fl->qn, declared_to);
    }
    if (with) {
        lombok_push(ctx, class_qn, lombok_cap(a, "with", lombok_stem(fl)), class_name, fl->type,
                    field->name, fl->start_line, fl->end_line, "With", fl->qn, declared_to);
    }
}

static bool lombok_node_is(const CBMExtractCtx *ctx, TSNode node, const char *text) {
    uint32_t from = ts_node_start_byte(node);
    uint32_t to = ts_node_end_byte(node);
    return !ts_node_is_null(node) && strlen(text) == to - from &&
           strncmp(ctx->source + from, text, to - from) == 0;
}

/* The member of `body` of the given kind and name (a nested class, a method),
 * or a null node. */
static TSNode lombok_member(const CBMExtractCtx *ctx, TSNode body, const char *kind,
                            const char *name) {
    uint32_t n = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < n; i++) {
        TSNode m = ts_node_named_child(body, i);
        if (strcmp(ts_node_type(m), kind) == 0 &&
            lombok_node_is(ctx, ts_node_child_by_field_name(m, TS_FIELD("name")), name)) {
            return m;
        }
    }
    return (TSNode){0};
}

/* Whether the builder class the source declares itself (`public static class
 * InvoiceBuilder { ... }`, which Lombok fills in) already has this method. */
static bool lombok_own_builder_has(const CBMExtractCtx *ctx, TSNode own, const char *method) {
    if (ts_node_is_null(own)) {
        return false;
    }
    TSNode body = ts_node_child_by_field_name(own, TS_FIELD("body"));
    return !ts_node_is_null(body) &&
           !ts_node_is_null(lombok_member(ctx, body, "method_declaration", method));
}

static void lombok_builder(CBMExtractCtx *ctx, TSNode class_node, const char *class_qn,
                           const char *class_name, const lombok_flags_t *cls,
                           const lombok_field_t *fields, int nfields, int declared_to) {
    CBMArena *a = ctx->arena;
    const char *bname =
        cls->builder_class ? cls->builder_class : cbm_arena_sprintf(a, "%sBuilder", class_name);
    const char *bqn = cbm_arena_sprintf(a, "%s.%s", class_qn, bname);
    uint32_t start = ts_node_start_point(class_node).row + TS_LINE_OFFSET;
    uint32_t end = ts_node_end_point(class_node).row + TS_LINE_OFFSET;
    /* A builder class the source declares keeps its own definition; Lombok
     * adds only the members it lacks. */
    TSNode own = lombok_member(ctx, ts_node_child_by_field_name(class_node, TS_FIELD("body")),
                               "class_declaration", bname);
    if (ts_node_is_null(own)) {
        CBMDefinition b;
        memset(&b, 0, sizeof(b));
        b.name = bname;
        b.qualified_name = bqn;
        b.label = "Class";
        b.file_path = ctx->rel_path;
        b.start_line = start;
        b.end_line = start;
        b.lines = 1;
        b.is_exported = true;
        const char **kv = (const char **)cbm_arena_alloc(a, 3 * sizeof(*kv));
        kv[0] = "lombok";
        kv[1] = "Builder";
        kv[2] = NULL;
        b.extra_props = kv;
        cbm_defs_push(&ctx->result->defs, a, b);
    }

    lombok_push(ctx, class_qn, cls->builder_method ? cls->builder_method : "builder", bname,
                NULL, NULL, start, start, "Builder", NULL, declared_to);
    if (cls->to_builder) {
        lombok_push(ctx, class_qn, "toBuilder", bname, NULL, NULL, start, start, "Builder", NULL,
                    declared_to);
    }
    for (int i = 0; i < nfields; i++) {
        const lombok_field_t *fl = &fields[i];
        /* An initialised final field is fixed, unless @Builder.Default makes
         * its initialiser only the default. */
        if (fl->is_static || (fl->is_final && fl->has_init && !fl->own.builder_default)) {
            continue;
        }
        const char *name =
            cls->setter_prefix ? lombok_cap(a, cls->setter_prefix, fl->name) : fl->name;
        if (!lombok_own_builder_has(ctx, own, name)) {
            lombok_push(ctx, bqn, name, bname, fl->type, fl->name, fl->start_line, fl->end_line,
                        "Builder", fl->qn, 0);
        }
    }
    const char *build = cls->build_method ? cls->build_method : "build";
    if (!lombok_own_builder_has(ctx, own, build)) {
        lombok_push(ctx, bqn, build, class_name, NULL, NULL, start, end > start ? start : end,
                    "Builder", NULL, 0);
    }
}

void cbm_extract_lombok_members(CBMExtractCtx *ctx, TSNode class_node, const char *class_qn,
                                const char *class_name, const char **class_decorators,
                                int fields_from) {
    if (!ctx || ctx->language != CBM_LANG_JAVA || !class_qn || !class_name) {
        return;
    }
    TSNode body = ts_node_child_by_field_name(class_node, TS_FIELD("body"));
    if (ts_node_is_null(body)) {
        return;
    }
    lombok_flags_t cls;
    memset(&cls, 0, sizeof(cls));
    for (int i = 0; class_decorators && class_decorators[i]; i++) {
        lombok_read(ctx, class_decorators[i], &cls, false);
    }
    /* On the arena, not the stack: this runs once per nesting level of a
     * class, inside the definition walk's recursion. */
    TSNode params = ts_node_child_by_field_name(class_node, TS_FIELD("parameters"));
    int ncomponents = ts_node_is_null(params) ? 0 : (int)ts_node_named_child_count(params);
    int max = ncomponents + lombok_count_fields(body);
    lombok_field_t *fields = NULL;
    if (max > 0) {
        fields = (lombok_field_t *)cbm_arena_alloc(ctx->arena, (size_t)max * sizeof(*fields));
    }
    int nfields = 0;
    if (fields) {
        nfields = lombok_collect_components(ctx, class_node, class_qn, fields_from, fields, max);
        nfields += lombok_collect_fields(ctx, body, class_qn, fields_from, fields + nfields,
                                         max - nfields);
    }
    bool any_field = false;
    for (int i = 0; i < nfields; i++) {
        any_field |= fields[i].own.getter || fields[i].own.setter || fields[i].own.with;
    }
    if (!cls.getter && !cls.setter && !cls.with && !cls.builder && !any_field) {
        return;
    }
    bool value = lombok_is_value(ctx, class_decorators);
    if (value) {
        for (int i = 0; i < nfields; i++) {
            /* @Value makes every field final, but for one marked @NonFinal */
            fields[i].is_final = fields[i].is_final || !fields[i].own.non_final;
        }
    }
    /* The class's own methods precede everything generated here. */
    int declared_to = ctx->result->defs.count;
    for (int i = 0; i < nfields; i++) {
        lombok_accessors(ctx, class_qn, class_name, &cls, &fields[i], declared_to);
    }
    if (cls.builder) {
        lombok_builder(ctx, class_node, class_qn, class_name, &cls, fields, nfields, declared_to);
    }
}
