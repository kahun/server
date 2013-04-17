/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."
#include "test.h"

#include <db.h>
#include <sys/stat.h>

static void
test_stat64 (unsigned int N)
{
    system("rm -rf " ENVDIR);
    toku_os_mkdir(ENVDIR, S_IRWXU+S_IRWXG+S_IRWXO);
    
    int r;
    DB_ENV *env;
    DB *db;
    DB_TXN *txn;
    r = db_env_create(&env, 0);                                           CKERR(r);

    r = env->set_cachesize(env, 0, 10000000, 1);
    r = env->open(env, ENVDIR, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r = db_create(&db, env, 0);                                           CKERR(r);

    {
	r=env->txn_begin(env, 0, &txn, 0); assert(r==0);
	r=db->open(db, txn, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
	r=txn->commit(txn, 0);    assert(r==0);
    }

    r=env->txn_begin(env, 0, &txn, 0);  CKERR(r);
    unsigned int i;
    u_int64_t dsize=0;
    for (i=0; i<N; i++) {
	char hello[30], there[30];
	snprintf(hello, sizeof(hello), "hello%d", i);
	snprintf(there, sizeof(there), "there%d", i);
	DBT key, val;
	r=db->put(db, txn,
		      dbt_init(&key, hello, strlen(hello)+1),
		      dbt_init(&val, there, strlen(there)+1),
		      DB_YESOVERWRITE);
	dsize += strlen(hello)+1 + strlen(there)+1;
	CKERR(r);
    }
    r=txn->commit(txn, 0); CKERR(r);

    r=env->txn_begin(env, 0, &txn, 0);  CKERR(r);
    DB_BTREE_STAT64 s;
    r=db->stat64(db, txn, &s); CKERR(r);
    if (verbose) {
	system("ls -l " ENVDIR);
	printf("nkeys=%" PRIu64 "\nndata=%" PRIu64 "\ndsize=%" PRIu64 "\n",
	       s.bt_nkeys, s.bt_ndata, s.bt_dsize);
	printf("fsize=%" PRIu64 "\n", s.bt_fsize);
	printf("expected dsize=%" PRIu64 "\n", dsize); 
    }
    assert(s.bt_nkeys==N);
    assert(s.bt_ndata==N);
    assert(s.bt_dsize==dsize);
    assert(s.bt_fsize>N);
    r=txn->commit(txn, 0); CKERR(r);

    r=db->close(db, 0); CKERR(r);
    r=env->close(env, 0); CKERR(r);
}

int
test_main (int argc, char *argv[])
{
    parse_args(argc, argv);
    test_stat64(40000);
    test_stat64(400000);
    return 0;
}