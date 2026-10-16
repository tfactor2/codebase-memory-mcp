/*
 * test_java_typed_calls.c — Java CALLS edges follow the receiver's type
 * through the whole pipeline (definitions → cross-file LSP → calls pass).
 *
 * One small project is indexed with cbm_pipeline_run and the CALLS edges of
 * InvoiceService are read back from the store. Each project method carries a
 * same-named decoy in an unrelated class (Filters), so a name-only guess is
 * visible as an edge to Filters. Covered:
 *   - Lombok @Data/@Getter/@Setter/@Value accessors and @Builder chains,
 *     @Accessors(prefix), @Builder.Default, a hand-written builder class, and
 *     Spring's @Value, which is not Lombok's;
 *   - receiver types of fields, parameters, `var x = new X()`, and the
 *     generic return of a project interface (Repo<Invoice>.first());
 *   - JDK and library receivers (Stream, org.jooq.DSLContext), whose calls
 *     get no edge instead of a project method of the same name;
 *   - a receiver of unknown type, whose call never guesses a Lombok member.
 * The same fixture runs on the sequential path and, padded past
 * MIN_FILES_FOR_PARALLEL, on the parallel one: both must agree.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "pipeline/pipeline.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Fixture ─────────────────────────────────────────────────────── */

static const char *const JT_INVOICE =
    "package com.acme.model;\n\n"
    "import lombok.Builder;\nimport lombok.Data;\n\n"
    "@Data\n@Builder\npublic class Invoice {\n"
    "    private String number;\n"
    "    private boolean paid;\n"
    "    private Customer customer;\n"
    "}\n";

static const char *const JT_CUSTOMER =
    "package com.acme.model;\n\n"
    "import lombok.Getter;\nimport lombok.Setter;\n\n"
    "public class Customer {\n"
    "    @Getter @Setter private String name;\n"
    "    @Getter private Address address;\n"
    "    private int hidden;\n\n"
    "    public String describe() { return name; }\n"
    "    public String describe(String prefix) { return prefix + name; }\n"
    "}\n";

static const char *const JT_ADDRESS = "package com.acme.model;\n\n"
                                      "import lombok.Value;\n\n"
                                      "@Value\npublic class Address {\n"
                                      "    String city;\n"
                                      "}\n";

static const char *const JT_REPO = "package com.acme.model;\n\n"
                                   "import java.util.List;\nimport java.util.Optional;\n\n"
                                   "public interface Repo<T> {\n"
                                   "    Optional<T> find(String id);\n"
                                   "    List<T> all();\n"
                                   "    T first();\n"
                                   "}\n";

static const char *const JT_BASE = "package com.acme.model;\n\n"
                                   "public abstract class Base {\n"
                                   "    public void audit() {}\n"
                                   "}\n";

static const char *const JT_SETTINGS = "package com.acme.model;\n\n"
                                       "import lombok.Getter;\n"
                                       "import lombok.experimental.Accessors;\n\n"
                                       "@Getter\n@Accessors(prefix = \"m\")\n"
                                       "public class Settings {\n"
                                       "    private String mLabel;\n"
                                       "    private String mode;\n"
                                       "}\n";

static const char *const JT_ORDER =
    "package com.acme.model;\n\n"
    "import lombok.Builder;\n\n"
    "@Builder\npublic class Order {\n"
    "    @Builder.Default private final String state = \"new\";\n"
    "    private final String id = \"x\";\n"
    "    private String note;\n\n"
    "    public static class OrderBuilder {\n"
    "        public OrderBuilder note(String n) { this.note = n; return this; }\n"
    "    }\n"
    "}\n";

/* @Builder on a record: the components are the builder's setters. */
static const char *const JT_BASKET = "package com.acme.model;\n\n"
                                     "import lombok.Builder;\n\n"
                                     "@Builder\npublic record Basket(String currency, Long amount) {\n"
                                     "    public static final Basket EMPTY = null;\n"
                                     "}\n";

/* Spring's @Value, with no Lombok import: no getter. */
static const char *const JT_CONFIG =
    "package com.acme.other;\n\n"
    "import org.springframework.beans.factory.annotation.Value;\n\n"
    "public class Config {\n"
    "    @Value(\"${retry.interval}\")\n"
    "    private long interval;\n"
    "}\n";

/* Every method here shares a name with a real target elsewhere. */
static const char *const JT_FILTERS = "package com.acme.other;\n\n"
                                      "public class Filters {\n"
                                      "    public Filters filter(Object p) { return this; }\n"
                                      "    public Filters map(Object f) { return this; }\n"
                                      "    public String getName() { return \"\"; }\n"
                                      "    public Object select(Object a) { return a; }\n"
                                      "    public Object build() { return null; }\n"
                                      "    public String getCity() { return \"\"; }\n"
                                      "    public int size() { return 0; }\n"
                                      "    public Filters number(String n) { return this; }\n"
                                      "    public void setPaid(boolean p) {}\n"
                                      "    public Object getCustomer() { return null; }\n"
                                      "}\n";

static const char *const JT_SERVICE =
    "package com.acme.svc;\n\n"
    "import com.acme.model.Base;\n"
    "import com.acme.model.Basket;\n"
    "import com.acme.model.Customer;\n"
    "import com.acme.model.Invoice;\n"
    "import com.acme.model.Order;\n"
    "import com.acme.model.Repo;\n"
    "import com.acme.model.Settings;\n"
    "import java.util.List;\n"
    "import java.util.stream.Collectors;\n"
    "import org.jooq.DSLContext;\n\n"
    "public class InvoiceService extends Base {\n"
    "    private final Repo<Invoice> invoices;\n"
    "    private final DSLContext dsl;\n\n"
    "    public InvoiceService(Repo<Invoice> invoices, DSLContext dsl) {\n"
    "        this.invoices = invoices;\n"
    "        this.dsl = dsl;\n"
    "    }\n\n"
    "    public String customerName(String id) {\n"
    "        return invoices.find(id).map(c -> c.getCustomer().getName()).orElse(\"\");\n"
    "    }\n\n"
    "    public String city() {\n"
    "        return invoices.first().getCustomer().getAddress().getCity();\n"
    "    }\n\n"
    "    public List<String> numbers() {\n"
    "        return invoices.all().stream().filter(i -> i.isPaid()).map(Invoice::getNumber)\n"
    "                .collect(Collectors.toList());\n"
    "    }\n\n"
    "    public Invoice make() {\n"
    "        return Invoice.builder().number(\"1\").paid(true).build();\n"
    "    }\n\n"
    "    public int touch(Customer c) {\n"
    "        var inv = new Invoice(\"x\", false, c);\n"
    "        inv.setPaid(true);\n"
    "        c.describe();\n"
    "        c.describe(\"p\");\n"
    "        audit();\n"
    "        dsl.select(\"a\");\n"
    "        return invoices.all().size();\n"
    "    }\n\n"
    "    public Order order() {\n"
    "        return Order.builder().state(\"a\").note(\"b\").build();\n"
    "    }\n\n"
    "    public Basket basket() {\n"
    "        return Basket.builder().currency(\"EUR\").amount(1L).build();\n"
    "    }\n\n"
    "    public String label(Settings s) {\n"
    "        return s.getLabel();\n"
    "    }\n\n"
    "    public void raw(java.util.List items) {\n"
    "        items.forEach(x -> x.getNumber());\n"
    "    }\n"
    "}\n";

static void jt_write_fixture(const char *tmp, int filler) {
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Invoice.java"), JT_INVOICE);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Customer.java"), JT_CUSTOMER);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Address.java"), JT_ADDRESS);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Repo.java"), JT_REPO);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Base.java"), JT_BASE);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Settings.java"), JT_SETTINGS);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Order.java"), JT_ORDER);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/model/Basket.java"), JT_BASKET);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/other/Config.java"), JT_CONFIG);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/other/Filters.java"), JT_FILTERS);
    th_write_file(TH_PATH(tmp, "src/main/java/com/acme/svc/InvoiceService.java"), JT_SERVICE);
    for (int i = 0; i < filler; i++) {
        char rel[128];
        char body[256];
        snprintf(rel, sizeof(rel), "src/main/java/com/acme/fill/Fill%d.java", i);
        snprintf(body, sizeof(body),
                 "package com.acme.fill;\n\npublic class Fill%d {\n"
                 "    public int value%d() { return %d; }\n}\n",
                 i, i, i);
        th_write_file(TH_PATH(tmp, rel), body);
    }
}

/* ── Store reads ─────────────────────────────────────────────────── */

typedef struct {
    char tmp[256];
    char db_path[512];
    char project[512];
    sqlite3 *db;
} jt_index_t;

static int jt_index(jt_index_t *ix, int filler) {
    memset(ix, 0, sizeof(*ix));
    snprintf(ix->tmp, sizeof(ix->tmp), "/tmp/cbm_jtyped_XXXXXX");
    if (!cbm_mkdtemp(ix->tmp)) {
        return -1;
    }
    jt_write_fixture(ix->tmp, filler);
    snprintf(ix->db_path, sizeof(ix->db_path), "%s/jt.db", ix->tmp);
    cbm_pipeline_t *p = cbm_pipeline_new(ix->tmp, ix->db_path, CBM_MODE_FULL);
    if (!p) {
        return -1;
    }
    int rc = cbm_pipeline_run(p);
    snprintf(ix->project, sizeof(ix->project), "%s", cbm_pipeline_project_name(p));
    cbm_pipeline_free(p);
    if (rc != 0 || sqlite3_open_v2(ix->db_path, &ix->db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        return -1;
    }
    return 0;
}

static void jt_close(jt_index_t *ix) {
    if (ix->db) {
        sqlite3_close(ix->db);
    }
    th_cleanup(ix->tmp);
}

/* Targets of the CALLS edges from method `caller` whose textual callee ends
 * in `.leaf` or is `leaf`, as "<qn-without-project>|<strategy>" lines. */
static int jt_targets(jt_index_t *ix, const char *caller, const char *leaf, char *out,
                      size_t out_len) {
    out[0] = '\0';
    const char *sql =
        "SELECT t.qualified_name, json_extract(e.properties,'$.strategy'), "
        "json_extract(e.properties,'$.callee') FROM edges e "
        "JOIN nodes s ON s.id = e.source_id JOIN nodes t ON t.id = e.target_id "
        "WHERE e.type = 'CALLS' AND s.name = ?1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ix->db, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, caller, -1, SQLITE_STATIC);
    int n = 0;
    size_t plen = strlen(ix->project);
    size_t llen = strlen(leaf);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        const char *strategy = (const char *)sqlite3_column_text(st, 1);
        const char *callee = (const char *)sqlite3_column_text(st, 2);
        if (!qn || !callee) {
            continue;
        }
        size_t clen = strlen(callee);
        bool leaf_match = (clen == llen && strcmp(callee, leaf) == 0) ||
                          (clen > llen && callee[clen - llen - 1] == '.' &&
                           strcmp(callee + clen - llen, leaf) == 0);
        if (!leaf_match) {
            continue;
        }
        if (strncmp(qn, ix->project, plen) == 0 && qn[plen] == '.') {
            qn += plen + 1;
        }
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, "%s|%s\n", qn, strategy ? strategy : "");
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* Exactly one CALLS edge for (caller, leaf), and its target QN ends in
 * `want_suffix`. Prints what the graph has on failure. */
static int jt_expect(jt_index_t *ix, const char *caller, const char *leaf,
                     const char *want_suffix) {
    char got[4096];
    int n = jt_targets(ix, caller, leaf, got, sizeof(got));
    if (n == 1) {
        const char *bar = strchr(got, '|');
        size_t qlen = bar ? (size_t)(bar - got) : strlen(got);
        size_t wlen = strlen(want_suffix);
        if (qlen >= wlen && strncmp(got + qlen - wlen, want_suffix, wlen) == 0) {
            return 1;
        }
    }
    printf("\n  %s -> %s: want one edge to ...%s, have %d:\n%s", caller, leaf, want_suffix, n,
           got);
    return 0;
}

/* No CALLS edge for (caller, leaf) reaches a node whose QN contains `bad`. */
static int jt_expect_none_to(jt_index_t *ix, const char *caller, const char *leaf,
                             const char *bad) {
    char got[4096];
    (void)jt_targets(ix, caller, leaf, got, sizeof(got));
    if (!strstr(got, bad)) {
        return 1;
    }
    printf("\n  %s -> %s: an edge reaches %s:\n%s", caller, leaf, bad, got);
    return 0;
}

/* No CALLS edge at all for (caller, leaf). */
static int jt_expect_no_edge(jt_index_t *ix, const char *caller, const char *leaf) {
    char got[4096];
    int n = jt_targets(ix, caller, leaf, got, sizeof(got));
    if (n == 0) {
        return 1;
    }
    printf("\n  %s -> %s: want no edge, have %d:\n%s", caller, leaf, n, got);
    return 0;
}

/* Number of nodes with this QN (under the fixture's source root) and, through
 * *lombok, whether the first one carries a `lombok` property. */
static int jt_node(jt_index_t *ix, const char *qn, int *lombok) {
    char full[512];
    snprintf(full, sizeof(full), "%s.src.main.java.%s", ix->project, qn);
    const char *sql = "SELECT json_extract(properties,'$.lombok') IS NOT NULL "
                      "FROM nodes WHERE qualified_name = ?1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ix->db, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, full, -1, SQLITE_TRANSIENT);
    int found = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (found++ == 0 && lombok) {
            *lombok = sqlite3_column_int(st, 0);
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* The CALLS edges out of the svc package, one sorted line each, with the
 * project prefix dropped so two indexes compare. The Java resolver's
 * strategies are kept; the textual registry's are one label, as its names
 * already differ between the two paths before this resolver runs (a
 * constructor is import_map on one and unique_name on the other). */
static char *jt_edge_set(jt_index_t *ix) {
    const char *sql =
        "SELECT group_concat(x, char(10)) FROM (SELECT s.name || '|' || "
        "coalesce(json_extract(e.properties,'$.callee'),'') || '|' || "
        "substr(t.qualified_name, length(?1) + 2) || '|' || "
        "CASE WHEN json_extract(e.properties,'$.strategy') LIKE 'lsp%' "
        "THEN json_extract(e.properties,'$.strategy') ELSE 'registry' END AS x FROM edges e "
        "JOIN nodes s ON s.id = e.source_id JOIN nodes t ON t.id = e.target_id "
        "WHERE e.type = 'CALLS' AND s.file_path LIKE '%/svc/%' ORDER BY x)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ix->db, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, ix->project, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
        out = strdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static int jt_check_receivers(jt_index_t *ix) {
    int ok = 1;
    /* A field typed by a project generic interface; its type argument flows
     * into the return of first() and on through Lombok getters. */
    ok &= jt_expect(ix, "customerName", "find", "com.acme.model.Repo.find");
    ok &= jt_expect(ix, "city", "first", "com.acme.model.Repo.first");
    ok &= jt_expect(ix, "city", "getCustomer", "com.acme.model.Invoice.getCustomer");
    ok &= jt_expect(ix, "city", "getAddress", "com.acme.model.Customer.getAddress");
    ok &= jt_expect(ix, "city", "getCity", "com.acme.model.Address.getCity");
    /* A lambda parameter bound through Optional<Invoice>.map. */
    ok &= jt_expect(ix, "customerName", "getName", "com.acme.model.Customer.getName");
    /* `var x = new X(...)`, a parameter, and an inherited bare call. */
    ok &= jt_expect(ix, "touch", "setPaid", "com.acme.model.Invoice.setPaid");
    ok &= jt_expect(ix, "touch", "describe", "com.acme.model.Customer.describe");
    ok &= jt_expect(ix, "touch", "audit", "com.acme.model.Base.audit");
    return ok;
}

static int jt_check_lombok_builder(jt_index_t *ix) {
    int ok = 1;
    ok &= jt_expect(ix, "make", "builder", "com.acme.model.Invoice.builder");
    ok &= jt_expect(ix, "make", "number", "com.acme.model.Invoice.InvoiceBuilder.number");
    ok &= jt_expect(ix, "make", "paid", "com.acme.model.Invoice.InvoiceBuilder.paid");
    ok &= jt_expect(ix, "make", "build", "com.acme.model.Invoice.InvoiceBuilder.build");
    return ok;
}

static int jt_check_external(jt_index_t *ix) {
    int ok = 1;
    /* JDK and library receivers never bind a project method by name... */
    ok &= jt_expect_none_to(ix, "numbers", "filter", "Filters");
    ok &= jt_expect_none_to(ix, "numbers", "map", "Filters");
    ok &= jt_expect_none_to(ix, "customerName", "map", "Filters");
    ok &= jt_expect_none_to(ix, "touch", "select", "Filters");
    ok &= jt_expect_none_to(ix, "touch", "size", "Filters");
    /* ...and get no edge: the graph holds no node for a library method. */
    ok &= jt_expect_no_edge(ix, "numbers", "filter");
    ok &= jt_expect_no_edge(ix, "touch", "select");
    ok &= jt_expect_no_edge(ix, "touch", "size");
    /* A receiver of unknown type does not guess the one Lombok getNumber. */
    ok &= jt_expect_no_edge(ix, "raw", "getNumber");
    return ok;
}

static int jt_check_lombok_nodes(jt_index_t *ix) {
    int ok = 1;
    const char *want[] = {
        "com.acme.model.Invoice.getNumber",  "com.acme.model.Invoice.isPaid",
        "com.acme.model.Invoice.setCustomer", "com.acme.model.Customer.setName",
        "com.acme.model.Address.getCity",    "com.acme.model.Invoice.InvoiceBuilder.build",
        /* @Accessors(prefix = "m"): mLabel's getter drops the prefix. */
        "com.acme.model.Settings.getLabel",
        /* @Builder.Default keeps an initialised final field in the builder. */
        "com.acme.model.Order.OrderBuilder.state",
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        if (jt_node(ix, want[i], NULL) != 1) {
            printf("\n  want one Lombok node %s\n", want[i]);
            ok = 0;
        }
    }
    /* Not generated: a setter on @Value, an accessor of an unannotated field,
     * a field no @Accessors prefix matches, an initialised final field in a
     * builder, and a getter for Spring's @Value. */
    const char *absent[] = {
        "com.acme.model.Address.setCity",   "com.acme.model.Customer.getHidden",
        "com.acme.model.Settings.getMode",  "com.acme.model.Settings.getMLabel",
        "com.acme.model.Order.OrderBuilder.id", "com.acme.other.Config.getInterval",
        /* a record's static field is no component */
        "com.acme.model.Basket.BasketBuilder.EMPTY",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        if (jt_node(ix, absent[i], NULL) != 0) {
            printf("\n  unexpected Lombok node %s\n", absent[i]);
            ok = 0;
        }
    }
    /* The builder class the source declares, and its own note(), stay the
     * source's: one node each, with no `lombok` property. */
    const char *own[] = {"com.acme.model.Order.OrderBuilder",
                         "com.acme.model.Order.OrderBuilder.note"};
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        int lombok = -1;
        if (jt_node(ix, own[i], &lombok) != 1 || lombok != 0) {
            printf("\n  %s: want one declared node, no lombok property\n", own[i]);
            ok = 0;
        }
    }
    return ok;
}

static int jt_check_lombok_options(jt_index_t *ix) {
    int ok = 1;
    ok &= jt_expect(ix, "label", "getLabel", "com.acme.model.Settings.getLabel");
    ok &= jt_expect(ix, "order", "builder", "com.acme.model.Order.builder");
    ok &= jt_expect(ix, "order", "state", "com.acme.model.Order.OrderBuilder.state");
    ok &= jt_expect(ix, "order", "note", "com.acme.model.Order.OrderBuilder.note");
    ok &= jt_expect(ix, "order", "build", "com.acme.model.Order.OrderBuilder.build");
    ok &= jt_expect(ix, "basket", "currency", "com.acme.model.Basket.BasketBuilder.currency");
    ok &= jt_expect(ix, "basket", "amount", "com.acme.model.Basket.BasketBuilder.amount");
    ok &= jt_expect(ix, "basket", "build", "com.acme.model.Basket.BasketBuilder.build");
    return ok;
}

static int jt_run_all(int filler) {
    jt_index_t ix;
    if (jt_index(&ix, filler) != 0) {
        printf("\n  indexing the fixture failed\n");
        jt_close(&ix);
        return 0;
    }
    int ok = 1;
    ok &= jt_check_lombok_nodes(&ix);
    ok &= jt_check_receivers(&ix);
    ok &= jt_check_lombok_builder(&ix);
    ok &= jt_check_lombok_options(&ix);
    ok &= jt_check_external(&ix);
    jt_close(&ix);
    return ok;
}

TEST(java_typed_calls_sequential) {
    ASSERT_TRUE(jt_run_all(0));
    PASS();
}

TEST(java_typed_calls_parallel) {
    ASSERT_TRUE(jt_run_all(60));
    PASS();
}

/* The sequential and the parallel path produce the same svc edges. */
TEST(java_typed_calls_paths_agree) {
    jt_index_t seq;
    jt_index_t par;
    int seq_rc = jt_index(&seq, 0);
    int par_rc = jt_index(&par, 60);
    char *a = seq_rc == 0 ? jt_edge_set(&seq) : NULL;
    char *b = par_rc == 0 ? jt_edge_set(&par) : NULL;
    int same = a && b && strcmp(a, b) == 0;
    if (!same) {
        printf("\n  sequential:\n%s\n  parallel:\n%s\n", a ? a : "(none)", b ? b : "(none)");
    }
    free(a);
    free(b);
    jt_close(&seq);
    jt_close(&par);
    ASSERT_TRUE(same);
    PASS();
}

SUITE(java_typed_calls) {
    RUN_TEST(java_typed_calls_sequential);
    RUN_TEST(java_typed_calls_parallel);
    RUN_TEST(java_typed_calls_paths_agree);
}
