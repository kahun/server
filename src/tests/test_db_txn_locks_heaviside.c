/* -*- mode: C; c-basic-offset: 4 -*- */
#include <toku_portability.h>
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."

#include <memory.h>
#include <toku_portability.h>
#include <db.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

#include "test.h"

struct heavi_extra {
    DBT key;
    DBT val;
    DB* db;
};

static int
heavi_after (const DBT *key, const DBT *val, void *extra) {
    //Assumes cmp is int_dbt_cmp
    struct heavi_extra *info = extra;
    int cmp = int_dbt_cmp(info->db, key, &info->key);
    if (cmp!=0) return cmp;
    if (!val) return -1;
    cmp = int_dbt_cmp(info->db, val, &info->val);
    return cmp<=0 ? -1 : 0;
    //Returns <0 for too small/equal
    //Returns 0 for greater, but with the same key
    //Returns >0 for greater with different key
}

static int
heavi_before (const DBT *key, const DBT *val, void *extra) {
    struct heavi_extra *info = extra;
    int cmp = int_dbt_cmp(info->db, key, &info->key);
    if (cmp!=0) return cmp;
    if (!val) return +1;
    cmp = int_dbt_cmp(info->db, val, &info->val);
    return cmp>=0 ? 1 : 0;
    //Returns >0 for too large/equal
    //Returns 0 for smaller with same key
    //returns -1 for smaller with different key
}

// ENVDIR is defined in the Makefile

static DB *db;
static DB_TXN* txns[(int)256];
static DB_ENV* dbenv;
static DBC*    cursors[(int)256];

static void
put(BOOL success, char txn, int _key, int _data) {
    assert(txns[(int)txn]);

    int r;
    DBT key;
    DBT data;
    
    r = db->put(db, txns[(int)txn],
                    dbt_init(&key, &_key, sizeof(int)),
                    dbt_init(&data, &_data, sizeof(int)),
                    DB_YESOVERWRITE);

    if (success)    CKERR(r);
    else            CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
}

static void
cget(BOOL success, BOOL find, char txn, int _key, int _data, 
     int _key_expect, int _data_expect, u_int32_t flags) {
    assert(txns[(int)txn] && cursors[(int)txn]);

    int r;
    DBT key;
    DBT data;
    
    r = cursors[(int)txn]->c_get(cursors[(int)txn],
                                 dbt_init(&key,  &_key,  sizeof(int)),
                                 dbt_init(&data, &_data, sizeof(int)),
                                 flags);
    if (success) {
        if (find) {
            CKERR(r);
            assert(*(int *)key.data  == _key_expect);
            assert(*(int *)data.data == _data_expect);
        }
        else        CKERR2(r,  DB_NOTFOUND);
    }
    else            CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
}

static void
init_txn (char name) {
    int r;
    assert(!txns[(int)name]);
    r = dbenv->txn_begin(dbenv, NULL, &txns[(int)name], DB_TXN_NOWAIT);
        CKERR(r);
    assert(txns[(int)name]);
}

static void
init_dbc (char name) {
    int r;

    assert(!cursors[(int)name] && txns[(int)name]);
    r = db->cursor(db, txns[(int)name], &cursors[(int)name], 0);
        CKERR(r);
    assert(cursors[(int)name]);
}

static void
commit_txn (char name) {
    int r;
    assert(txns[(int)name] && !cursors[(int)name]);

    r = txns[(int)name]->commit(txns[(int)name], 0);
        CKERR(r);
    txns[(int)name] = NULL;
}

static void
abort_txn (char name) {
    int r;
    assert(txns[(int)name] && !cursors[(int)name]);

    r = txns[(int)name]->abort(txns[(int)name]);
        CKERR(r);
    txns[(int)name] = NULL;
}

static void
close_dbc (char name) {
    int r;

    assert(cursors[(int)name]);
    r = cursors[(int)name]->c_close(cursors[(int)name]);
        CKERR(r);
    cursors[(int)name] = NULL;
}

static void
early_commit (char name) {
    assert(cursors[(int)name] && txns[(int)name]);
    close_dbc(name);
    commit_txn(name);
}

static void
early_abort (char name) {
    assert(cursors[(int)name] && txns[(int)name]);
    close_dbc(name);
    abort_txn(name);
}

static void
setup_dbs (u_int32_t dup_flags) {
    int r;

    system("rm -rf " ENVDIR);
    toku_os_mkdir(ENVDIR, S_IRWXU+S_IRWXG+S_IRWXO);
    dbenv   = NULL;
    db      = NULL;
    /* Open/create primary */
    r = db_env_create(&dbenv, 0);
        CKERR(r);
    u_int32_t env_txn_flags  = DB_INIT_TXN | DB_INIT_LOCK;
    u_int32_t env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL;
	r = dbenv->open(dbenv, ENVDIR, env_open_flags | env_txn_flags, 0600);
        CKERR(r);
    
    r = db_create(&db, dbenv, 0);
        CKERR(r);
    if (dup_flags) {
        r = db->set_flags(db, dup_flags);
            CKERR(r);
    }
    r = db->set_bt_compare( db, int_dbt_cmp);
    CKERR(r);
    r = db->set_dup_compare(db, int_dbt_cmp);
    CKERR(r);

    char a;
    for (a = 'a'; a <= 'z'; a++) init_txn(a);
    init_txn('\0');
    r = db->open(db, txns[(int)'\0'], "foobar.db", NULL, DB_BTREE, DB_CREATE, 0600);
        CKERR(r);
    commit_txn('\0');
    for (a = 'a'; a <= 'z'; a++) init_dbc(a);
}

static void
close_dbs(void) {
    char a;
    for (a = 'a'; a <= 'z'; a++) {
        if (cursors[(int)a]) close_dbc(a);
        if (txns[(int)a])    commit_txn(a);
    }

    int r;
    r = db->close(db, 0);
        CKERR(r);
    db      = NULL;
    r = dbenv->close(dbenv, 0);
        CKERR(r);
    dbenv   = NULL;
}


static __attribute__((__unused__))
void
test_abort (u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    early_abort('a');
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_SET);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_SET);
    put(FALSE, 'a', 1, 1);
    early_commit('b');
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 1, 1, 1, 1, DB_SET);
    cget(TRUE, FALSE, 'a', 2, 1, 1, 1, DB_SET);
    cget(FALSE, TRUE, 'c', 1, 1, 0, 0, DB_SET);
    early_abort('a');
    cget(TRUE, FALSE, 'c', 1, 1, 0, 0, DB_SET);
    close_dbs();
    /* ********************************************************************** */
}

struct dbt_pair {
    DBT key;
    DBT val;
};

struct int_pair {
    int key;
    int val;
};

int got_r_h;

static __attribute__((__unused__))
int
f_heavi (DBT const *key, DBT const *val, void *extra_f, int r_h) {
    struct int_pair *info = extra_f;

    if (r_h==0) got_r_h = 0;
    assert(key->size == 4);
    assert(val->size == 4);
    
    info->key = *(int*)key->data;
    info->val = *(int*)val->data;
    int r = 0;
    return r;
}

static __attribute__((__unused__))
void
ignore (void *ignore __attribute__((__unused__))) {
}
#define TOKU_IGNORE(x) ignore((void*)x)

static void
cget_heavi (BOOL success, BOOL find, char txn, int _key, int _val, 
	    int _key_expect, int _val_expect, int direction,
	    int r_h_expect,
	    int (*h)(const DBT*,const DBT*,void*)) {
#if defined(USE_BDB)
    TOKU_IGNORE(success);
    TOKU_IGNORE(find);
    TOKU_IGNORE((size_t)txn);
    TOKU_IGNORE((size_t)_key);
    TOKU_IGNORE((size_t)_val);
    TOKU_IGNORE((size_t)_key_expect);
    TOKU_IGNORE((size_t)_val_expect);
    TOKU_IGNORE((size_t)direction);
    TOKU_IGNORE(h);
    TOKU_IGNORE((size_t)r_h_expect);
    return;
#else
    assert(txns[(int)txn] && cursors[(int)txn]);

    int r;
    struct heavi_extra input;
    struct int_pair output;
    dbt_init(&input.key, &_key, sizeof(int));
    dbt_init(&input.val, &_val, sizeof(int));
    input.db = db;
    output.key = 0;
    output.val = 0;
    
    got_r_h = direction;

    r = cursors[(int)txn]->c_getf_heaviside(cursors[(int)txn], 0, //No prelocking
               f_heavi, &output,
               h, &input, direction);
    if (!success) {
        CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
        return;
    }
    if (!find) {
        CKERR2(r,  DB_NOTFOUND);
        return;
    }
    CKERR(r);
    assert(got_r_h == r_h_expect);
    assert(output.key == _key_expect);
    assert(output.val == _val_expect);
#endif
}


static void
test_heavi (u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget_heavi(TRUE, FALSE, 'a', 0, 0, 0, 0,  1, 0, heavi_after); 
    cget_heavi(TRUE, FALSE, 'a', 0, 0, 0, 0, -1, 0, heavi_before); 
    close_dbs();
    /* ********************************************************************** */
    //Not found locks left to right (with empty db == entire db)
    setup_dbs(dup_flags);
    cget_heavi(TRUE, FALSE, 'a', 0, 0, 0, 0,  1, 0, heavi_after); 
    put(FALSE, 'b', 7, 6);
    put(FALSE, 'b', -1, -1);
    put(TRUE,  'a', 4, 4);
    early_commit('a');
    put(TRUE, 'b', 7, 6);
    put(TRUE, 'b', -1, -1);
    close_dbs();
    /* ********************************************************************** */
    //Not found locks left to right (with empty db == entire db)
    setup_dbs(dup_flags);
    cget_heavi(TRUE, FALSE, 'a', 0, 0, 0, 0, -1, 0, heavi_before); 
    put(FALSE, 'b', 7, 6);
    put(FALSE, 'b', -1, -1);
    put(TRUE,  'a', 4, 4);
    early_commit('a');
    put(TRUE, 'b', 7, 6);
    put(TRUE, 'b', -1, -1);
    close_dbs();
    /* ********************************************************************** */
    //Duplicate mode behaves differently.
    setup_dbs(dup_flags);
    int k,v;
    for (k = 10; k <= 100; k+= 10) {
        v = k+5;
        put(TRUE, 'a', k, v);
    }
    if (dup_flags) {
        cget_heavi(TRUE, TRUE, 'a', 100, 0, 100, 105,  1, 0, heavi_after); 
    }
    else {
        cget_heavi(TRUE, FALSE, 'a', 100, 0, 0, 0,  1, 0, heavi_after); 
    }
    close_dbs();
    /* ********************************************************************** */
    //Locks stop at actual elements in the DB.
    setup_dbs(dup_flags);
    //int k,v;
    for (k = 10; k <= 100; k+= 10) {
        v = k+5;
        put(TRUE, 'a', k, v);
    }
    cget_heavi(TRUE, FALSE, 'a', 105, 1, 0, 0,  1, 0, heavi_after); 
    put(FALSE, 'b', 104, 1);
    put(FALSE, 'b', 105, 0);
    put(FALSE, 'b', 105, 1);
    put(FALSE, 'b', 105, 2);
    put(FALSE, 'b', 106, 0);
    put(TRUE,  'b', 99,  0);
    put((BOOL)(dup_flags!=0), 'b', 100, 104);
    close_dbs();
    /* ********************************************************************** */
    // Test behavior of heavi_after
    setup_dbs(dup_flags);
    //int k,v;
    for (k = 10; k <= 100; k+= 10) {
        v = k+5;
        put(TRUE, 'a', k, v);
    }
    for (k = 5; k <= 95; k+= 10) {
        v = k+5;
        cget_heavi(TRUE, TRUE, 'a', k, v, k+5, v+5,  1, 1, heavi_after); 
    }
    put(FALSE, 'b', -1, -2);
    put(TRUE, 'b', 200, 201);
    cget_heavi(FALSE, FALSE, 'a', 105, 105, 0, 0, 1, 0, heavi_after);
    close_dbs();
    /* ********************************************************************** */
    // Test behavior of heavi_before
    setup_dbs(dup_flags);
    //int k,v;
    for (k = 10; k <= 100; k+= 10) {
        v = k+5;
        put(TRUE, 'a', k, v);
    }
    for (k = 105; k >= 15; k-= 10) {
        v = k+5;
        cget_heavi(TRUE, TRUE, 'a', k, v, k-5, v-5,  -1, -1, heavi_before); 
    }
    put(FALSE, 'b', 200, 201);
    put(TRUE,  'b', -1, -2);
    cget_heavi(FALSE, FALSE, 'a', -5, -5, 0, 0, -1, 0, heavi_after);
    close_dbs();
}

static void
test (u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    close_dbs();
    /* ********************************************************************** */
    test_heavi(dup_flags);
    /* ********************************************************************** */
}


int
test_main(int argc, const char* argv[]) {
    parse_args(argc, argv);
    if (!IS_TDB) {
	if (verbose) {
	    printf("Warning: " __FILE__" does not work in BDB.\n");
	}
    } else {
	test(0);
	test(DB_DUP | DB_DUPSORT);
	/*
	  test_abort(0);
	  test_abort(DB_DUP | DB_DUPSORT);
	*/
    }
    return 0;
}