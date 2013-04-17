/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2009 Tokutek Inc.  All rights reserved."
#include "test.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <memory.h>
#include <sys/stat.h>
#include <db.h>
#include "tokuconst.h"
#define MAX_NEST MAX_TRANSACTION_RECORDS
#define MAX_SIZE MAX_TRANSACTION_RECORDS

/*********************
 *
 * Purpose of this test is to verify nested transactions (support right number of possible values)
create empty db
for test = 1 to MAX
   for nesting level 0
     - randomly insert or not
   for nesting_level = 1 to MAX
     - begin txn
     - randomly one of (insert, delete, do nothing)
     -  if insert, use a value/len unique to this txn
     - query to verify
   for nesting level = MAX to 1
     - randomly abort or commit each transaction
     - query to verify
delete db
 */


enum { TYPE_DELETE = 1, TYPE_INSERT, TYPE_PLACEHOLDER };

u_int8_t valbufs[MAX_NEST][MAX_SIZE];
DBT vals        [MAX_NEST];
u_int8_t keybuf [MAX_SIZE];
DBT key;
u_int8_t types  [MAX_NEST];
u_int8_t currval[MAX_NEST];
DB_TXN   *txns   [MAX_NEST];
DB_TXN   *txn_query;
int which_expected;

static void
fillrandom(u_int8_t buf[MAX_SIZE], u_int32_t length) {
    assert(length < MAX_SIZE);
    u_int32_t i;
    for (i = 0; i < length; i++) {
        buf[i] = random() & 0xFF;
    } 
}

static void
initialize_values (void) {
    int nest_level;
    for (nest_level = 0; nest_level < MAX_NEST; nest_level++) {
        fillrandom(valbufs[nest_level], nest_level);
        dbt_init(&vals[nest_level], &valbufs[nest_level][0], nest_level);
    }
    u_int32_t len = random() % MAX_SIZE;
    fillrandom(keybuf, len);
    dbt_init(&vals[nest_level], &keybuf[0], len);
}


static DB *db;
static DB_ENV *env;

static void
setup_db (void) {
    system("rm -rf " ENVDIR);
    toku_os_mkdir(ENVDIR, S_IRWXU+S_IRWXG+S_IRWXO);

    int r;
    r = db_env_create(&env, 0); CKERR(r);
    r = env->set_data_dir(env, ENVDIR);
    r = env->set_lg_dir(env, ENVDIR);
    r = env->open(env, 0, DB_INIT_MPOOL | DB_INIT_LOG | DB_INIT_LOCK | DB_INIT_TXN | DB_PRIVATE | DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); 
    CKERR(r);

    {
        DB_TXN *txn = 0;
        r = env->txn_begin(env, 0, &txn, 0); CKERR(r);

        r = db_create(&db, env, 0); CKERR(r);
        r = db->open(db, txn, "test.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
        r = txn->commit(txn, 0); CKERR(r);
    }
    r = env->txn_begin(env, NULL, &txn_query, DB_READ_UNCOMMITTED);
        CKERR(r);
}


static void
close_db (void) {
    int r;
    r = txn_query->commit(txn_query, 0);
    CKERR(r);
    r=db->close(db, 0); CKERR(r);
    r=env->close(env, 0); CKERR(r);
}

static void
verify_val(u_int8_t nest_level) {
    assert(nest_level < MAX_NEST);
    if (nest_level>0) assert(txns[nest_level]);
    assert(types[nest_level] != TYPE_PLACEHOLDER);
    int r;
    DBT observed_val;
    dbt_init(&observed_val, NULL, 0);
    r = db->get(db, txn_query, &key, &observed_val, 0);
    if (types[nest_level] == TYPE_INSERT) {
        CKERR(r);
        int idx = currval[nest_level];
        assert(observed_val.size == vals[idx].size);
        assert(memcmp(observed_val.data, vals[idx].data, vals[idx].size) == 0);
    }
    else {
        assert(types[nest_level] == TYPE_DELETE);
        CKERR2(r, DB_NOTFOUND);
    }
}

static u_int8_t
randomize_no_placeholder_type() {
    int r;
    r = random() % 2;
    switch (r) {
        case 0:
            return TYPE_INSERT;
        case 1:
            return TYPE_DELETE;
        default:
            assert(FALSE);
    }
}

static u_int8_t
randomize_type() {
    int r;
    r = random() % 4;
    switch (r) {
        case 0:
            return TYPE_INSERT;
        case 1:
            return TYPE_DELETE;
        case 2:
        case 3:
            return TYPE_PLACEHOLDER;
        default:
            assert(FALSE);
    }
}

static void
maybe_insert_or_delete(u_int8_t nest, int type) {
    int r;
    if (nest>0) assert(txns[nest]);
    types[nest] = type;
    currval[nest] = nest;
    switch (types[nest]) {
        case TYPE_INSERT:
            r = db->put(db, txns[nest], &key, &vals[nest], DB_YESOVERWRITE);
                CKERR(r);
            break;
        case TYPE_DELETE:
            r = db->del(db, txns[nest], &key, DB_DELETE_ANY);
                CKERR(r);
            break;
        case TYPE_PLACEHOLDER:
            types[nest] = types[nest - 1];
            currval[nest] = currval[nest-1];
            break;
        default:
            assert(FALSE);
    }
    verify_val(nest);
}

static void
start_txn_and_maybe_insert_or_delete(u_int8_t nest) {
    int iteration;
    int r;
    for (iteration = 0; iteration < 4; iteration++) {
        BOOL skip = FALSE;
        if (nest == 0) {
            types[nest] = randomize_no_placeholder_type();
            assert(types[nest] != TYPE_PLACEHOLDER);
            //Committed entry is autocommitted by not providing the txn
            txns[nest] = NULL;
        }
        else {
            if (iteration == 0) {
                types[nest] = randomize_type();
                r = env->txn_begin(env, txns[nest-1], &txns[nest], 0);
                    CKERR(r);
                if (types[nest] == TYPE_PLACEHOLDER) skip = TRUE;
            }
            else {
                types[nest] = randomize_no_placeholder_type();
                assert(types[nest] != TYPE_PLACEHOLDER);
            }
        }
        maybe_insert_or_delete(nest, types[nest]);
        assert(types[nest] != TYPE_PLACEHOLDER);
        if (skip) break;
    }
}

static void
initialize_db(void) {
    types[0] = TYPE_DELETE; //Not yet inserted
    verify_val(0);
    int i;
    for (i = 0; i < MAX_NEST; i++) {
        start_txn_and_maybe_insert_or_delete(i);
    }
}

static void
test_txn_nested_jumble (int iteration) {
    int r;
    if (verbose) { fprintf(stderr, "%s (%s):%d [iteration # %d]\n", __FILE__, __FUNCTION__, __LINE__, iteration); fflush(stderr); }

    initialize_db();

    //BELOW IS OLD CODE
    int index_of_expected_value  = MAX_NEST - 1;
    int nest_level               = MAX_NEST - 1;
    int min_allowed_branch_level = MAX_NEST - 2;
    while (nest_level > 0) {
        int operation = random() % 4;
        switch (operation) {
            case 0:
                //abort
                r = txns[nest_level]->abort(txns[nest_level]);
                    CKERR(r);
                index_of_expected_value = nest_level - 1;
                txns[nest_level] = NULL;
                nest_level--;
                verify_val(index_of_expected_value);
                break;
            case 1:
                //commit
                r = txns[nest_level]->commit(txns[nest_level], DB_TXN_NOSYNC);
                    CKERR(r);
                currval[nest_level-1] = currval[index_of_expected_value];
                types[nest_level-1]   = types[index_of_expected_value];
                index_of_expected_value = nest_level - 1;
                txns[nest_level] = NULL;
                nest_level--;
                verify_val(index_of_expected_value);
                break;
            case 2:;
                //do more work with this guy
                int type = randomize_no_placeholder_type();
                maybe_insert_or_delete(nest_level, type);
                index_of_expected_value = nest_level;
                continue; //transaction is still alive
            case 3:
                if (min_allowed_branch_level >= nest_level) {
                    //start new subtree
                    int max = nest_level + 4;
                    if (MAX_NEST - 1 < max) max = MAX_NEST - 1;
                    assert(max > nest_level);
                    int branch_level;
                    for (branch_level = nest_level + 1; branch_level <= max; branch_level++) {
                        start_txn_and_maybe_insert_or_delete(branch_level);
                    }
                    nest_level = max;
                    min_allowed_branch_level--;
                    index_of_expected_value = nest_level;
                }
                continue; //transaction is still alive
            default:
                assert(FALSE);
        }
    }
    //Clean out dictionary

    types[0] = TYPE_DELETE;
    r = db->del(db, NULL, &key, DB_DELETE_ANY);
        CKERR(r);
    verify_val(0);
}

int
test_main(int argc, char *argv[]) {
    parse_args(argc, argv);
    initialize_values();
    int i;
    setup_db();
    for (i = 0; i < 100; i++) {
        test_txn_nested_jumble(i);
    }
    close_db();
    return 0;
}
