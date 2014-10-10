/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Special exception for linking this software with OpenSSL:
 *
 *  In addition, as a special exception, Skylable Ltd. gives permission to
 *  link the code of this program with the OpenSSL library and distribute
 *  linked combinations including the two. You must obey the GNU General
 *  Public License in all respects for all of the code used other than
 *  OpenSSL. You may extend this exception to your version of the program,
 *  but you are not obligated to do so. If you do not wish to do so, delete
 *  this exception statement from your version.
 */

#include "default.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>

#include "sxdbi.h"
#include "hashfs.h"
#include "hdist.h"
#include "../libsx/src/misc.h"

#include "sx.h"
#include "qsort.h"
#include "utils.h"
#include "../libsx/src/vcrypto.h"
#include "../libsx/src/clustcfg.h"

#define HASHDBS 16
#define METADBS 16
#define GCDBS 1
/* NOTE: HASHFS_VERSION must be kept below 15 bytes */
#define HASHFS_VERSION "SX-Storage 1.5"
#define SIZES 3
const char sizedirs[SIZES] = "sml";
const char *sizelongnames[SIZES] = { "small", "medium", "large" };
const unsigned int bsz[SIZES] = {SX_BS_SMALL, SX_BS_MEDIUM, SX_BS_LARGE};

#define HDIST_SEED 0x1337
#define MURMUR_SEED 0xacab
#define TOKEN_REPLICA_LEN 8
#define TOKEN_EXPIRE_LEN 16
#define TOKEN_TEXT_LEN (UUID_STRING_SIZE + 1 + TOKEN_RAND_BYTES * 2 + 1 + TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN + 1 + AUTH_KEY_LEN * 2)

/* FIXME: the following optimization is not currently used (filehash_xxx are no ops)
 * To be reviewed and reenabled or dropped for good */
/* #define FILEHASH_OPTIMIZATION */

#define WARNHASH(MSG, X) do {				\
    char _warnhash[sizeof(sx_hash_t)*2+1];		\
    bin2hex((X)->b, sizeof(*X), _warnhash, sizeof(_warnhash));	\
    WARN("(%s): %s #%s#", __FUNCTION__, MSG, _warnhash); \
    } while(0)

#define DEBUGHASH(MSG, X) do {				\
    char _debughash[sizeof(sx_hash_t)*2+1];		\
    if (UNLIKELY(sxi_log_is_debug(&logger))) {          \
	bin2hex((X)->b, sizeof(*X), _debughash, sizeof(_debughash));	\
	DEBUG("%s: #%s#", MSG, _debughash);				\
    }\
    } while(0)

rc_ty sx_hashfs_check_blocksize(unsigned int bs) {
    unsigned int hs;
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    return (hs == SIZES) ? FAIL_BADBLOCKSIZE : OK;
}

static int write_block(int fd, const void *data, uint64_t off, unsigned int data_len) {
    uint8_t *dt = (uint8_t *)data;
    while(data_len) {
	int l = pwrite(fd, dt, data_len, off);
	if(l<0) {
	    if(errno == EINTR)
		continue;
	    msg_set_errno_reason("Failed to write block");
	    return 1;
	}
	data_len -= l;
	dt += l;
	off += l;
    }
    return 0;
}

static int read_block(int fd, uint8_t *dt, uint64_t off, unsigned int buf_len) {
    while(buf_len) {
	int l = pread(fd, dt, buf_len, off);
	if(l<0) {
	    if(errno == EINTR)
		continue;
	    msg_set_errno_reason("Failed to read block");
	    return 1;
	}
	if(!l) {
	    msg_set_reason("Incomplete block read");
	    return 1;
	}
	buf_len -= l;
	dt += l;
	off += l;
    }
    return 0;
}

int sx_hashfs_hash_buf(const void *salt, unsigned int salt_len, const void *buf, unsigned int buf_len, sx_hash_t *hash) {
    return sxi_sha1_calc(salt, salt_len, buf, buf_len, hash->b);
}
#define hash_buf sx_hashfs_hash_buf

#define CREATE_DB(DBTYPE) \
do { \
    sqlite3 *handle = NULL;\
    /* Create the dbatabase */ \
    if(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)) { \
	CRIT("Failed to create %s database: %s", DBTYPE, sqlite3_errmsg(handle)); \
	goto create_hashfs_fail; \
    } \
    if (!(db = qnew(handle))) \
        goto create_hashfs_fail;\
    if(qprep(db, &q, "PRAGMA synchronous = OFF") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    if(qprep(db, &q, "PRAGMA journal_mode = WAL") || qstep_ret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Set create the hashfs table which is a generic k/v store for config items */ \
    if(qprep(db, &q, "CREATE TABLE hashfs (key TEXT NOT NULL PRIMARY KEY, value TEXT NOT NULL)") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Fill in the basic settings */ \
    if(qprep(db, &q, "INSERT INTO hashfs (key, value) VALUES (:k, :v)")) \
	goto create_hashfs_fail; \
    if(qbind_text(q, ":k", "version") || qbind_text(q, ":v", HASHFS_VERSION) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "dbtype") || qbind_text(q, ":v", DBTYPE) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "cluster") || qbind_blob(q, ":v", cluster->binary, sizeof(cluster->binary)) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    DEBUG("creating %s", path);\
} while(0)

/* TODO: share more code */
#define CREATE_DB2(DBTYPE) \
do { \
    sqlite3 *handle = NULL;\
    /* Create the dbatabase */ \
    if(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)) { \
	CRIT("Failed to create %s database: %s", DBTYPE, sqlite3_errmsg(handle)); \
	goto create_hashfs_fail; \
    } \
    if (!(db = qnew(handle))) \
        goto create_hashfs_fail; \
    if(qprep(db, &q, "PRAGMA synchronous=NORMAL") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    if(qprep(db, &q, "PRAGMA journal_mode = WAL") || qstep_ret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Set create the hashfs table which is a generic k/v store for config items */ \
    if(qprep(db, &q, "CREATE TABLE hashfs (key TEXT NOT NULL PRIMARY KEY, value TEXT NOT NULL)") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Fill in the basic settings */ \
    if(qprep(db, &q, "INSERT INTO hashfs (key, value) VALUES (:k, :v)")) \
	goto create_hashfs_fail; \
    if(qbind_text(q, ":k", "version") || qbind_text(q, ":v", HASHFS_VERSION) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "dbtype") || qbind_text(q, ":v", DBTYPE) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "cluster") || qbind_blob(q, ":v", cluster->binary, sizeof(cluster->binary)) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    DEBUG("creating %s", path);\
} while(0)


static int qlog_set = 0;
rc_ty sx_storage_create(const char *dir, sx_uuid_t *cluster, uint8_t *key, int key_size) {
    unsigned int dirlen, i, j;
    sxi_db_t *db = NULL;
    sqlite3_stmt *q = NULL;
    char *path, dbitem[64];
    int ret = FAIL_EINIT;
    sxc_uri_t *uri = NULL;

    if(!dir || !(dirlen = strlen(dir))) {
	CRIT("Bad path");
	return EINVAL;
    }

    if(ssl_version_check())
	return FAIL_EINIT;

    if(access(dir, R_OK | W_OK | X_OK)) {
	PCRIT("Cannot access storage directory %s", dir);
	return FAIL_EINIT;
    }

    if(!(path = wrap_malloc(dirlen + bsz[SIZES-1])))
	goto create_hashfs_fail;

    /* --- HASHFS db --- */
    sqlite3_config(SQLITE_CONFIG_LOG, qlog, NULL);
    qlog_set = 1;
    sprintf(path, "%s/hashfs.db", dir);
    CREATE_DB("hashfs");
    sqlite3_reset(q); /* q is now prepared for hashfs insertions */

    if(qbind_text(q, ":k", "current_dist_rev") || qbind_int64(q, ":v", 0) || qstep_noret(q))
	goto create_hashfs_fail;
    sqlite3_reset(q);
    if(qbind_text(q, ":k", "current_dist") || qbind_blob(q, ":v", "", 0) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the file dbs */
    for(i=0; i<METADBS; i++) {
	sprintf(dbitem, "metadb_%08x", i);
	sprintf(path, "f%08x.db", i);
	sqlite3_reset(q);
	if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	    goto create_hashfs_fail;
    }

    /* Set the path to the block dbs */
    for(j = 0; j < SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    sprintf(path, "h%c%08x.db", sizedirs[j], i);
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
		goto create_hashfs_fail;

	    sprintf(dbitem, "datafile_%c_%08x", sizedirs[j], i);
	    sprintf(path, "h%c%08x.bin", sizedirs[j], i);
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
		goto create_hashfs_fail;
	}
    }

    /* Set the path to the temp db */
    strcpy(dbitem, "tempdb");
    strcpy(path, "temp.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the event db */
    strcpy(dbitem, "eventdb");
    strcpy(path, "events.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the xfer db */
    strcpy(dbitem, "xferdb");
    strcpy(path, "xfers.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    /* Create HASHFS tables */
    if(qprep(db, &q, "CREATE TABLE users (uid INTEGER PRIMARY KEY NOT NULL, user BLOB ("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL UNIQUE, name TEXT ("STRIFY(SXLIMIT_MAX_USERNAME_LEN)") NOT NULL UNIQUE, key BLOB ("STRIFY(AUTH_KEY_LEN)") NOT NULL UNIQUE, role INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 0)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
/*    if(qprep(db, &q, "CREATE INDEX users_byname ON users(name, enabled)") || qstep_noret(q))
       goto create_hashfs_fail;
    qnullify(q);*/

    if(qprep(db, &q, "INSERT INTO users(uid, user, name, key, role, enabled) VALUES(0, :userhash, :name, :key, :role, 1)") ||
       qbind_blob(q, ":userhash", CLUSTER_USER, AUTH_UID_LEN) ||
       qbind_text(q, ":name", "rootcluster") || qbind_blob(q,":key",key,key_size) ||
       qbind_int64(q, ":role", ROLE_CLUSTER) ||
       qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE volumes (vid INTEGER PRIMARY KEY NOT NULL, volume TEXT ("STRIFY(SXLIMIT_MAX_VOLNAME_LEN)") NOT NULL UNIQUE, replica INTEGER NOT NULL, revs INTEGER NOT NULL, cursize INTEGER NOT NULL, maxsize INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 0, owner_id INTEGER NOT NULL REFERENCES users(uid))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE vmeta (volume_id INTEGER NOT NULL REFERENCES volumes(vid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY(volume_id, key))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE privs (volume_id INTEGER NOT NULL REFERENCES volumes(vid) ON DELETE CASCADE ON UPDATE CASCADE, user_id INTEGER NOT NULL REFERENCES users(uid) ON DELETE CASCADE, priv INTEGER NOT NULL, PRIMARY KEY (volume_id, user_id))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    qclose(&db);

    /* --- META dbs --- */
    for(i=0; i<METADBS; i++) {
	sprintf(path, "%s/f%08x.db", dir, i);
	sprintf(dbitem, "metadb_%08x", i);
	CREATE_DB(dbitem);
	qnullify(q); /* q is now prepared for hashfs insertions */

	/* Create META tables */
	if(qprep(db, &q, "CREATE TABLE files (fid INTEGER NOT NULL PRIMARY KEY, volume_id INTEGER NOT NULL, name TEXT ("STRIFY(SXLIMIT_MAX_FILENAME_LEN)") NOT NULL, size INTEGER NOT NULL, rev TEXT (56) NOT NULL, content BLOB NOT NULL, UNIQUE(volume_id, name, rev))") || qstep_noret(q))
	    goto create_hashfs_fail;
	qnullify(q);
	if(qprep(db, &q, "CREATE TABLE fmeta (file_id INTEGER NOT NULL REFERENCES files(fid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY(file_id, key))") || qstep_noret(q))
	    goto create_hashfs_fail;
	qnullify(q);
	if(qprep(db, &q, "CREATE TABLE relocs (file_id INTEGER NOT NULL PRIMARY KEY, dest BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL)") || qstep_noret(q)) /* NO FK for better normal use performance */
	    goto create_hashfs_fail;
	qnullify(q);

	qclose(&db);
    }

    /* --- HASH dbs --- */
    for(j = 0; j < SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    int fd;

	    sprintf(path, "%s/h%c%08x.db", dir, sizedirs[j], i);
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    CREATE_DB(dbitem);
	    sqlite3_reset(q); /* q is now prepared for hashfs insertions */
	    if(qbind_text(q, ":k", "block_size") || qbind_int(q, ":v", bsz[j]) || qstep_noret(q))
		goto create_hashfs_fail;
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", "next_blockno") || qbind_int(q, ":v", 1) || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);

	    /* Create HASH tables */
	    if(qprep(db, &q, "CREATE TABLE blocks (\
                id INTEGER PRIMARY KEY NOT NULL,\
                hash BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                blockno INTEGER,\
                created_at INTEGER,\
                UNIQUE(hash))") || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);
            /* if(qprep(db, &q, "CREATE INDEX blockno ON blocks(id, blockno)") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);*/

            if(qprep(db, &q, "CREATE TABLE reservations (\
                blockid INTEGER NOT NULL REFERENCES blocks(id) ON DELETE CASCADE ON UPDATE CASCADE,\
                reserveid BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                ttl INTEGER NOT NULL,\
                PRIMARY KEY(blockid, reserveid))") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);
            if(qprep(db, &q, "CREATE INDEX idx_reserve ON reservations(reserveid, blockid)") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);
            if(qprep(db, &q, "CREATE INDEX idx_res_ttl ON reservations(ttl)") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);

            /* TODO: put this into GC_%x.db
             * >0: inuse
             * <0: delete
             * */
            if(qprep(db, &q, "CREATE TABLE operations (\
                blockid INTEGER NOT NULL REFERENCES blocks(id) ON DELETE CASCADE ON UPDATE CASCADE,\
                tokenid BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                replica INTEGER NOT NULL,\
                op INTEGER NOT NULL,\
                ttl INTEGER NOT NULL,\
                PRIMARY KEY(blockid, tokenid, replica, op))") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);
            if(qprep(db, &q, "CREATE INDEX idx_ttl ON operations(ttl)") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);

            /* age = rebalance/hdist version
             * */
            if(qprep(db, &q, "CREATE TABLE use (\
                blockid INTEGER NOT NULL REFERENCES blocks(id) ON DELETE CASCADE ON UPDATE CASCADE,\
                replica INTEGER NOT NULL,\
                age INTEGER NOT NULL,\
                used INTEGER NOT NULL,\
                PRIMARY KEY(blockid, replica, age))") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);
            if(qprep(db, &q, "CREATE INDEX u0 ON use(blockid, used) WHERE used <= 0") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);

	    /* Create freelist table */
	    if(qprep(db, &q, "CREATE TABLE avail (blocknumber INTEGER NOT NULL PRIMARY KEY ASC)") || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);

	    qclose(&db);

	    /* Create DATA files */
	    sprintf(path, "%s/h%c%08x.bin", dir, sizedirs[j], i);
	    fd = creat(path, 0666);
	    if(fd < 0) {
		PCRIT("Cannot create data file %s", path);
		goto create_hashfs_fail;
	    }
	    memset(path, 0, bsz[j]);
	    sprintf(path, "%-16sdatafile_%c_%08x             %08x", HASHFS_VERSION, sizedirs[j], i, bsz[j]);
	    memcpy(path+64, cluster->binary, sizeof(cluster->binary));
	    if(write_block(fd, path, 0, bsz[j])) {
		close(fd);
		goto create_hashfs_fail;
	    }
	    if(close(fd)) {
		PCRIT("Cannot close data file %s", path);
		goto create_hashfs_fail;
	    }
	}
    }

    /* --- TEMP db --- */
    sprintf(path, "%s/temp.db", dir);
    CREATE_DB("tempdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "CREATE TABLE tmpfiles (tid INTEGER PRIMARY KEY, token TEXT (32) NULL UNIQUE, volume_id INTEGER NOT NULL, name TEXT ("STRIFY(SXLIMIT_MAX_FILENAME_LEN)") NOT NULL, size INTEGER NOT NULL DEFAULT 0, t TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), flushed INTEGER NOT NULL DEFAULT 0, content BLOB, uniqidx BLOB, ttl INTEGER NOT NULL DEFAULT 0, avail BLOB)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if (qprep(db, &q, "CREATE INDEX tmpfiles_ttl ON tmpfiles(ttl) WHERE ttl > 0") || qstep_noret(q))
        goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE tmpmeta (tid INTEGER NOT NULL REFERENCES tmpfiles(tid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY (tid, key))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    /* --- EVENT db --- */
    sprintf(path, "%s/events.db", dir);
    CREATE_DB("eventdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "INSERT INTO hashfs (key, value) VALUES ('next_version_check', datetime(strftime('%s', 'now') + (abs(random()) % 10800), 'unixepoch'))") || qstep_noret(q)) /* Schedule next version check within 3 hours */
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE jobs (job INTEGER NOT NULL PRIMARY KEY, parent INTEGER NULL, type INTEGER NOT NULL, lock TEXT NULL, data BLOB NOT NULL, sched_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), expiry_time TEXT NOT NULL, complete INTEGER NOT NULL DEFAULT 0, result INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT \"\", user INTEGER NULL, UNIQUE(lock))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX jobs_status ON jobs (complete, sched_time)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX jobs_parent ON jobs (parent)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE actions (id INTEGER NOT NULL PRIMARY KEY, job_id INTEGER NOT NULL REFERENCES jobs(job) ON DELETE CASCADE ON UPDATE CASCADE, phase INTEGER NOT NULL DEFAULT 0, target BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, addr TEXT NOT NULL, internaladdr TEXT NOT NULL, capacity INTEGER NOT NULL)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX actions_status ON actions (job_id, phase DESC)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    /* --- XFER db --- */
    sprintf(path, "%s/xfers.db", dir);
    CREATE_DB("xferdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "CREATE TABLE topush (id INTEGER NOT NULL PRIMARY KEY, block BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL, size INTEGER NOT NULL, node BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, sched_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), expiry_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', '"STRIFY(TOPUSH_EXPIRE)" seconds')), UNIQUE (block, size, node))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX topush_sched ON topush(sched_time ASC, expiry_time)") || qstep_noret(q))
        goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE onhold (hid INTEGER NOT NULL PRIMARY KEY, hblock BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL, hsize INTEGER NOT NULL, hnode BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, UNIQUE (hblock, hsize, hnode))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    sync();
    ret = OK;

create_hashfs_fail:
    if (ret != OK)
	WARN("failed to create hashfs");
    sqlite3_finalize(q);
    qclose(&db);
    free(path);
    sxc_free_uri(uri);
    if(ret)
	sxi_rmdirs(dir);
    return ret;
}

static int qopen(const char *path, sxi_db_t **dbp, const char *dbtype, sx_uuid_t *cluster) {
    sqlite3_stmt *q = NULL;
    const char *str;
    sqlite3 *handle = NULL;

    if(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL)) {
	CRIT("Failed to open database %s: %s", path, sqlite3_errmsg(handle));
	goto qopen_fail;
    }
    if (!(*dbp = qnew(handle)))
        goto qopen_fail;
    if(sqlite3_busy_timeout(handle, db_busy_timeout * 1000)) {
	CRIT("Failed to set timeout on database %s: %s", path, sqlite3_errmsg(handle));
	goto qopen_fail;
    }
    if(qprep(*dbp, &q, "PRAGMA synchronous = NORMAL") || qstep_noret(q))
	goto qopen_fail;
    qnullify(q);

    if(qprep(*dbp, &q, "SELECT value FROM hashfs WHERE key = :k"))
	goto qopen_fail;

    if(qbind_text(q, ":k", "version") || qstep_ret(q))
	goto qopen_fail;
    str = (const char *)sqlite3_column_text(q, 0);
    if(!str || strcmp(str, HASHFS_VERSION)) {
	CRIT("Version mismatch on db %s: expected %s, found %s", path, HASHFS_VERSION, str ? str : "none");
	goto qopen_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dbtype") || qstep_ret(q))
	goto qopen_fail;
    str = (const char *)sqlite3_column_text(q, 0);
    if(!str || (dbtype && strcmp(str, dbtype))) {
	CRIT("Type mismatch on db %s: expected %s, found %s", path, dbtype, str ? str : "none");
	goto qopen_fail;
    }

    if(cluster) {
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "cluster") || qstep_ret(q))
	    goto qopen_fail;
	str = (const char *)sqlite3_column_blob(q, 0);
	if(!str || sqlite3_column_bytes(q, 0) != sizeof(cluster->binary) || memcmp(str, cluster->binary, sizeof(cluster->binary))) {
	    sx_uuid_t wrong;
	    uuid_from_binary(&wrong, str);
	    CRIT("Cluster UUID mismatch on db %s: expected %s, found %s", path, cluster->string, wrong.string);
	    goto qopen_fail;
	}
    }

    sqlite3_finalize(q);

    return 0;

qopen_fail:
    WARN("failed to open '%s'", path);
    sqlite3_finalize(q);
    qclose(dbp);
    return 1;
}

struct rebalance_iter {
    unsigned sizeidx;
    unsigned ndbidx;
    unsigned rebalance_ver;
    int64_t ignored;
    int64_t deleted;
    int64_t all;
    unsigned restart_count;
    unsigned iter_no_more;
    sqlite3_stmt *q[SIZES][HASHDBS];
};

struct _sx_hashfs_t {
    uint8_t *blockbuf;

    sxi_db_t *db;
    sqlite3_stmt *q_getval;
    sqlite3_stmt *q_gethdrev;
    sqlite3_stmt *q_getuser;
    sqlite3_stmt *q_getuserbyid;
    sqlite3_stmt *q_getuserbyname;
    sqlite3_stmt *q_listusers;
    sqlite3_stmt *q_listacl;
    sqlite3_stmt *q_createuser;
    sqlite3_stmt *q_deleteuser;
    sqlite3_stmt *q_onoffuser;
    sqlite3_stmt *q_grant;
    sqlite3_stmt *q_getuid;
    sqlite3_stmt *q_getuidname;
    sqlite3_stmt *q_revoke;
    sqlite3_stmt *q_volbyname;
    sqlite3_stmt *q_volbyid;
    sqlite3_stmt *q_metaget;
    sqlite3_stmt *q_nextvol;
    sqlite3_stmt *q_getaccess;
    sqlite3_stmt *q_addvol;
    sqlite3_stmt *q_addvolmeta;
    sqlite3_stmt *q_addvolprivs;
    sqlite3_stmt *q_onoffvol;
    sqlite3_stmt *q_getvolstate;
    sqlite3_stmt *q_delvol;
    sqlite3_stmt *q_chownvol;
    sqlite3_stmt *q_minreqs;
    sqlite3_stmt *q_updatevolcursize;
    sqlite3_stmt *q_setvolcursize;

    sxi_db_t *tempdb;
    sqlite3_stmt *qt_new;
    sqlite3_stmt *qt_update;
    sqlite3_stmt *qt_updateuniq;
    sqlite3_stmt *qt_extend;
    sqlite3_stmt *qt_addmeta;
    sqlite3_stmt *qt_delmeta;
    sqlite3_stmt *qt_getmeta;
    sqlite3_stmt *qt_countmeta;
    sqlite3_stmt *qt_gettoken;
    sqlite3_stmt *qt_tokenstats;
    sqlite3_stmt *qt_tmpdata;
    sqlite3_stmt *qt_delete;
    sqlite3_stmt *qt_flush;
    sqlite3_stmt *qt_gc_tokens;

    sxi_db_t *metadb[METADBS];
    sqlite3_stmt *qm_ins[METADBS];
    sqlite3_stmt *qm_list[METADBS];
    sqlite3_stmt *qm_listrevs[METADBS];
    unsigned char qm_list_done[METADBS];
    uint64_t qm_list_queries;
    sqlite3_stmt *qm_get[METADBS];
    sqlite3_stmt *qm_getrev[METADBS];
    sqlite3_stmt *qm_tooold[METADBS];
    sqlite3_stmt *qm_metaget[METADBS];
    sqlite3_stmt *qm_metaset[METADBS];
    sqlite3_stmt *qm_metadel[METADBS];
    sqlite3_stmt *qm_delfile[METADBS];
    sqlite3_stmt *qm_wiperelocs[METADBS];
    sqlite3_stmt *qm_addrelocs[METADBS];
    sqlite3_stmt *qm_getreloc[METADBS];
    sqlite3_stmt *qm_delreloc[METADBS];
    sqlite3_stmt *qm_delbyvol[METADBS];

    sxi_db_t *datadb[SIZES][HASHDBS];
    sqlite3_stmt *qb_get[SIZES][HASHDBS];
    sqlite3_stmt *qb_nextavail[SIZES][HASHDBS];
    sqlite3_stmt *qb_nextalloc[SIZES][HASHDBS];
    sqlite3_stmt *qb_add[SIZES][HASHDBS];
    sqlite3_stmt *qb_setfree[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc1[SIZES][HASHDBS];
    sqlite3_stmt *qb_bumpavail[SIZES][HASHDBS];
    sqlite3_stmt *qb_bumpalloc[SIZES][HASHDBS];
    sqlite3_stmt *qb_moduse[SIZES][HASHDBS];
    sqlite3_stmt *qb_op[SIZES][HASHDBS];
    sqlite3_stmt *qb_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_del_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_get_meta[SIZES][HASHDBS];
    sqlite3_stmt *qb_deleteold[SIZES][HASHDBS];
    sqlite3_stmt *qb_add_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_unused[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_bad[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_expired_reservation[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_expired_reservation2[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_expired_ops[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_reserve2[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_op[SIZES][HASHDBS];

    sxi_db_t *eventdb;
    sqlite3_stmt *qe_getjob;
    sqlite3_stmt *qe_addjob;
    sqlite3_stmt *qe_addact;
    sqlite3_stmt *qe_countjobs;
    sqlite3_stmt *qe_islocked;
    sqlite3_stmt *qe_hasjobs;
    sqlite3_stmt *qe_lock;
    sqlite3_stmt *qe_unlock;
    int addjob_begun;

    sxi_db_t *xferdb;
    sqlite3_stmt *qx_add;
    sqlite3_stmt *qx_wipehold;
    sqlite3_stmt *qx_hold;
    sqlite3_stmt *qx_isheld;
    sqlite3_stmt *qx_release;
    sqlite3_stmt *qx_hasheld;

    struct timeval jmgr_timestamp;

    char *ssl_ca_file;
    char *cluster_name;
    uint16_t http_port;

    sxi_hdist_t *hd;
    sx_nodelist_t *prev_dist, *next_dist, *nextprev_dist, *prevnext_dist;
    int64_t hd_rev;
    unsigned int have_hd, is_rebalancing, is_orphan;
    time_t last_dist_change;

    sx_hashfs_volume_t curvol;
    sx_uid_t curvoluid;

    sx_hashfs_file_t list_file;
    int list_recurse;
    int64_t list_volid;
    /* 2*SXLIMIT because each char can be a wildcard that might need to be
     * escaped for an exact match */
    char list_pattern[SXLIMIT_MAX_FILENAME_LEN+1];
    char list_pattern_esc[2*SXLIMIT_MAX_FILENAME_LEN+3];
    unsigned int list_pattern_slashes;

    int64_t get_id;
    const sx_hash_t *get_content;
    unsigned int get_nblocks;
    unsigned int get_replica;
    int get_ndb;
    int rev_ndb;


    int64_t put_id;
    int64_t put_extendsize;
    unsigned int put_extendfrom;
    unsigned int put_putblock;
    unsigned int put_getblock;
    unsigned int put_checkblock;
    unsigned int put_singlecheck;
    unsigned int put_replica;
    int64_t upload_minspeed;
    unsigned int put_hs;
    unsigned int put_success;
    sx_hash_t *put_blocks;
    sx_hash_t put_reserve_id;
    unsigned int *put_nidxs;
    unsigned int *put_hashnos;
    unsigned int put_nblocks;
    char put_token[TOKEN_TEXT_LEN + 1];
    struct {
	char key[SXLIMIT_META_MAX_KEY_LEN+1];
	uint8_t value[SXLIMIT_META_MAX_VALUE_LEN];
	int value_len;
    } meta[SXLIMIT_META_MAX_ITEMS];
    unsigned int nmeta;

    unsigned int relocdb_start, relocdb_cur;
    int64_t relocid;


    int datafd[SIZES][HASHDBS];
    sx_uuid_t cluster_uuid, node_uuid; /* MODHDIST: store sx_node_t instead - see sx_hashfs_self */
    char version[16];
    sx_hash_t tokenkey;

    sxc_client_t *sx;
    sxi_conns_t *sx_clust;
    sxi_hashop_t hc;
    char root_auth[AUTHTOK_ASCII_LEN+1];

    int job_trigger, xfer_trigger, gc_trigger, gc_expire_trigger;
    char job_message[JOB_FAIL_REASON_SIZE];

    char *dir;
    int gcver;
    int gc_wal_pages;
    struct rebalance_iter rit;
};

static void close_all_dbs(sx_hashfs_t *h) {
    unsigned int i, j;

    sqlite3_finalize(h->qx_add);
    sqlite3_finalize(h->qx_hold);
    sqlite3_finalize(h->qx_isheld);
    sqlite3_finalize(h->qx_release);
    sqlite3_finalize(h->qx_hasheld);
    sqlite3_finalize(h->qx_wipehold);
    qclose(&h->xferdb);

    sqlite3_finalize(h->qe_getjob);
    sqlite3_finalize(h->qe_addjob);
    sqlite3_finalize(h->qe_addact);
    sqlite3_finalize(h->qe_countjobs);
    sqlite3_finalize(h->qe_islocked);
    sqlite3_finalize(h->qe_hasjobs);
    sqlite3_finalize(h->qe_lock);
    sqlite3_finalize(h->qe_unlock);

    qclose(&h->eventdb);

    for(j=0; j<SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    sqlite3_finalize(h->qb_get[j][i]);
	    sqlite3_finalize(h->qb_nextavail[j][i]);
	    sqlite3_finalize(h->qb_nextalloc[j][i]);
	    sqlite3_finalize(h->qb_add[j][i]);
	    sqlite3_finalize(h->qb_setfree[j][i]);
	    sqlite3_finalize(h->qb_gc1[j][i]);
	    sqlite3_finalize(h->qb_bumpavail[j][i]);
	    sqlite3_finalize(h->qb_bumpalloc[j][i]);
            sqlite3_finalize(h->qb_moduse[j][i]);
            sqlite3_finalize(h->qb_op[j][i]);
            sqlite3_finalize(h->qb_reserve[j][i]);
            sqlite3_finalize(h->qb_del_reserve[j][i]);
            sqlite3_finalize(h->qb_get_meta[j][i]);
            sqlite3_finalize(h->rit.q[j][i]);
            sqlite3_finalize(h->qb_deleteold[j][i]);
            sqlite3_finalize(h->qb_add_reserve[j][i]);
            sqlite3_finalize(h->qb_find_unused[j][i]);
            sqlite3_finalize(h->qb_find_bad[j][i]);
            sqlite3_finalize(h->qb_find_expired_reservation[j][i]);
            sqlite3_finalize(h->qb_find_expired_reservation2[j][i]);
            sqlite3_finalize(h->qb_find_expired_ops[j][i]);
            sqlite3_finalize(h->qb_gc_reserve[j][i]);
            sqlite3_finalize(h->qb_gc_reserve2[j][i]);
            sqlite3_finalize(h->qb_gc_op[j][i]);
	    qclose(&h->datadb[j][i]);

	    if(h->datafd[j][i] >= 0)
		close(h->datafd[j][i]);
	}
    }
    for(i=0; i<METADBS; i++) {
	sqlite3_finalize(h->qm_ins[i]);
	sqlite3_finalize(h->qm_list[i]);
	sqlite3_finalize(h->qm_listrevs[i]);
	sqlite3_finalize(h->qm_get[i]);
	sqlite3_finalize(h->qm_getrev[i]);
	sqlite3_finalize(h->qm_tooold[i]);
	sqlite3_finalize(h->qm_metaget[i]);
	sqlite3_finalize(h->qm_metaset[i]);
	sqlite3_finalize(h->qm_metadel[i]);
	sqlite3_finalize(h->qm_delfile[i]);
	sqlite3_finalize(h->qm_wiperelocs[i]);
	sqlite3_finalize(h->qm_addrelocs[i]);
	sqlite3_finalize(h->qm_getreloc[i]);
	sqlite3_finalize(h->qm_delreloc[i]);
	sqlite3_finalize(h->qm_delbyvol[i]);
	qclose(&h->metadb[i]);
    }

    sqlite3_finalize(h->q_addvol);
    sqlite3_finalize(h->q_addvolmeta);
    sqlite3_finalize(h->q_addvolprivs);
    sqlite3_finalize(h->q_onoffvol);
    sqlite3_finalize(h->q_getvolstate);
    sqlite3_finalize(h->q_delvol);
    sqlite3_finalize(h->q_chownvol);
    sqlite3_finalize(h->q_minreqs);
    sqlite3_finalize(h->q_updatevolcursize);
    sqlite3_finalize(h->q_setvolcursize);
    sqlite3_finalize(h->q_onoffuser);
    sqlite3_finalize(h->q_gethdrev);
    sqlite3_finalize(h->q_getuser);
    sqlite3_finalize(h->q_getuserbyid);
    sqlite3_finalize(h->q_getuserbyname);
    sqlite3_finalize(h->q_listusers);
    sqlite3_finalize(h->q_listacl);
    sqlite3_finalize(h->q_getaccess);
    sqlite3_finalize(h->q_createuser);
    sqlite3_finalize(h->q_deleteuser);
    sqlite3_finalize(h->q_grant);
    sqlite3_finalize(h->q_getuid);
    sqlite3_finalize(h->q_getuidname);
    sqlite3_finalize(h->q_revoke);
    sqlite3_finalize(h->q_nextvol);

    sqlite3_finalize(h->qt_new);
    sqlite3_finalize(h->qt_update);
    sqlite3_finalize(h->qt_updateuniq);
    sqlite3_finalize(h->qt_extend);
    sqlite3_finalize(h->qt_addmeta);
    sqlite3_finalize(h->qt_delmeta);
    sqlite3_finalize(h->qt_getmeta);
    sqlite3_finalize(h->qt_countmeta);
    sqlite3_finalize(h->qt_gettoken);
    sqlite3_finalize(h->qt_tmpdata);
    sqlite3_finalize(h->qt_tokenstats);
    sqlite3_finalize(h->qt_delete);
    sqlite3_finalize(h->qt_flush);
    sqlite3_finalize(h->qt_gc_tokens);

    sqlite3_finalize(h->q_volbyname);
    sqlite3_finalize(h->q_volbyid);
    sqlite3_finalize(h->q_metaget);
    sqlite3_finalize(h->q_getval);
    qclose(&h->tempdb);
    qclose(&h->db);
}

/* TODO: shouldn't use hidden variables */
#define OPEN_DB(DBNAME, DBHANDLE) \
do { \
    sqlite3_reset(h->q_getval); \
    if(qbind_text(h->q_getval, ":k", DBNAME) || qstep_ret(h->q_getval)) {\
	WARN("Couldn't find DB '%s'", DBNAME);				\
	goto open_hashfs_fail; \
    }\
    str = (const char *)sqlite3_column_text(h->q_getval, 0); \
    if(!str) \
	goto open_hashfs_fail; \
    if(*str != '/') { \
	unsigned int subpathlen = strlen(str) + 1; \
	if(subpathlen > pathlen) { \
	    pathlen = subpathlen; \
	    if(!(path = wrap_realloc_or_free(path, dirlen + pathlen))) \
		goto open_hashfs_fail; \
	} \
	memcpy(path + dirlen, str, subpathlen); \
	str = path; \
    } \
    if(qopen(str, DBHANDLE, DBNAME, &h->cluster_uuid)) \
	goto open_hashfs_fail; \
} while(0)


void sx_hashfs_checkpoint_passive(sx_hashfs_t *h)
{
    unsigned i, j;
    /* PASSIVE: doesn't block writers/readers
     * FULL/RESTART: blocks writers (not reader)
     * by default sqlite performs a PASSIVE checkpoint every 1000 pages,
     * and ignores errors
     * */
    qcheckpoint(h->db);
    qcheckpoint(h->tempdb);
    for (i=0;i<METADBS;i++)
        qcheckpoint(h->metadb[i]);
    for (i=0;i<SIZES;i++)
        for (j=0;j<HASHDBS;j++)
            qcheckpoint(h->datadb[i][j]);
    qcheckpoint(h->eventdb);
    qcheckpoint(h->xferdb);
}

void sx_hashfs_checkpoint_gc(sx_hashfs_t *h)
{
}

void sx_hashfs_checkpoint_xferdb(sx_hashfs_t *h)
{
    qcheckpoint_idle(h->xferdb);
}

void sx_hashfs_checkpoint_eventdb(sx_hashfs_t *h)
{
    qcheckpoint_idle(h->eventdb);
    qcheckpoint_idle(h->tempdb);
}

static int load_config(sx_hashfs_t *h, sxc_client_t *sx) {
    const void *p;
    int r, ret = -1;

    DEBUG("Reloading cluster configuration");

    free(h->cluster_name);
    h->cluster_name = NULL;
    free(h->ssl_ca_file);
    h->ssl_ca_file = NULL;
    if(h->have_hd)
	sxi_hdist_free(h->hd);
    h->have_hd = 0;
    h->hd_rev = 0;
    sx_nodelist_delete(h->next_dist);
    h->next_dist = NULL;
    sx_nodelist_delete(h->prev_dist);
    h->prev_dist = NULL;
    sx_nodelist_delete(h->nextprev_dist);
    h->nextprev_dist = NULL;
    sx_nodelist_delete(h->prevnext_dist);
    h->prevnext_dist = NULL;
    h->is_rebalancing = 0;
    h->sx = sx;
    h->last_dist_change = time(NULL);

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "cluster_name"))
	goto load_config_fail;
    switch(qstep(h->q_getval)) {
    case SQLITE_ROW:
	/* MODHDIST: this is an operational node */
	h->cluster_name = wrap_strdup((const char*)sqlite3_column_text(h->q_getval, 0));
	if (!h->cluster_name)
	    goto load_config_fail;

	h->http_port = 0;
	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "http_port")) {
	    CRIT("Failed to retrieve network settings from database");
	    goto load_config_fail;
	}
	r = qstep(h->q_getval);
	if(r == SQLITE_ROW)
	    h->http_port = sqlite3_column_int(h->q_getval, 0);
	else if(r != SQLITE_DONE) {
	    CRIT("Failed to retrieve network settings from database");
	    goto load_config_fail;
	}

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "ssl_ca_file")) {
	    CRIT("Failed to retrieve security certificate from database");
	    goto load_config_fail;
	}

	r = qstep(h->q_getval);
	if(r == SQLITE_ROW) {
            const char *relpath = (const char*)sqlite3_column_text(h->q_getval, 0);
            unsigned cafilen;

	    cafilen = strlen(relpath);
	    if(cafilen) {
		cafilen += strlen(h->dir) + 2;
		h->ssl_ca_file = wrap_malloc(cafilen);
		if(!h->ssl_ca_file)
		    goto load_config_fail;
		if (*relpath == '/')
		    snprintf(h->ssl_ca_file, cafilen, "%s", relpath);
		else
		    snprintf(h->ssl_ca_file, cafilen, "%s/%s", h->dir, relpath);
	    }
	} else if(r != SQLITE_DONE) {
	    CRIT("Failed to retrieve node CA certificate file from database");
	    goto load_config_fail;
	}

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "node") || qstep_ret(h->q_getval)) {
	    CRIT("Failed to retrieve node UUID from database");
	    goto load_config_fail;
	}
	p = sqlite3_column_blob(h->q_getval, 0);
	if(!p || sqlite3_column_bytes(h->q_getval, 0) != sizeof(h->node_uuid.binary)) {
	    CRIT("Bad node UUID retrieved from database");
	    goto load_config_fail;
	}
	uuid_from_binary(&h->node_uuid, p);

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "current_dist_rev")) {
	    CRIT("Failed to retrieve cluster distribution from database");
	    goto load_config_fail;
	}

	r = qstep(h->q_getval);
	if(r == SQLITE_ROW) {
	    h->hd_rev = sqlite3_column_int64(h->q_getval, 0);

	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", "current_dist") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	    if(!sqlite3_column_bytes(h->q_getval, 0)) {
		/* MODHDIST: this node has received its configuration but the hdist model wasn't
		 * enabled yet so we consider it effectively the same as a bare node */
		sqlite3_reset(h->q_getval);
		free(h->cluster_name);
		h->cluster_name = NULL;
		h->hd_rev = 0;
		break;
	    }
	} else if(r == SQLITE_DONE) {
	    if(qbind_text(h->q_getval, ":k", "dist_rev") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	    h->hd_rev = sqlite3_column_int64(h->q_getval, 0);

	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", "dist") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	} else {
	    CRIT("Failed to retrieve cluster distribution from database");
	    goto load_config_fail;
	}
	p = sqlite3_column_blob(h->q_getval, 0);
	if(!p) {
	    CRIT("Bad cluster distribution retrieved from database");
	    goto load_config_fail;
	}
	if(!(h->hd = sxi_hdist_from_cfg(p, sqlite3_column_bytes(h->q_getval, 0)))) {
	    CRIT("Failed to load cluster distribution");
	    goto load_config_fail;
	}

	h->next_dist = sx_nodelist_dup(sxi_hdist_nodelist(h->hd, 0));
	if(sxi_hdist_buildcnt(h->hd) == 1) {
	    h->prev_dist = sx_nodelist_dup(h->next_dist);
	    if(!sx_nodelist_lookup(h->next_dist, &h->node_uuid)) {
		INFO("THIS NODE IS NO LONGER A CLUSTER MEMBER");
		h->is_orphan = 1;
	    }
	} else if(sxi_hdist_buildcnt(h->hd) == 2) {
	    h->prev_dist = sx_nodelist_dup(sxi_hdist_nodelist(h->hd, 1));
	    h->is_rebalancing = 1;
	} else {
	    CRIT("Failed to load cluster distribution: too many models");
	    goto load_config_fail;
	}

	h->nextprev_dist = sx_nodelist_dup(h->next_dist);
	h->prevnext_dist = sx_nodelist_dup(h->prev_dist);
	if(!h->next_dist || !h->prev_dist ||
	   sx_nodelist_addlist(h->nextprev_dist, h->prev_dist) ||
	   sx_nodelist_addlist(h->prevnext_dist, h->next_dist)) {
	    CRIT("Failed to allocate cluster distribution lists");
	    goto load_config_fail;
	}

	sqlite3_reset(h->q_getval);

	if(!h->sx_clust) {
	    h->sx_clust = sxi_conns_new(sx);
            sxi_conns_disable_proxy(h->sx_clust);
        }
	if(!h->sx_clust ||
	   sxi_conns_set_uuid(h->sx_clust, h->cluster_uuid.string) ||
	   sxi_conns_set_auth(h->sx_clust, h->root_auth) ||
	   sxi_conns_set_port(h->sx_clust, h->http_port) ||
	   sxi_conns_set_sslname(h->sx_clust, h->cluster_name)) {
	    CRIT("Failed to initialize cluster connectors");
	    goto load_config_fail;
	}
	sxi_conns_set_cafile(h->sx_clust, h->ssl_ca_file);

	h->have_hd = 1;
	break;

    case SQLITE_DONE:
	/* MODHDIST: this is a bare node which cannot operate until programmed */
	h->cluster_name = NULL;
	break;

    default:
	CRIT("Failed to retrieve cluster name from database");
	goto load_config_fail;
    }

    ret = 0;
 load_config_fail:
    sqlite3_reset(h->q_getval);
    return ret;
}



sx_hashfs_t *sx_hashfs_open(const char *dir, sxc_client_t *sx) {
    unsigned int dirlen, pathlen, i, j;
    sqlite3_stmt *q = NULL;
    char *path, dbitem[64], dynqry[128];
    const char *str;
    sx_hashfs_t *h;

    if(!dir || !(dirlen = strlen(dir))) {
	CRIT("Bad path");
	return NULL;
    }
    if (ssl_version_check())
	return NULL;

    if(!(h = wrap_calloc(1, sizeof(*h))))
	return NULL;
    memset(h->datafd, -1, sizeof(h->datafd));
    h->sx = NULL;
    h->job_trigger = h->xfer_trigger = h->gc_trigger = h->gc_expire_trigger = -1;
    /* TODO: read from hashfs kv store */
    h->upload_minspeed = GC_UPLOAD_MINSPEED;

    dirlen++;
    pathlen = 1024;
    if(!(path = wrap_malloc(dirlen + pathlen)))
	goto open_hashfs_fail;
    h->dir = strdup(dir);
    if (!h->dir)
        goto open_hashfs_fail;

    if (!qlog_set) {
	sqlite3_config(SQLITE_CONFIG_LOG, qlog, NULL);
	qlog_set = 1;
    }

    gettimeofday(&h->jmgr_timestamp, NULL);

    /* reset sqlite3's PRNG, to avoid generating colliding tempfile names in
     * forked processes */
    sqlite3_initialize();
    sqlite3_test_control(SQLITE_TESTCTRL_PRNG_RESET);
    /* reset OpenSSL's PRNG otherwise it'll share state after a fork */
    sxi_rand_cleanup();

    sprintf(path, "%s/hashfs.db", dir);
    if(qopen(path, &h->db, "hashfs", NULL))
	goto open_hashfs_fail;
    if(qprep(h->db, &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	goto open_hashfs_fail;
    qnullify(q);
    if(qprep(h->db, &h->q_getval, "SELECT value FROM hashfs WHERE key = :k"))
	goto open_hashfs_fail;
    if(qbind_text(h->q_getval, ":k", "cluster") || qstep_ret(h->q_getval))
	goto open_hashfs_fail;
    str = (const char *)sqlite3_column_blob(h->q_getval, 0);
    if(!str || sqlite3_column_bytes(h->q_getval, 0) != sizeof(h->cluster_uuid.binary)) {
	CRIT("Failed to retrieve cluster UUID from database");
	goto open_hashfs_fail;
    }
    uuid_from_binary(&h->cluster_uuid, str);

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "version") || qstep_ret(h->q_getval))
	goto open_hashfs_fail;
    str = (const char *)sqlite3_column_text(h->q_getval, 0);
    if(!str || strlen(str) >= sizeof(h->version)) {
	CRIT("Failed to retrieve HashFS version from database");
	goto open_hashfs_fail;
    }
    strcpy(h->version, str);

    if(qprep(h->db, &q, "SELECT key FROM users WHERE uid = 0 AND role = "STRIFY(ROLE_CLUSTER)" AND enabled = 1") || qstep_ret(q)) {
	CRIT("Failed to retrieve cluster key from database");
	goto open_hashfs_fail;
    }
    if(!(str = sqlite3_column_blob(q, 0)) || sqlite3_column_bytes(q, 0) != AUTH_KEY_LEN) {
	CRIT("Bad cluster key retrieved from database");
	goto open_hashfs_fail;
    }
    if(encode_auth_bin(CLUSTER_USER, (const unsigned char *)str, AUTH_KEY_LEN, h->root_auth, sizeof(h->root_auth))) {
	CRIT("Failed to encode cluster key");
	goto open_hashfs_fail;
    }
    if(hash_buf("", 0, str, AUTH_KEY_LEN, &h->tokenkey)) {
	CRIT("Failed to generate token key");
	goto open_hashfs_fail;
    }
    qnullify(q);
    if(load_config(h, sx))
	goto open_hashfs_fail;

    if(qprep(h->db, &h->q_gethdrev, "SELECT MIN(value) FROM hashfs WHERE key IN ('current_dist_rev','dist_rev')"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuser, "SELECT uid, key, role FROM users WHERE user = :user AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuserbyid, "SELECT user FROM users WHERE uid = :uid AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuserbyname, "SELECT user FROM users WHERE name = :name AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_listusers, "SELECT uid, name, user, key, role FROM users WHERE uid > :lastuid AND enabled=1 ORDER BY uid ASC LIMIT 1"))
	goto open_hashfs_fail;
    /* FIXME: this query is broken and should be rewritten - index usage not checked */
    /* e.g.:
     * $ sxacl list sx://admin@local/r2
     * admin: read write owner
     * acab: read write
     * admin: read write owner
     * adm: read write owner
     */
    if(qprep(h->db, &h->q_listacl, "SELECT name, priv, uid, owner_id FROM privs, volumes INNER JOIN users ON user_id=uid WHERE volume_id=:volid AND vid=:volid AND volumes.enabled = 1 AND users.enabled = 1 AND (priv <> 0 OR owner_id=uid) AND user_id > :lastuid ORDER BY user_id ASC LIMIT 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getaccess, "SELECT privs.priv, volumes.owner_id FROM privs, volumes, users WHERE privs.volume_id = :volume AND privs.user_id = :user AND volumes.vid = :volume AND volumes.enabled = 1 AND users.uid = :user AND users.enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_createuser, "INSERT INTO users(user, name, key, role) VALUES(:userhash,:name,:key,:role)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_deleteuser, "DELETE FROM users WHERE uid = :uid"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_onoffuser, "UPDATE users SET enabled = :enable WHERE name = :username"))
	goto open_hashfs_fail;
    /* update if present otherwise insert:
     * note: the read and write has to be in same transaction otherwise
     * there'd be race conditions.
     * */
    if(qprep(h->db, &h->q_grant, "INSERT OR REPLACE INTO privs(volume_id, user_id, priv)\
	     VALUES(:volid, :uid,\
		    COALESCE((SELECT priv FROM privs WHERE volume_id=:volid AND user_id=:uid), 0)\
		    | :priv)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuid, "SELECT uid, role FROM users WHERE name = :name AND (:inactivetoo OR enabled=1)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuidname, "SELECT name FROM users WHERE uid = :uid AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_revoke, "REPLACE INTO privs(volume_id, user_id, priv)\
	     VALUES(:volid, :uid,\
		    COALESCE((SELECT priv FROM privs WHERE volume_id=:volid AND user_id=:uid), 0)\
		    & :privmask)"))
	goto open_hashfs_fail;
    /* To keep the next query simple we do not check if the user is enabled
     * This is preliminary enforced in auth_begin */
    if(qprep(h->db, &h->q_nextvol, "SELECT volumes.vid, volumes.volume, volumes.replica, volumes.cursize, volumes.maxsize, volumes.owner_id, volumes.revs FROM volumes LEFT JOIN privs ON privs.volume_id = volumes.vid WHERE volumes.volume > :previous AND volumes.enabled = 1 AND (:user = 0 OR (privs.priv > 0 AND privs.user_id=:user)) ORDER BY volumes.volume ASC LIMIT 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_volbyname, "SELECT vid, volume, replica, cursize, maxsize, owner_id, revs FROM volumes WHERE volume = :name AND enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_volbyid, "SELECT vid, volume, replica, cursize, maxsize, owner_id, revs FROM volumes WHERE vid = :volid AND enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_metaget, "SELECT key, value FROM vmeta WHERE volume_id = :volume"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvol, "INSERT INTO volumes (volume, replica, revs, cursize, maxsize, owner_id) VALUES (:volume, :replica, :revs, 0, :size, :owner)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvolmeta, "INSERT INTO vmeta (volume_id, key, value) VALUES (:volume, :key, :value)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvolprivs, "INSERT INTO privs (volume_id, user_id, priv) VALUES (:volume, :user, :priv)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_onoffvol, "UPDATE volumes SET enabled = :enable WHERE volume = :volume"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getvolstate, "SELECT enabled FROM volumes WHERE volume = :volume"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_delvol, "DELETE FROM volumes WHERE volume = :volume AND enabled = 0"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_chownvol, "UPDATE volumes SET owner_id = :new WHERE owner_id = :old"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_minreqs, "SELECT COALESCE(MAX(replica), 1), COALESCE(SUM(maxsize*replica), 0) FROM volumes"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_updatevolcursize, "UPDATE volumes SET cursize = cursize + :size WHERE vid = :volume"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_setvolcursize, "UPDATE volumes SET cursize = :size WHERE vid = :volume"))
        goto open_hashfs_fail;

    OPEN_DB("tempdb", &h->tempdb);
    /* needed for ON DELETE CASCADE to work */
    if(qprep(h->tempdb, &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	goto open_hashfs_fail;
    qnullify(q);

    if(qprep(h->tempdb, &h->qt_new, "INSERT INTO tmpfiles (volume_id, name, token) VALUES (:volume, :name, lower(hex(:random)))"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_update, "UPDATE tmpfiles SET size = :size, content = :all, uniqidx = :uniq, ttl = :expiry WHERE tid = :id AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_extend, "UPDATE tmpfiles SET content = cast((content || :all) as blob), uniqidx = cast((uniqidx || :uniq) as blob) WHERE tid = :id AND length(content) = :size AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_addmeta, "INSERT OR REPLACE INTO tmpmeta (tid, key, value) VALUES (:id, :key, :value)"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_delmeta, "DELETE FROM tmpmeta WHERE tid = :id AND key = :key"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_getmeta, "SELECT key, value FROM tmpmeta WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_countmeta, "SELECT COUNT(*) FROM tmpmeta WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_gettoken, "SELECT token, ttl, volume_id, name FROM tmpfiles WHERE tid = :id AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_tokenstats, "SELECT tid, size, volume_id, length(content) FROM tmpfiles WHERE token = :token AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_tmpdata, "SELECT t || ':' || token AS revision, name, size, volume_id, content, uniqidx, flushed, avail, token FROM tmpfiles WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_updateuniq, "UPDATE tmpfiles SET uniqidx = :uniq, avail = :avail WHERE tid = :id AND flushed = 1"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_flush, "UPDATE tmpfiles SET flushed = 1 WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_delete, "DELETE FROM tmpfiles WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_gc_tokens, "DELETE FROM tmpfiles WHERE ttl < :now AND ttl > 0"))
	goto open_hashfs_fail;

    if(!(h->blockbuf = wrap_malloc(bsz[SIZES-1])))
	goto open_hashfs_fail;

    for(j=0; j<SIZES; j++) {
	char hexsz[9];
	sprintf(hexsz, "%08x", bsz[j]);
	for(i=0; i<HASHDBS; i++) {
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    OPEN_DB(dbitem, &h->datadb[j][i]);
            if(qprep(h->datadb[j][i], &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
                goto open_hashfs_fail;
            qnullify(q);
	    if(qprep(h->datadb[j][i], &h->qb_nextavail[j][i], "SELECT blocknumber FROM avail ORDER BY blocknumber ASC LIMIT 1"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_nextalloc[j][i], "SELECT value FROM hashfs WHERE key = 'next_blockno'"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_add[j][i], "UPDATE blocks SET blockno=:next, created_at = :now WHERE hash=:hash"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_setfree[j][i], "INSERT OR IGNORE INTO avail VALUES(:blockno)"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_gc1[j][i], "DELETE FROM blocks WHERE id = :blockid"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_get[j][i], "SELECT blockno FROM blocks WHERE hash = :hash AND blockno IS NOT NULL"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_bumpavail[j][i], "DELETE FROM avail WHERE blocknumber = :next"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_bumpalloc[j][i], "UPDATE hashfs SET value = value + 1 WHERE key = 'next_blockno'"))
		goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_moduse[j][i],
                     "INSERT OR REPLACE INTO use(blockid, replica, age, used)\
                     SELECT id, :replica, :age, \
                        COALESCE((SELECT used + :used FROM use WHERE blockid=id AND replica=:replica AND age=:age), :used)\
                    FROM blocks WHERE hash = :hash"))/* AND blockno IS NOT NULL */
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_op[j][i], "INSERT OR IGNORE INTO operations(blockid, tokenid, replica, op, ttl) SELECT id, :tokenid, :replica, :op, :ttl FROM blocks WHERE hash = :hash"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_reserve[j][i], "INSERT OR REPLACE INTO reservations(blockid, reserveid, ttl) SELECT id, :reserveid, :ttl FROM blocks WHERE hash = :hash"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_del_reserve[j][i], "DELETE FROM reservations WHERE reserveid=:reserveid AND blockid=(SELECT id FROM blocks WHERE hash=:hash)"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_get_meta[j][i], "SELECT replica, used FROM use WHERE blockid=:blockid AND age < :current_age"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->rit.q[j][i], "SELECT id, hash FROM blocks WHERE hash > :prevhash AND blockno IS NOT NULL"))
                goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_add_reserve[j][i], "INSERT OR IGNORE INTO blocks (hash) VALUES (:hash)"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_find_unused[j][i], "SELECT id, blockno, hash FROM blocks LEFT JOIN use ON use.blockid=id AND used<>0 LEFT JOIN reservations ON reservations.blockid=id WHERE id > :last AND reservations.blockid IS NULL GROUP BY id HAVING SUM(used)=0 OR COUNT(use.blockid)=0 ORDER BY id;"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_find_bad[j][i], "SELECT COUNT(blockid) FROM use WHERE used <= 0 AND used <> 0"))
		goto open_hashfs_fail;
            /* hash moved,
             * hashes that are not moved don't have the old counters deleted,
             * and must be taken into account when GCing!
             * this is to avoid updating the entire table during rebalance
             * */
            if(qprep(h->datadb[j][i], &h->qb_deleteold[j][i], "DELETE FROM use WHERE blockid=(SELECT id FROM blocks WHERE hash=:hash AND blockno IS NOT NULL) AND age < :current_age"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_find_expired_reservation[j][i], "SELECT reserveid FROM reservations INNER JOIN blocks ON blockid=id AND created_at IS NOT NULL WHERE reserveid > :lastreserveid GROUP BY reserveid HAVING created_at < :expires OR created_at IS NULL ORDER BY reserveid LIMIT 1"))
               goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_find_expired_reservation2[j][i], "SELECT ROWID from reservations WHERE ttl < :now AND ROWID>:lastrowid LIMIT 1"))
               goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_find_expired_ops[j][i],"SELECT ROWID FROM operations WHERE ttl < :now LIMIT 1"))
               goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_reserve[j][i], "DELETE FROM reservations WHERE reserveid=:reserveid"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_reserve2[j][i], "DELETE FROM reservations WHERE ROWID=:rowid"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_op[j][i], "DELETE FROM operations WHERE ROWID=:rowid"))
                goto open_hashfs_fail;

	    sprintf(dbitem, "datafile_%c_%08x", sizedirs[j], i);
	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", dbitem) || qstep_ret(h->q_getval))
		goto open_hashfs_fail;

	    str = (const char *)sqlite3_column_text(h->q_getval, 0);
	    if(!str || !*str)
		goto open_hashfs_fail;
	    if(*str != '/') {
		unsigned int subpathlen = strlen(str) + 1;
		if(subpathlen > pathlen) {
		    pathlen = subpathlen;
		    if(!(path = wrap_realloc_or_free(path, dirlen + pathlen)))
			goto open_hashfs_fail;
		}
		memcpy(path + dirlen, str, subpathlen);
		str = path;
	    }

	    h->datafd[j][i] = open(str, O_RDWR);
	    if(h->datafd[j][i] < 0) {
		perror("open");
		goto open_hashfs_fail;
	    }
	    if(read_block(h->datafd[j][i], h->blockbuf, 0, bsz[j]))
		goto open_hashfs_fail;
	    if(memcmp(h->blockbuf, HASHFS_VERSION, strlen(HASHFS_VERSION)) ||
	       memcmp(h->blockbuf + 16, dbitem, strlen(dbitem)) ||
	       memcmp(h->blockbuf + 48, hexsz, strlen(hexsz)) ||
	       memcmp(h->blockbuf + 64, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary))) {
		CRIT("Bad header in datafile %s", str);
		goto open_hashfs_fail;
	    }
	}
    }

    for(i=0; i<METADBS; i++) {
	sprintf(dbitem, "metadb_%08x", i);
	OPEN_DB(dbitem, &h->metadb[i]);
	if(qprep(h->metadb[i], &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	    goto open_hashfs_fail;
	qnullify(q);
	if(qprep(h->metadb[i], &h->qm_ins[i], "INSERT INTO files (volume_id, name, size, content, rev) VALUES (:volume, :name, :size, :hashes, :revision)"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_list[i], "SELECT name, size, rev FROM files WHERE volume_id = :volume AND name > :previous GROUP BY name HAVING rev = MAX(rev) ORDER BY name ASC LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_listrevs[i], "SELECT size, rev FROM files WHERE volume_id = :volume AND name = :name AND rev > :previous ORDER BY rev ASC LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_get[i], "SELECT fid, size, content, rev, size FROM files WHERE volume_id = :volume AND name = :name GROUP BY name HAVING rev = MAX(rev) LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_getrev[i], "SELECT fid, size, content, rev, size FROM files WHERE volume_id = :volume AND name = :name AND rev = :revision LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_tooold[i], "SELECT fid, rev, COUNT(*) AS nrevs, size FROM files WHERE volume_id = :volume AND name = :name GROUP BY name HAVING rev = MIN(rev) LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metaget[i], "SELECT key, value FROM fmeta WHERE file_id = :file"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metaset[i], "INSERT OR REPLACE INTO fmeta (file_id, key, value) VALUES (:file, :key, :value)"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metadel[i], "DELETE FROM fmeta WHERE file_id = :file AND key = :key"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delfile[i], "DELETE FROM files WHERE fid = :file"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_wiperelocs[i], "DELETE FROM relocs"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_addrelocs[i], "INSERT INTO relocs (file_id, dest) SELECT fid, :node FROM files WHERE volume_id = :volid"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_getreloc[i], "SELECT file_id, dest, volume_id, name, size, rev, content FROM relocs LEFT JOIN files ON relocs.file_id = files.fid WHERE file_id > :prev"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delreloc[i], "DELETE FROM relocs WHERE file_id = :fileid"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delbyvol[i], "DELETE FROM files WHERE volume_id = :volid"))
	    goto open_hashfs_fail;
    }

    OPEN_DB("eventdb", &h->eventdb);
    if(qprep(h->eventdb, &h->qe_getjob, "SELECT complete, result, reason FROM jobs WHERE job = :id AND :owner IN (user, 0)"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_addjob, "INSERT INTO jobs (parent, type, lock, expiry_time, data, user) SELECT :parent, :type, :lock, datetime(:expiry + strftime('%s', COALESCE((SELECT expiry_time FROM jobs WHERE job = :parent), 'now')), 'unixepoch'), :data, :uid"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_addact, "INSERT INTO actions (job_id, target, addr, internaladdr, capacity) VALUES (:job, :node, :addr, :int_addr, :capa)"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_countjobs, "SELECT COUNT(*) FROM jobs WHERE user = :uid AND complete = 0"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_islocked, "SELECT value from hashfs WHERE key = 'lockedby'"))
	goto open_hashfs_fail;
    snprintf(dynqry, sizeof(dynqry), "SELECT 1 FROM jobs WHERE complete = 0 AND type NOT IN (%d, %d, %d, %d) LIMIT 1", JOBTYPE_DISTRIBUTION, JOBTYPE_JLOCK, JOBTYPE_STARTREBALANCE, JOBTYPE_FINISHREBALANCE);
    if(qprep(h->eventdb, &h->qe_hasjobs, dynqry))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_lock, "INSERT INTO hashfs (key, value) VALUES ('lockedby', :node)"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_unlock, "DELETE FROM hashfs WHERE key = 'lockedby'"))
	goto open_hashfs_fail;

    OPEN_DB("xferdb", &h->xferdb);
    if(qprep(h->xferdb, &h->qx_add, "INSERT INTO topush (block, size, node) VALUES (:b, :s, :n)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_hold, "INSERT OR IGNORE INTO onhold (hblock, hsize, hnode) VALUES (:b, :s, :n)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_isheld, "SELECT 1 FROM onhold WHERE hblock = :b AND hsize = :s"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_release, "DELETE FROM onhold WHERE hid IN (SELECT hid FROM onhold, topush WHERE id = :pushid AND hblock = block AND hsize = size AND hnode = node)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_hasheld, "SELECT 1 FROM onhold LIMIT 1"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_wipehold, "DELETE FROM onhold"))
       goto open_hashfs_fail;


    qnullify(q);
    sqlite3_reset(h->q_getval);

    free(path);
    return h;

open_hashfs_fail:
    free(path);
    sqlite3_finalize(q);

    close_all_dbs(h);
    sqlite3_shutdown();
    free(h->blockbuf);
    free(h);
    return NULL;
}

int sx_hashfs_distcheck(sx_hashfs_t *h) {
    int ret = 0;

    if(!h)
	return 0;

    sqlite3_reset(h->q_gethdrev);
    switch(qstep(h->q_gethdrev)) {
    case SQLITE_DONE:
	break;
    case SQLITE_ROW:
	if(sqlite3_column_int64(h->q_gethdrev, 0) != h->hd_rev)
	    ret = 1;
	break;
    default:
	WARN("Failed to check distribution version, assuming unchanged");
    }
    sqlite3_reset(h->q_gethdrev);

    if(ret && load_config(h, h->sx))
	ret = -1;

    return ret; /* return 0 = no change, 1 = hdist-change, -1 = error */
}

time_t sx_hashfs_disttime(sx_hashfs_t *h) {
    return h->last_dist_change;
}

const char *sx_hashfs_cluster_name(sx_hashfs_t *h) {
    return h ? h->cluster_name : NULL;
}

uint16_t sx_hashfs_http_port(sx_hashfs_t *h) {
    return h ? h->http_port : 0;
}

const char *sx_hashfs_ca_file(sx_hashfs_t *h) {
    return h ? h->ssl_ca_file : NULL;
}

int sx_hashfs_uses_secure_proto(sx_hashfs_t *h) {
    if(!h)
	return -1;
    return (h->ssl_ca_file != NULL);
}

void sx_hashfs_set_triggers(sx_hashfs_t *h, int job_trigger, int xfer_trigger, int gc_trigger, int gc_expire_trigger) {
    if(!h)
	return;
    h->job_trigger = job_trigger;
    h->xfer_trigger = xfer_trigger;
    h->gc_trigger = gc_trigger;
    h->gc_expire_trigger = gc_expire_trigger;
}

void sx_hashfs_close(sx_hashfs_t *h) {
    if(!h)
	return;
    if(h->have_hd)
	sxi_hdist_free(h->hd);
    sx_nodelist_delete(h->prev_dist);
    sx_nodelist_delete(h->next_dist);
    sx_nodelist_delete(h->prevnext_dist);
    sx_nodelist_delete(h->nextprev_dist);

    close_all_dbs(h);

    free(h->blockbuf);
/*    if(h->sx)
	sx_shutdown(h->sx, 0);
    do not free sx here: it is not owned by hashfs.c!
    freeing it here would cause use-after-free if sx_done() is called
    */
    if(h->sx_clust)
	sxi_conns_free(h->sx_clust);
    sqlite3_shutdown();
    free(h->ssl_ca_file);
    free(h->cluster_name);
    free(h->dir);
    free(h);
}

int sx_storage_is_bare(sx_hashfs_t *h) {
    return (h != NULL) && (h->cluster_name == NULL);
}

rc_ty sx_storage_activate(sx_hashfs_t *h, const char *name, const sx_uuid_t *node_uuid, uint8_t *admin_uid, unsigned int uid_size, uint8_t *admin_key, int key_size, uint16_t port, const char *ssl_ca_file, const sx_nodelist_t *allnodes) {
    rc_ty r, ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    const sx_node_t *self;
    unsigned int nodeidx;

    if(!h || !name || !node_uuid || !admin_key) {
	NULLARG();
	return EFAULT;
    }
    if(!sx_storage_is_bare(h)) {
	msg_set_reason("Storage was already activated");
	return EINVAL;
    }

    self = sx_nodelist_lookup_index(allnodes, node_uuid, &nodeidx);
    if(!self) {
	msg_set_reason("Failed to find node uuid in node list");
	return EINVAL;
    }

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto storage_activate_fail;

    r = sx_hashfs_create_user(h, "admin", admin_uid, uid_size, admin_key, key_size, ROLE_ADMIN);
    if(r != OK) {
	ret = r;
	goto storage_activate_fail;
    }
    r = sx_hashfs_user_onoff(h, "admin", 1);
    if(r != OK) {
	ret = r;
	goto storage_activate_fail;
    }

    if(ssl_ca_file) {
	if(qbind_text(q, ":k", "ssl_ca_file") || qbind_text(q, ":v", ssl_ca_file) || qstep_noret(q))
	    goto storage_activate_fail;
    }
    if(qbind_text(q, ":k", "cluster_name") || qbind_text(q, ":v", name) || qstep_noret(q))
	goto storage_activate_fail;
    if(qbind_text(q, ":k", "node") || qbind_blob(q, ":v", node_uuid->binary, sizeof(node_uuid->binary)) || qstep_noret(q))
	goto storage_activate_fail;
    if(qbind_text(q, ":k", "http_port") || qbind_int(q, ":v", port) || qstep_noret(q))
	goto storage_activate_fail;

    if(sx_hashfs_hdist_change_commit(h))
	goto storage_activate_fail;

    if(sx_hashfs_modhdist(h, allnodes))
	goto storage_activate_fail;

    if(qcommit(h->db))
	goto storage_activate_fail;

    ret = OK;
 storage_activate_fail:
    if(ret != OK)
	qrollback(h->db);

    sqlite3_finalize(q);
    return ret;
}

static unsigned int size_to_blocks(uint64_t size, unsigned int *size_type, unsigned int *block_size) {
    unsigned int ret, sizenum = 1, bs;
    if(size > 128*1024*1024)
	sizenum = 2;
    else if(size < 128*1024)
	sizenum = 0;
    bs = bsz[sizenum];
    ret = size / bs;
    if(size % bs)
	ret++;
    if(size_type)
	*size_type = sizenum;
    if(block_size)
	*block_size = bs;
    return ret;
}

static int cmphash(const void *a, const void *b) {
    return memcmp(a, b, sizeof(sx_hash_t));
}

static unsigned int gethashdb(const sx_hash_t *hash) {
    return MurmurHash64(hash, sizeof(*hash), MURMUR_SEED) & (HASHDBS-1);
}

static int getmetadb(const char *filename) {
    sx_hash_t hash;
    if(hash_buf(NULL, 0, filename, strlen(filename), &hash))
	return -1;

    return MurmurHash64(&hash, sizeof(hash), MURMUR_SEED) & (METADBS-1);
}

/* Returns 0 if the volume name is valid,
 * sets reason otherwise */
rc_ty sx_hashfs_check_volume_name(const char *name) {
    unsigned int namelen;
    if (!name) {
	NULLARG();
	return EFAULT;
    }
    if(*name=='.') {
	msg_set_reason("Invalid volume name '%s': must not start with a '.'", name);
	return EINVAL;
    }
    namelen = strlen(name);
    if(namelen < SXLIMIT_MIN_VOLNAME_LEN || namelen > SXLIMIT_MAX_VOLNAME_LEN) {
	msg_set_reason("Invalid volume name '%s': must be between %d and %d bytes",
		       name, SXLIMIT_MIN_VOLNAME_LEN, SXLIMIT_MAX_VOLNAME_LEN);
	return EINVAL;
    }
    if(utf8_validate_len(name) < 0) {
	msg_set_reason("Invalid volume name '%s': must be valid UTF8", name);
	return EINVAL;
    }
    return 0;
}

static int check_file_name(const char *name) {
    unsigned int namelen;
    if(!name) {
	NULLARG();
	return -1;
    }
    namelen = strlen(name);
    if(namelen < SXLIMIT_MIN_FILENAME_LEN || namelen > SXLIMIT_MAX_FILENAME_LEN) {
	msg_set_reason("Invalid file name '%s': must be between %d and %d bytes",
		       name, SXLIMIT_MIN_FILENAME_LEN, SXLIMIT_MAX_FILENAME_LEN);
	return -1;
    }
    if(utf8_validate_len(name) < 0) {
	msg_set_reason("Invalid file name '%s': must be valid UTF8", name);
	return -1;
    }
    return namelen;
}

rc_ty sx_hashfs_check_meta(const char *key, const void *value, unsigned int value_len) {
    unsigned int key_len;

    if(!key || !value) {
	NULLARG();
	return EFAULT;
    }

    key_len = strlen(key);
    if(key_len < SXLIMIT_META_MIN_KEY_LEN) {
	msg_set_reason("Invalid metadata key length %d: must be between %d and %d",
		       key_len, SXLIMIT_META_MIN_KEY_LEN, SXLIMIT_META_MAX_KEY_LEN);
	return EINVAL;
    }
    if(key_len > SXLIMIT_META_MAX_KEY_LEN) {
	msg_set_reason("Invalid metadata key length %d: must be between %d and %d",
		       key_len, SXLIMIT_META_MIN_KEY_LEN, SXLIMIT_META_MAX_KEY_LEN);
	return EMSGSIZE;
    }
    if (SXLIMIT_META_MIN_VALUE_LEN > 0 && value_len < SXLIMIT_META_MIN_VALUE_LEN) {
	msg_set_reason("Invalid metadata value length %d: must be between %d and %d",
		       value_len, SXLIMIT_META_MIN_VALUE_LEN, SXLIMIT_META_MAX_VALUE_LEN);
	return EINVAL;
    }
    if (value_len > SXLIMIT_META_MAX_VALUE_LEN) {
	msg_set_reason("Invalid metadata value length %d: must be between %d and %d",
		       value_len, SXLIMIT_META_MIN_VALUE_LEN, SXLIMIT_META_MAX_VALUE_LEN);
	return EMSGSIZE;
    }

    if (utf8_validate_len(key) < 0) {
	msg_set_reason("Invalid metadata key '%s': must be valid UTF8", key);
	return EINVAL;
    }
    return 0;
}

int sx_hashfs_check_username(const char *name) {
    unsigned int namelen;
    if(!name)
	return -1;
    namelen = strlen(name);
    if(namelen < SXLIMIT_MIN_USERNAME_LEN || namelen > SXLIMIT_MAX_USERNAME_LEN)
	return -1;
    if(utf8_validate_len(name) < 0)
	return 1;
    return 0;
}


static int parse_revision(const char *revision, unsigned int *revtime) {
    const char *eod;
    time_t t;

    if(!revision)
	return -1;
    if(strlen(revision) != REV_LEN)
	return -1;
    if(revision[REV_TIME_LEN] != ':')
	return -1;

    eod = strptimegm(revision, "%Y-%m-%d %H:%M:%S", &t);
    if(eod != &revision[REV_TIME_LEN - 4])
	return -1;
    if(revtime)
	*revtime = (unsigned int)t;
    return 0;
}

static int check_revision(const char *revision) {
    return parse_revision(revision, NULL);
}

#define TOKEN_SIGNED_LEN UUID_STRING_SIZE + 1 + TOKEN_RAND_BYTES * 2 + 1 + TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN + 1
rc_ty sx_hashfs_make_token(sx_hashfs_t *h, const uint8_t *user, const char *rndhex, unsigned int replica, int64_t expires_at, const char **token) {
    sxi_hmac_sha1_ctx *hmac_ctx;
    uint8_t md[SXI_SHA1_BIN_LEN], rndbin[TOKEN_RAND_BYTES];
    char rndhexbuf[TOKEN_RAND_BYTES * 2 + 1], replicahex[2 + TOKEN_REPLICA_LEN + 1], expirehex[TOKEN_EXPIRE_LEN + 1];
    sx_uuid_t node_uuid;
    unsigned int len;
    rc_ty ret;

    if(!h || !user) {
	NULLARG();
	return EINVAL;
    }

    if(rndhex) {
	if(strlen(rndhex) != TOKEN_RAND_BYTES * 2 || hex2bin(rndhex, TOKEN_RAND_BYTES * 2, rndbin, sizeof(rndbin))) {
	    msg_set_reason("Invalid random string");
	    return EINVAL;
	}
    } else {
	/* non-blocking pseudo-random bytes, i.e. we don't want to block or deplete
	 * entropy as we only need a unique sequence of bytes, not a secret one as
	 * it is sent in plaintext anyway, and signed with an HMAC */
	if(sxi_rand_pseudo_bytes(rndbin, sizeof(rndbin)) == -1) {
	    /* can also return 0 or 1 but that doesn't matter here */
	    WARN("Cannot generate random bytes");
	    msg_set_reason("Failed to generate random string");
	    return FAIL_EINTERNAL;
	}
	if (bin2hex(rndbin, sizeof(rndbin), rndhexbuf, sizeof(rndhexbuf)))
            WARN("bin2hex failed");
	rndhex = rndhexbuf;
    }

    ret = sx_hashfs_self_uuid(h, &node_uuid);
    if(ret) {
        WARN("self_uuid failed");
	return ret;
    }

    snprintf(replicahex, sizeof(replicahex), "%010x", replica);
    snprintf(expirehex, sizeof(expirehex), "%016llx", (long long)expires_at);
    snprintf(h->put_token, sizeof(h->put_token), "%s:%s:%s:%s:", node_uuid.string, rndhex, replicahex+2, expirehex);
    len = strlen(h->put_token);
    if(len != TOKEN_SIGNED_LEN) {
	msg_set_reason("Generated token with bad length");
	return EINVAL;
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, &h->tokenkey, sizeof(h->tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, (unsigned char *)h->put_token, len) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &len) ||
       len != AUTH_KEY_LEN) {
	msg_set_reason("Failed to compute token hmac");
	CRIT("Cannot genearate token hmac");
	ret = FAIL_EINTERNAL;
    } else {
	bin2hex(md, AUTH_KEY_LEN, &h->put_token[TOKEN_SIGNED_LEN], AUTH_KEY_LEN * 2 + 1);
	h->put_token[sizeof(h->put_token)-1] = '\0';
	*token = h->put_token;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);

    return ret;
}


struct token_data {
    sx_uuid_t uuid;
    char token[TOKEN_RAND_BYTES*2+1];
    unsigned int replica;
    int64_t expires_at;
};

static int parse_token(sxc_client_t *sx, const uint8_t *user, const char *token, const sx_hash_t *tokenkey, struct token_data *td) {
    char uuid_str[UUID_STRING_SIZE+1], hmac[AUTH_KEY_LEN*2+1];
    char *eptr;
    uint8_t md[SXI_SHA1_BIN_LEN];
    sxi_hmac_sha1_ctx *hmac_ctx;
    unsigned int ml;

    if(!user || !token || !td) {
	NULLARG();
	return 1;
    }
    if(strlen(token) != TOKEN_TEXT_LEN) {
	msg_set_reason("Invalid token length: expected %d, got %u", TOKEN_TEXT_LEN, (unsigned)strlen(token));
	return 1;
    }
    if(token[UUID_STRING_SIZE] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN] != ':') {
	msg_set_reason("Invalid token format");
	return 1;
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, tokenkey, sizeof(*tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, (unsigned char *)token, TOKEN_SIGNED_LEN) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &ml) ||
       ml != AUTH_KEY_LEN) {
	sxi_hmac_sha1_cleanup(&hmac_ctx);
	CRIT("Cannot generate token hmac");
	return 1;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);
    bin2hex(md, AUTH_KEY_LEN, hmac, sizeof(hmac));
    if(hmac_compare((const unsigned char *)&token[TOKEN_SIGNED_LEN], (const unsigned char *)hmac, AUTH_KEY_LEN*2)) {
	msg_set_reason("Token signature does not match");
	return 1;
    }

    memcpy(uuid_str, token, UUID_STRING_SIZE);
    uuid_str[UUID_STRING_SIZE] = '\0';
    if(uuid_from_string(&td->uuid, uuid_str)) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    memcpy(td->token, &token[UUID_STRING_SIZE+1], sizeof(td->token));
    td->token[sizeof(td->token)-1] = '\0';

    td->replica = strtol(&token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1], &eptr, 16);
    if(eptr != &token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN]) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    td->expires_at = strtol(&token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1 + TOKEN_REPLICA_LEN + 1], &eptr, 16);
    if(eptr != &token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN]) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    return 0;
}

rc_ty sx_hashfs_token_get(sx_hashfs_t *h, const uint8_t *user, const char *token, unsigned int *replica_count, int64_t *expires_at) {
    struct token_data tkdt;
    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt))
	return EINVAL;
    *replica_count = tkdt.replica;
    if (expires_at)
        *expires_at = tkdt.expires_at;
    return OK;
}

static long long get_count(sxi_db_t *db, const char *table)
{
    long long ret = 0;
    char query[128];
    sqlite3_stmt *q = NULL;
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s", table);
    if(!qprep(db, &q, query) && !qstep_ret(q))
	ret = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return ret;
}

void sx_hashfs_stats(sx_hashfs_t *h)
{
    int i, j;
    INFO("User#: %lld", get_count(h->db, "users"));
    INFO("Volume#: %lld", get_count(h->db, "volumes"));
    INFO("Volume metadata#: %lld", get_count(h->db, "vmeta"));
    long long files = 0, fmeta = 0;
    for(i=0; i<METADBS; i++) {
	files += get_count(h->metadb[i], "files");
	fmeta += get_count(h->metadb[i], "fmeta");
    }
    INFO("File#: %lld", files);
    INFO("File metadata#: %lld", fmeta);
    INFO("Block counts:");
    for (j=0; j<SIZES; j++) {
	long long blocks = 0;
	for(i=0;i<HASHDBS;i++)
	    blocks += get_count(h->datadb[j][i], "blocks");
	INFO("\t%-8s (%8d byte) block#: %lld", sizelongnames[j], bsz[j], blocks);
    }
}

static void analyze_db(sxi_db_t *db)
{
    const char *name = sqlite3_db_filename(db->handle, "main");
    if (!name) name = "";
    INFO("%s:", name);
    sqlite3_stmt *q = NULL;
    if (!qprep(db, &q, "PRAGMA integrity_check;")) {
	while(qstep(q) == SQLITE_ROW)
	    INFO("\tintegrity_check: %s", sqlite3_column_text(q,0));
    }
    sqlite3_finalize(q);
}

void sx_hashfs_analyze(sx_hashfs_t *h)
{
    /* TODO: some reporting about jobs/events table */
    INFO("Analyzing databases...");
    analyze_db(h->db);
    unsigned i, j;
    for(i=0; i<METADBS; i++)
	analyze_db(h->metadb[i]);
    for (j=0; j<SIZES; j++) {
	for(i=0;i<HASHDBS;i++) {
	    analyze_db(h->datadb[j][i]);
	}
    }
}

int sx_hashfs_check(sx_hashfs_t *h, int debug) {
    unsigned int i, j;
    int64_t rows = 0; /* because arguing with gcc is pointless */
    int res = 0;

    for(i=0; i<METADBS; i++) {
	sqlite3_stmt *lock = NULL, *count = NULL, *list = NULL, *unlock = NULL;
	int fail = 1;

	if(qprep(h->metadb[i], &lock, "BEGIN EXCLUSIVE TRANSACTION") ||
	   qprep(h->metadb[i], &count, "SELECT COUNT(*) FROM files") ||
	   qprep(h->metadb[i], &list, "SELECT rowid, name, size, content FROM files ORDER BY name ASC") ||
	   qprep(h->metadb[i], &unlock, "ROLLBACK"))
	    goto hashfs_check_fileerr;
	if(qstep_noret(lock))
	    goto hashfs_check_fileerr;

	if(debug) {
	    if(qstep_ret(count))
		goto hashfs_check_fileerr;
	    rows = sqlite3_column_int64(count, 0);
	    INFO("Checking consistency of %lld files in metadata database %u / %u...", (long long int)rows, i+1, METADBS);
	}

	while(1) {
	    const char *name;
	    int64_t size, row;
	    unsigned int listlen;
	    int r = qstep(list);
	    if(r == SQLITE_DONE)
		break;
	    if(r != SQLITE_ROW)
		goto hashfs_check_fileerr;

	    row = sqlite3_column_int64(list, 0);
	    name = (const char *)sqlite3_column_text(list, 1);

	    if(!name || !strlen(name)) {
		WARN("Found invalid name on row %lld in metadata database %08x", (long long int)row, i);
		continue;
	    }

	    size = sqlite3_column_int64(list, 2);
	    sqlite3_column_blob(list, 3);
	    listlen = sqlite3_column_bytes(list, 3);
	    if(size < 0 || (listlen % SXI_SHA1_BIN_LEN) || size_to_blocks(size, NULL, NULL) != listlen / SXI_SHA1_BIN_LEN) {
		WARN("Invalid size for file %s (row %lld) in metadata database %08x", name, (long long int)row, i);
		continue;
	    }
	}

	fail = 0;

	hashfs_check_fileerr:
	if(unlock)
	    qstep(unlock);
	sqlite3_finalize(unlock);
	sqlite3_finalize(list);
	sqlite3_finalize(count);
	sqlite3_finalize(lock);

	if(fail) {
	    WARN("Verification of files in metadata database %08x aborted due to errors", i);
	    res = 1;
	}
    }

    for(j = 0; j < SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    sqlite3_stmt *lock = NULL, *count= NULL, *dups = NULL, *list = NULL, *unlock = NULL;
	    int fail = 1;

	    if(qprep(h->datadb[j][i], &lock, "BEGIN EXCLUSIVE TRANSACTION") ||
	       qprep(h->datadb[j][i], &count, "SELECT count(*) FROM blocks") ||
	       qprep(h->datadb[j][i], &list, "SELECT rowid, hash, blockno FROM blocks ORDER BY blockno ASC") ||
	       qprep(h->datadb[j][i], &dups, "SELECT b1.rowid, b1.hash, b2.rowid, b2.hash, b1.blockno FROM blocks AS b1 LEFT JOIN blocks AS b2 ON b1.rowid != b2.rowid WHERE b1.blockno = b2.blockno") ||
	       qprep(h->datadb[j][i], &unlock, "ROLLBACK"))
		goto hashfs_check_dataerr;
	    if(qstep_noret(lock))
		goto hashfs_check_dataerr;

	    if(debug) {
		if(qstep_ret(count))
		    goto hashfs_check_dataerr;
		rows = sqlite3_column_int64(count, 0);
		INFO("Checking consistency of %lld blocks in %s hash database %u / %u...", (long long int)rows, sizelongnames[j], i+1, HASHDBS);
	    }

	    while(1) {
		char h1[SXI_SHA1_BIN_LEN * 2 + 1], h2[SXI_SHA1_BIN_LEN * 2 + 1];
		const sx_hash_t *refhash;
		sx_hash_t comphash;
		int64_t off, row;
		int r = qstep(list);
		if(r == SQLITE_DONE)
		    break;
		if(r != SQLITE_ROW)
		    goto hashfs_check_dataerr;

		row = sqlite3_column_int64(list, 0);

		refhash = (const sx_hash_t *)sqlite3_column_blob(list, 1);
		if(!refhash || sqlite3_column_bytes(list, 1) != SXI_SHA1_BIN_LEN) {
		    WARN("Found invalid hash on row %lld in %s hash database %08x", (long long int)row, sizelongnames[j], i);
		    res = 1;
		    continue;
		}

		off = sqlite3_column_int(list, 2);
		if(off <= 0) {
		    bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
		    WARN("Invalid offset found for hash %s (row %lld) in %s data file %08x", h1, (long long int)row, sizelongnames[j], i);
		    res = 1;
		    continue;
		}
		off *= bsz[j];

		if(read_block(h->datafd[j][i], h->blockbuf, off, bsz[j])) {
		    bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
		    WARN("Failed to read hash %s (row %lld) from %s data file %08x at offset %lld", h1, (long long int)row, sizelongnames[j], i, (long long int)off);
		    res = 1;
		    continue;
		}

		if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), h->blockbuf, bsz[j], &comphash))
		    goto hashfs_check_dataerr;

		if(cmphash(refhash, &comphash)) {
		    bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
		    bin2hex(comphash.b, sizeof(comphash), h2, sizeof(h2));
		    WARN("Mismatch %s (row %lld) vs %s on %s data file %08x at offset %lld", h1, (long long int)row, h2, sizelongnames[j], i, (long long int)off);
		    res = 1;
		}
	    }

	    if(debug)
		INFO("Checking duplicates within %lld blocks in %s hash database %u / %u...", (long long int)rows, sizelongnames[j], i+1, HASHDBS);

	    while(1) {
		char h1[SXI_SHA1_BIN_LEN * 2 + 1], h2[SXI_SHA1_BIN_LEN * 2 + 1];
		const sx_hash_t *hash1, *hash2;
		int64_t row1, row2;
		int r = qstep(dups);
		if(r == SQLITE_DONE)
		    break;
		if(r != SQLITE_ROW)
		    goto hashfs_check_dataerr;

		row1 = sqlite3_column_int64(dups, 0);
		row2 = sqlite3_column_int64(dups, 2);
		if(row1 > row2) /* Filtering out half of the set i.e. we report (A, B) but not (B, A) */
		    continue;   /* For some reasons doing this in sql is very slow */

		hash1 = (const sx_hash_t *)sqlite3_column_blob(dups, 1);
		hash2 = (const sx_hash_t *)sqlite3_column_blob(dups, 3);
		if(sqlite3_column_bytes(dups, 1) != sizeof(*hash1))
		    strcpy(h1, "<INVALID HASH>");
		else
		    bin2hex(hash1->b, sizeof(*hash1), h1, sizeof(h1));
		if(sqlite3_column_bytes(dups, 3) != sizeof(*hash2))
		    strcpy(h2, "<INVALID HASH>");
		else
		    bin2hex(hash2->b, sizeof(*hash2), h2, sizeof(h2));

		WARN("Hash %s (row %lld) and hash %s (row %lld) in %s hash database %08x share the same block number %lld", h1, (long long int)row1, h2, (long long int)row2, sizelongnames[j], i, sqlite3_column_int64(dups, 4));
		res = 1;
	    }

	    fail = 0;

	    hashfs_check_dataerr:
	    if(unlock)
		qstep(unlock);
	    sqlite3_finalize(unlock);
	    sqlite3_finalize(list);
	    sqlite3_finalize(dups);
	    sqlite3_finalize(count);
	    sqlite3_finalize(lock);

	    if(fail) {
		WARN("Verification of hashes in %s hash database %08x aborted due to errors", sizelongnames[j], i);
		res = 1;
	    }
	}
    }
    return res;

}

const char *sx_hashfs_version(sx_hashfs_t *h) {
    return h->version;
}

const sx_uuid_t *sx_hashfs_uuid(sx_hashfs_t *h) {
    return &h->cluster_uuid;
}

int sx_hashfs_is_rebalancing(sx_hashfs_t *h) {
    return h ? h->is_rebalancing : 0;
}

int sx_hashfs_is_orphan(sx_hashfs_t *h) {
    return h ? h->is_orphan : 0;
}

/* MODHDIST: this was forked off into sx_hashfs_hdist_change_add
 * it should be simplified to only handle local activation (sxadm cluster --new) */
rc_ty sx_hashfs_modhdist(sx_hashfs_t *h, const sx_nodelist_t *list) {
    sxi_hdist_t *newmod = NULL;
    unsigned int nnodes, i, blob_size;
    sqlite3_stmt *q;
    const void *blob;
    rc_ty ret = OK;

    if(!h || !list) {
	NULLARG();
	return EINVAL;
    }

    nnodes = sx_nodelist_count(list);
    if(nnodes < 1) {
	msg_set_reason("Called with empty distribution list");
	return EINVAL;
    }

    if(h->have_hd == 0) {
	newmod = sxi_hdist_new(HDIST_SEED, 2, NULL);
    } else if(!h->is_rebalancing) {
	if((ret = sxi_hdist_get_cfg(h->hd, &blob, &blob_size)) == OK &&
	   (newmod = sxi_hdist_from_cfg(blob, blob_size)) != NULL) {
	    ret = sxi_hdist_newbuild(newmod);
	    if(ret != OK)
		sxi_hdist_free(newmod);
	}
    } else
	ret = EEXIST;

    if(ret == OK && !newmod)
	ret = ENOMEM;

    if(ret != OK) {
	msg_set_reason("Failed to prepare the distribution update");
	sxi_hdist_free(newmod);
	return ret;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(list, i);
	ret = sxi_hdist_addnode(newmod, sx_node_uuid(n), sx_node_addr(n), sx_node_internal_addr(n), sx_node_capacity(n), NULL);
	if(ret == OK)
	    continue;
	msg_set_reason("Failed to add the distribution node");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_build(newmod);
    if(ret) {
	msg_set_reason("Failed to update the distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_get_cfg(newmod, &blob, &blob_size);
    if(ret) {
	msg_set_reason("Failed to update the distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)") ||
       qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", blob, blob_size) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
    } else {
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "dist_rev") ||
	   qbind_int64(q, ":v", sxi_hdist_version(newmod)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save the updated distribution model");
	    ret = FAIL_EINTERNAL;
	}
    }
    qnullify(q);
    if(ret)
	return ret;

    if(h->have_hd)
	sxi_hdist_free(h->hd);
    h->hd = newmod;
    h->have_hd = 1;
    return OK;
}


const sx_nodelist_t *sx_hashfs_nodelist(sx_hashfs_t *h, sx_hashfs_nl_t which) {
    if(!h)
	return NULL;
    switch(which) {
    case NL_PREV:
	return h->prev_dist;
    case NL_NEXT:
	return h->next_dist;
    case NL_PREVNEXT:
	return h->prevnext_dist;
    case NL_NEXTPREV:
	return h->nextprev_dist;
    default:
	return NULL;
    }
}

static int hash_nidx_tobuf(sx_hashfs_t *h, const sx_hash_t *hash, unsigned int replica_count, unsigned int *nidx) {
    const sx_nodelist_t *nodes;
    sx_nodelist_t *belongsto;
    unsigned int i, nnodes;

    if(!h || !hash) {
	NULLARG();
	return 1;
    }

    if(!h->have_hd) {
	BADSTATE("Called before initialization");
	return 1;
    }

    nodes = sx_hashfs_nodelist(h, NL_NEXT);
    nnodes = sx_nodelist_count(nodes);

    if(replica_count < 1 || replica_count > nnodes) {
	msg_set_reason("Bad replica count: %d must be between %d and %d", replica_count, 1, nnodes);
	return 1;
    }

    /* MODHDIST: using _next set - see rant under are_blocks_available() */
    belongsto = sxi_hdist_locate(h->hd, MurmurHash64(hash, sizeof(*hash), HDIST_SEED), replica_count, 0);
    if(!belongsto) {
	WARN("Cannot get nodes for volume");
	return 1;
    }

    for(i=0; i<replica_count; i++) {
	const sx_node_t *node = sx_nodelist_get(belongsto, i);
	const sx_uuid_t *uuid = sx_node_uuid(node);
	if(!sx_nodelist_lookup_index(nodes, uuid, &nidx[i])) {
	    CRIT("node id %s from hdist is unknown to us", uuid->string);
	    sx_nodelist_delete(belongsto);
	    return 1;
	}
    }

    sx_nodelist_delete(belongsto);
    return 0;
}

int sx_hashfs_is_or_was_my_volume(sx_hashfs_t *h, const sx_hashfs_volume_t *vol) {
    sx_nodelist_t *volnodes;
    sx_hash_t hash;
    int ret = 0;

    if(!h || !vol || !h->have_hd)
	return 0;

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
	WARN("hashing volume name failed");
	return 0;
    }

    volnodes = sx_hashfs_hashnodes(h, NL_NEXTPREV, &hash, vol->replica_count);
    if(volnodes) {
	if(sx_nodelist_lookup(volnodes, &h->node_uuid))
	    ret = 1;
	sx_nodelist_delete(volnodes);
    }

    return ret;
}

static unsigned int slashes_in(const char *s) {
    unsigned int l = strlen(s), found = 0;
    const char *sl;
    while(l && (sl = memchr(s, '/', l))) {
	found++;
	sl++;
	l -= sl -s;
	s = sl;
    }
    return found;
}

static char *ith_slash(char *s, unsigned int i) {
    unsigned found = 0;
    while ((s = strchr(s, '/'))) {
        found++;
        if (found == i)
            return s;
        s++;
    }
    return NULL;
}

rc_ty sx_hashfs_revision_first(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, const sx_hashfs_file_t **file) {
    if(!volume || !file) {
	NULLARG();
	return EINVAL;
    }

    if(!sx_hashfs_is_or_was_my_volume(h, volume)) {
	msg_set_reason("Wrong node for volume '%s': ...", volume->name);
	return ENOENT;
    }

    if(check_file_name(name)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    h->rev_ndb = getmetadb(name);
    if(h->rev_ndb < 0)
	return FAIL_EINTERNAL;

    sqlite3_reset(h->qm_listrevs[h->rev_ndb]);

    if(qbind_int64(h->qm_listrevs[h->rev_ndb], ":volume", volume->id) ||
       qbind_text(h->qm_listrevs[h->rev_ndb], ":name", name))
	return FAIL_EINTERNAL;

    strncpy(h->list_file.name, name, sizeof(h->list_file.name));
    h->list_file.name[sizeof(h->list_file.name)-1] = '\0';
    h->list_file.revision[0] = '\0';
    *file = &h->list_file;

    return sx_hashfs_revision_next(h);
}


rc_ty sx_hashfs_revision_next(sx_hashfs_t *h) {
    sqlite3_stmt *q = h->qm_listrevs[h->rev_ndb];
    const char *revision;
    int r;

    sqlite3_reset(q);
    if(qbind_text(q, ":previous", h->list_file.revision))
	return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	sqlite3_reset(q);
	return h->list_file.revision[0] ? ITER_NO_MORE : ENOENT;
    }
    if(r != SQLITE_ROW) {
	sqlite3_reset(q);
	return FAIL_EINTERNAL;
    }

    h->list_file.file_size = sqlite3_column_int64(q, 0);
    revision = (const char *)sqlite3_column_text(q, 1);
    if(parse_revision(revision, &h->list_file.created_at)) {
	WARN("Found bad revision %s", revision ? revision : "(NULL)");
	sqlite3_reset(q);
	return FAIL_EINTERNAL;
    }
    strncpy(h->list_file.revision, revision, sizeof(h->list_file.revision));
    h->list_file.revision[sizeof(h->list_file.revision)-1] = '\0';
    size_to_blocks(h->list_file.file_size, NULL, &h->list_file.block_size);

    sqlite3_reset(q);

    return OK;
}

static inline int has_wildcard(const char *str)
{
    return strcspn(str, "*?[") < strlen(str);
}

rc_ty sx_hashfs_list_first(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *pattern, const sx_hashfs_file_t **file, int recurse) {
    int l = 0, r = 0, plen, glob = -1;

    if(!pattern)
	pattern = "/";

    while(pattern[0] == '/')
	pattern++;

    if (!*pattern)
	pattern = "/";

    plen = check_file_name(pattern);
    if(!h || !volume || plen < 0) {
	NULLARG();
	return EINVAL;
    }

    if(!sx_hashfs_is_or_was_my_volume(h, volume)) {
	/* TODO: got, expected: */
	msg_set_reason("Wrong node for volume '%s': ...", volume->name);
	return ENOENT;
    }

    memcpy(h->list_pattern, pattern, plen + 1);
    if(h->list_pattern[l] == '*' || h->list_pattern[l] == '?' || h->list_pattern[l] == '[')
        glob = 0;
    for(l = 0, r = 1; r<plen; r++) {
	if(h->list_pattern[l] != '/' || h->list_pattern[r] != '/') {
	    l++;
	    if(l!=r)
		h->list_pattern[l] = h->list_pattern[r];
	}
        if(glob == -1 && (h->list_pattern[l] == '*' || h->list_pattern[l] == '?' || h->list_pattern[l] == '[')) {
            glob = l;
        }
    }
    plen = l+1;
    h->list_pattern[plen] = '\0';
    for (l=0, r=0; r<plen;) {
        if (strchr("*?[]\\", h->list_pattern[r]))
            h->list_pattern_esc[l++] = '\\';
        h->list_pattern_esc[l++] = h->list_pattern[r++];
    }
    h->list_pattern_esc[l] = '\0';
    if (h->list_pattern[plen-1] == '/') {
        plen--;
        l--;
        if (plen > 0) {
            memcpy(&h->list_pattern[plen], "/*", 3);
            memcpy(&h->list_pattern_esc[l], "/*", 3);
            if(glob == -1)
                glob = plen + 1;
        } else {
            memcpy(&h->list_pattern[plen], "*", 2);
            memcpy(&h->list_pattern_esc[l], "*", 2);
            if(glob == -1)
                glob = plen;
        }
    } else {
        if(glob == -1) /* Exact file name match */
            glob = plen;
    }

    if(glob > 0) {
        strncpy(h->list_file.itername, h->list_pattern, glob);
        strncpy(h->list_file.itername_limit, h->list_pattern, glob);
    }

    h->list_pattern_slashes = slashes_in(h->list_pattern);
    h->list_file.name[0] = '\0';
    if(glob != -1) {
        h->list_file.itername[glob] = '\0';
        h->list_file.itername_limit[glob] = '\0';
        h->list_file.itername_limit_len = glob;
        /* Do not skip exact match (e.g. pattern = /foo* -> result would not contain /foo */
        if(glob > 0) {
           h->list_file.itername[glob-1]--;
           h->list_file.itername_limit[glob-1]++;
        }
    } else {
        h->list_file.itername[0] = '\0';
        h->list_file.itername_limit[0] = '\0';
        h->list_file.itername_limit_len = 0;
    }

    h->list_file.lastname[0] = '\0';
    h->list_recurse = recurse;
    h->list_volid = volume->id;
    if(file)
	*file = &h->list_file;

    memset(h->qm_list_done, 0, METADBS);

    /* For debugging */
    h->qm_list_queries = 0;

    return sx_hashfs_list_next(h);
}

rc_ty sx_hashfs_list_next(sx_hashfs_t *h) {
    int found, list_ndb, match_failed;
    int ret = OK;
    if(!h || !h->list_pattern || !*h->list_pattern)
	return EINVAL;

    do {
	found = 0;
	for(list_ndb=0; list_ndb < METADBS; list_ndb++) {
            if(h->qm_list_done[list_ndb])
                continue;

	    sqlite3_reset(h->qm_list[list_ndb]);
	    if(qbind_int64(h->qm_list[list_ndb], ":volume", h->list_volid) ||
	       qbind_text(h->qm_list[list_ndb], ":previous", h->list_file.itername)) {
                ret = FAIL_EINTERNAL;
                break;
            }

	    int r = qstep(h->qm_list[list_ndb]);

            /* For debugging, can be removed */
            h->qm_list_queries++;

	    if(r == SQLITE_DONE)
		continue;

	    if(r != SQLITE_ROW) {
                ret = FAIL_EINTERNAL;
                break;
            }

	    const char *n = (char *)sqlite3_column_text(h->qm_list[list_ndb], 0);
	    if(!n) {
		WARN("Cannot list NULL filename on meta database %u", list_ndb);
                ret = FAIL_EINTERNAL;
                break;
	    }

            if(h->list_file.itername_limit[0] != '\0' && strncmp(n, h->list_file.itername_limit, h->list_file.itername_limit_len) >= 0) {
                h->qm_list_done[list_ndb] = 1; /* This db won't be queried again */
                continue;
            }

	    if(!found || strcmp(n, h->list_file.name+1) < 0) {
		found = 1;
		h->list_file.name[0] = '/';
		strncpy(h->list_file.name+1, n, sizeof(h->list_file.name)-1);
		h->list_file.name[sizeof(h->list_file.name)-1] = '\0';

		h->list_file.file_size = sqlite3_column_int64(h->qm_list[list_ndb], 1);
		h->list_file.nblocks = size_to_blocks(h->list_file.file_size, NULL, &h->list_file.block_size);

		const char *revision = (const char *)sqlite3_column_text(h->qm_list[list_ndb], 2);
		if(!revision || parse_revision(revision, &h->list_file.created_at)) {
		    WARN("Bad revision found on file %s, volid %lld", h->list_file.name, (long long)h->list_volid);
		    ret = FAIL_EINTERNAL;
		    break;
		} else {
		    strncpy(h->list_file.revision, revision, sizeof(h->list_file.revision));
		    h->list_file.revision[sizeof(h->list_file.revision)-1] = '\0';
		}
	    }
	    sqlite3_reset(h->qm_list[list_ndb]);
	}
        if (ret)
            break;

	if(!found) {
            DEBUG("List queries performed: %llu", (unsigned long long)h->qm_list_queries);
	    ret = ITER_NO_MORE;
            break;
        }

	strncpy(h->list_file.itername, h->list_file.name+1, sizeof(h->list_file.itername));
	h->list_file.itername[sizeof(h->list_file.itername)-1] = '\0';

	char *q = ith_slash(h->list_file.name+1, h->list_pattern_slashes + 1);
	if (q)
	    *q = '\0';/* match just dir part */

        match_failed = fnmatch(h->list_pattern, h->list_file.name+1, FNM_PATHNAME | FNM_NOESCAPE);
        if(match_failed) /* If matching with globbing enabled failed, try to match with globbing escaped */
            match_failed = fnmatch(h->list_pattern_esc, h->list_file.name+1, FNM_PATHNAME);

        DEBUG("itername: %s, pattern: %s, path: %s -> %d", h->list_file.itername,
              h->list_pattern, h->list_file.name+1, match_failed);

	if (q)
	    *q = '/';/* full path again */
	if (!h->list_recurse && q) {
	    q[1] = '\0';

	    /* This is a fake dir, all unrelated items are zeroed */
	    h->list_file.file_size = 0;
	    h->list_file.block_size = 0;
	    h->list_file.nblocks = 0;
	    h->list_file.revision[0] = '\0';
	    /*
	      h->list_file.created_at = 0;

	      The created_at value here is not the fakedir mtime, but rather the mtime of
	      some random file inside it.
	      This value is not reported back to the client because it is incorrect
	      (the correct dir mtime would be the max(mtime) of all the files inside the fakedir and all its children).
	      This value is only used internally to adjust the Last-Modified header.
	    */
        }
        /* only continue if pattern matched, and it is a new file / directory */
    } while (match_failed || !strcmp(h->list_file.lastname, h->list_file.name));

    for(list_ndb=0; list_ndb < METADBS; list_ndb++) {
        sqlite3_reset(h->qm_list[list_ndb]);
    }
    if (ret)
        return ret;
    strncpy(h->list_file.lastname, h->list_file.name, sizeof(h->list_file.lastname));
    h->list_file.lastname[sizeof(h->list_file.lastname)-1] = '\0';
    return OK;
}

sx_nodelist_t *sx_hashfs_hashnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hash_t *hash, unsigned int replica_count) {
    sx_nodelist_t *prev = NULL, *next = NULL;
    unsigned int nnodes;
    int64_t mh;

    if(!h || !hash) {
	NULLARG();
	return NULL;
    }

    if(replica_count < 1) {
	msg_set_reason("Bad replica count: %d", replica_count);
	return NULL;
    }

    if(!h->have_hd) {
	BADSTATE("Called before initialization");
	return NULL;
    }

    mh = MurmurHash64(hash, sizeof(*hash), HDIST_SEED);

    if(h->is_rebalancing && (which == NL_PREV || which == NL_PREVNEXT || which == NL_NEXTPREV)) {
	nnodes = sx_nodelist_count(h->prev_dist);
	if(replica_count <= nnodes) {
	    prev = sxi_hdist_locate(h->hd, mh, replica_count, 1);
	    if(!prev) {
		msg_set_reason("Failed to locate hash");
		return NULL;
	    }
	} else if(which == NL_PREV)
	    msg_set_reason("Bad replica count: %d should be below %d", replica_count, nnodes);

	/* MODHDIST: over replica request is only fatal if we don't have a NEXT part */
	if(which == NL_PREV)
	    return prev;
    }

    nnodes = sx_nodelist_count(h->next_dist);
    if(replica_count > nnodes) {
	/* MODHDIST: over replica request is always fatal (replica can't have decreased) */
	msg_set_reason("Bad replica count: %d should be below %d", replica_count, nnodes);
	sx_nodelist_delete(prev);
	return NULL;
    }
    next = sxi_hdist_locate(h->hd, mh, replica_count, 0);
    if(!next) {
	msg_set_reason("Failed to locate hash");
	return NULL;
    }

    if(prev) {
	sx_nodelist_t *ret, *del;
	rc_ty r;
	if(which == NL_NEXTPREV) {
	    ret = next;
	    del = prev;
	} else {
	    ret = prev;
	    del = next;
	}
	r = sx_nodelist_addlist(ret, del);
	sx_nodelist_delete(del);
	if(r) {
	    sx_nodelist_delete(ret);
	    ret = NULL;
	}
	return ret;
    }

    return next;
}

sx_nodelist_t *sx_hashfs_putfile_hashnodes(sx_hashfs_t *h, const sx_hash_t *hash) {
    return sx_hashfs_hashnodes(h, NL_NEXT, hash, h->put_replica);
}

rc_ty sx_hashfs_volnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hashfs_volume_t *volume, int64_t size, sx_nodelist_t **nodes, unsigned int *block_size) {
    sx_hash_t hash;

    if(!h || !volume || !nodes) {
	NULLARG();
	return EFAULT;
    }
    if (size < SXLIMIT_MIN_FILE_SIZE || size > SXLIMIT_MAX_FILE_SIZE) {
	msg_set_reason("Invalid size %lld: must be between %lld and %lld",
		       (long long)size, (long long)SXLIMIT_MIN_FILE_SIZE,
		       (long long)SXLIMIT_MAX_FILE_SIZE);
	return EINVAL;
    }

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), volume->name, strlen(volume->name), &hash))
	return FAIL_EINTERNAL;

    if(!(*nodes = sx_hashfs_hashnodes(h, which, &hash, volume->replica_count)))
	return FAIL_EINTERNAL;

    size_to_blocks(size, NULL, block_size);
    return OK;
}


/* MODHDIST: there might now be 0, 1 or 2 selves. which do we return? */
const sx_node_t *sx_hashfs_self(sx_hashfs_t *h) {
    if(!h || sx_storage_is_bare(h))
	return NULL;
    return sx_nodelist_lookup(h->nextprev_dist, &h->node_uuid);
}

rc_ty sx_hashfs_self_uuid(sx_hashfs_t *h, sx_uuid_t *uuid) {
    if(!h || !uuid) {
	NULLARG();
	return EFAULT;
    }
    if(sx_storage_is_bare(h))
	return FAIL_EINIT;

    memcpy(uuid, &h->node_uuid, sizeof(*uuid));
    return OK;
}

const char *sx_hashfs_self_unique(sx_hashfs_t *h) {
    char *r = (char *)h->blockbuf;
    uint64_t a, b;

    if(sx_storage_is_bare(h))
	a = 0;
    else
	a = MurmurHash64(h->node_uuid.binary, sizeof(h->node_uuid.binary), HDIST_SEED);

    b = MurmurHash64(h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary), HDIST_SEED);
    sprintf(r, "%016llx%016llx", (long long)a, (long long)b);

    return r;
}

const char *sx_hashfs_authtoken(sx_hashfs_t *h) {
    return (h && strlen(h->root_auth) == AUTHTOK_ASCII_LEN) ? h->root_auth : NULL;
}

char *sxi_hashfs_admintoken(sx_hashfs_t *h) {
    uint8_t key[AUTH_KEY_LEN];
    char auth[AUTHTOK_ASCII_LEN + 1];

    if(!h) {
	NULLARG();
	return NULL;
    }

    if(sx_hashfs_get_user_info(h, ADMIN_USER, NULL, key, NULL))
	return NULL;

    if(encode_auth_bin(ADMIN_USER, (const unsigned char *) key, AUTH_KEY_LEN, auth, sizeof(auth))) {
	CRIT("Failed to encode cluster key");
	fprintf(stderr, "encode_auth_bin failed\n");
	return NULL;
    }

    return strdup(auth);
}

rc_ty sx_hashfs_derive_key(sx_hashfs_t *h, unsigned char *key, int len, const char *info)
{
    if (derive_key(h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary),
		   (const unsigned char*)h->root_auth, strlen(h->root_auth), info,
		   key, len))
	return FAIL_EINTERNAL;
    return OK;
}

rc_ty sx_hashfs_create_user(sx_hashfs_t *h, const char *user, const uint8_t *uid, unsigned uid_size, const uint8_t *key, unsigned key_size, int role)
{
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !user || !key) {
	NULLARG();
	return EFAULT;
    }

    if(sx_hashfs_check_username(user)) {
	msg_set_reason("Invalid user");
	return EINVAL;
    }

    if(key_size != AUTH_KEY_LEN) {
	msg_set_reason("Invalid key");
	return EINVAL;
    }

    if(uid && uid_size != AUTH_UID_LEN) {
	msg_set_reason("Invalid uid");
	return EINVAL;
    }

    if(role != ROLE_ADMIN && role != ROLE_USER) {
	msg_set_reason("Invalid role");
	return EINVAL;
    }

    sqlite3_stmt *q = h->q_createuser;
    sqlite3_reset(q);
    do {
	sx_hash_t uh;
	if(!uid) {
	    if (hash_buf(NULL, 0, user, strlen(user), &uh))
		break;
	    uid = uh.b;
	}
	if(qbind_blob(q, ":userhash", uid, AUTH_UID_LEN))
	    break;
	if (qbind_text(q, ":name", user))
	    break;
	if (qbind_blob(q, ":key", key, key_size))
	    break;
	if (qbind_int64(q, ":role", role))
	    break;
	int ret = qstep(q);
	if (ret == SQLITE_CONSTRAINT) {
	    rc = EEXIST;
	    break;
	}
	if (ret != SQLITE_DONE)
	    break;
        INFO("User '%s' created", user);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}

int encode_auth(const char *user, const unsigned char *key, unsigned key_size, char *auth, unsigned auth_size)
{
    if (!user || !key || !auth) {
	NULLARG();
	return -1;
    }
    if (key_size != AUTH_KEY_LEN) {
	msg_set_reason("Key of wrong size: %d != %d", key_size, AUTH_KEY_LEN);
	return -1;
    }
    if (auth_size < AUTHTOK_ASCII_LEN + 1) {
	msg_set_reason("Auth of wrong size: %d != %d",
		       auth_size, AUTHTOK_ASCII_LEN+1);
	return -1;
    }
    sx_hash_t h;
    if (hash_buf(NULL, 0, user, strlen(user), &h)) {
	WARN("hashing username failed");
	return -1;
    }
    if (!h.b || !key || !auth) {
	WARN("impossible NULL args");
	return -1;
    }
    return encode_auth_bin(h.b, key, key_size, auth, auth_size);
}

int encode_auth_bin(const uint8_t *userhash, const unsigned char *key, unsigned key_size, char *auth, unsigned auth_size)
{
    uint8_t buf[AUTHTOK_BIN_LEN];
    if (!userhash) {
	WARN("NULL userhash");
	return -1;
    }
    if (!key) {
	WARN("NULL key");
	return -1;
    }
    if (!auth) {
	WARN("NULL auth");
	return -1;
    }

    if (key_size != AUTH_KEY_LEN) {
	msg_set_reason("bad key size: %d != %d",
		       key_size, AUTH_KEY_LEN);
	return -1;
    }
    if (auth_size < AUTHTOK_ASCII_LEN + 1) {
	msg_set_reason("bad auth token size: %d != %d",
		       auth_size, AUTHTOK_ASCII_LEN+1);
	return -1;
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, userhash, AUTH_UID_LEN);
    memcpy(buf + AUTH_UID_LEN, key, AUTH_KEY_LEN);
    char *a = sxi_b64_enc_core(buf, sizeof(buf));
    strncpy(auth, a, auth_size);
    auth[auth_size - 1] = 0;
    free(a);
    return 0;
}

rc_ty sx_hashfs_list_users(sx_hashfs_t *h, user_list_cb_t cb, void *ctx) {
    rc_ty rc = FAIL_EINTERNAL;
    int ret;
    uint64_t lastuid = 0;

    if (!h || !cb) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_stmt *q = h->q_listusers;
    while(1) {
        sqlite3_reset(q);
        if(qbind_int64(q, ":lastuid", lastuid))
            break;
        ret = qstep(q);
	if(ret == SQLITE_DONE)
	    rc = OK;
	if(ret != SQLITE_ROW)
            break;
	sx_uid_t uid = sqlite3_column_int64(q, 0);
	const char *name = (const char *)sqlite3_column_text(q, 1);
	const uint8_t *user = sqlite3_column_blob(q, 2);
	const uint8_t *key = sqlite3_column_blob(q, 3);
	int is_admin = sqlite3_column_int64(q, 4) == ROLE_ADMIN;
        lastuid = uid;

	if(sqlite3_column_bytes(q, 2) != SXI_SHA1_BIN_LEN || sqlite3_column_bytes(q, 3) != AUTH_KEY_LEN) {
	    WARN("User %s (%lld) is invalid", name, (long long)uid);
	    continue;
	}

	if(cb(uid, name, user, key, is_admin, ctx)) {
	    rc = EINTR;
	    break;
	}
    }
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_list_acl(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, sx_uid_t uid, int uid_priv, acl_list_cb_t cb, void *ctx)
{
    char user[SXLIMIT_MAX_USERNAME_LEN+1];
    int64_t lastuid = 0;
    sx_priv_t priv = PRIV_NONE;
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !cb || !ctx)
	return EINVAL;

    /* list privileges for self */
    priv = uid_priv;
    if (uid > 0 ) {
        if ((rc = sx_hashfs_uid_get_name(h, uid, user, sizeof(user))))
            return rc;

        rc = FAIL_EINTERNAL;
        if (cb(user, priv, uid == vol->owner, ctx))
            return rc;
    }

    if (!(priv & (PRIV_ADMIN | PRIV_ACL))) {
        DEBUG("Not an owner/admin: printed only self privileges");
        return OK;
    }
    /* admin and owner can see full ACL list */

    sqlite3_stmt *q = h->q_listacl;
    do {
        int ret = SQLITE_ROW;
	if (qbind_int64(q, ":volid", vol->id))
            break;
        while (1) {
            sqlite3_reset(q);
            if (qbind_int64(q, ":lastuid", lastuid))
                break;
            ret = qstep(q);
            if (ret != SQLITE_ROW)
               break;
            int64_t list_uid = sqlite3_column_int64(q, 2);
            lastuid = list_uid;
            if (list_uid == uid)
                continue;/* we've already printed permissions for self */
	    int perm = sqlite3_column_int64(q, 1);
	    if (cb((const char*)sqlite3_column_text(q, 0), perm, list_uid == vol->owner, ctx))
		break;
	}
	if (ret != SQLITE_DONE)
	    break;
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}


static rc_ty get_uid_role(sx_hashfs_t *h, const char *username, int64_t *uid, int *role, int inactivetoo) {
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !username || sx_hashfs_check_username(username))
	return EINVAL;
    sqlite3_stmt *q = h->q_getuid;
    sqlite3_reset(q);
    do {
	if(qbind_text(q, ":name", username) ||
	   qbind_int(q, ":inactivetoo", (inactivetoo != 0)))
	    break;

	int ret = qstep(q);
	if (ret == SQLITE_DONE) {
	    rc = ENOENT;
	    break;
	}
	if (ret != SQLITE_ROW)
	    break;
	if(uid)
	    *uid = sqlite3_column_int64(q, 0);
        if(role)
            *role = sqlite3_column_int(q, 1);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_get_uid_role(sx_hashfs_t *h, const char *user, int64_t *uid, int *role) {
    return get_uid_role(h, user, uid, role, 0);
}

rc_ty sx_hashfs_get_uid(sx_hashfs_t *h, const char *user, int64_t *uid)
{
    return sx_hashfs_get_uid_role(h, user, uid, NULL);
}

rc_ty sx_hashfs_uid_get_name(sx_hashfs_t *h, uint64_t uid, char *name, unsigned len)
{
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !name || !len)
	return EINVAL;
    sqlite3_stmt *q = h->q_getuidname;
    sqlite3_reset(q);
    do {
	if (qbind_int64(q, ":uid", uid))
	    break;
	int ret = qstep(q);
	if (ret == SQLITE_DONE) {
	    rc = ENOENT;
	    break;
	}
	if (ret != SQLITE_ROW)
	    break;
	strncpy(name, (const char*)sqlite3_column_text(q, 0), len);
	name[len-1] = 0;
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}


rc_ty sx_hashfs_delete_user(sx_hashfs_t *h, const char *username, const char *new_owner) {
    rc_ty rc, ret = FAIL_EINTERNAL;
    sx_uid_t old, new;

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    rc = get_uid_role(h, username, &old, NULL, 1);
    if(rc != OK) {
	ret = rc;
	goto delete_user_err;
    }

    rc = sx_hashfs_get_uid(h, new_owner, &new);
    if(rc != OK) {
	/* FIXME: shall i rather fall back to any admin user? */
	ret = rc;
	goto delete_user_err;
    }

    if(qbind_int64(h->q_chownvol, ":new", new) || 
       qbind_int64(h->q_chownvol, ":old", old) ||
       qstep_noret(h->q_chownvol))
	goto delete_user_err;

    if(qbind_int64(h->q_deleteuser, ":uid", old) ||
       qstep_noret(h->q_deleteuser))
	goto delete_user_err;

    if(qcommit(h->db))
	goto delete_user_err;

    ret = OK;
    INFO("User %s removed (and replaced by %s)", username, new_owner);

 delete_user_err:
    sqlite3_reset(h->q_getuser);

    if(ret != OK)
	qrollback(h->db);

    return ret;
}


void sx_hashfs_volume_new_begin(sx_hashfs_t *h) {
    h->nmeta = 0;
}

rc_ty sx_hashfs_volume_new_addmeta(sx_hashfs_t *h, const char *key, const void *value, unsigned int value_len) {
    if(!h)
	return FAIL_EINTERNAL;

    rc_ty rc;
    if((rc = sx_hashfs_check_meta(key, value, value_len)))
	return rc;

    if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	return EOVERFLOW;

    memcpy(h->meta[h->nmeta].key, key, strlen(key)+1);
    memcpy(h->meta[h->nmeta].value, value, value_len);
    h->meta[h->nmeta].value_len = value_len;
    h->nmeta++;
    return OK;
}

static rc_ty get_min_reqs(sx_hashfs_t *h, unsigned int *min_nodes, int64_t *min_capa) {
    sqlite3_reset(h->q_minreqs);
    if(qstep_ret(h->q_minreqs))
        return FAIL_EINTERNAL;

    if(min_nodes)
        *min_nodes = sqlite3_column_int(h->q_minreqs, 0);
    if(min_capa)
        *min_capa = sqlite3_column_int64(h->q_minreqs, 1);

    sqlite3_reset(h->q_minreqs);
    return OK;
}

static int64_t get_cluster_capacity(sx_hashfs_t *h, sx_hashfs_nl_t which) {
    int64_t size = 0;
    unsigned int i, nnodes;

    nnodes = sx_nodelist_count(sx_hashfs_nodelist(h, which));
    for(i = 0; i < nnodes; i++)
        size += sx_node_capacity(sx_nodelist_get(sx_hashfs_nodelist(h, which), i));

    return size;
}

/* Return error if given size is incorrect */
rc_ty sx_hashfs_check_volume_size(sx_hashfs_t *h, int64_t size, unsigned int replica) {
    int64_t vols_size;
    int64_t nodes_size = 0;

    if(size < SXLIMIT_MIN_VOLUME_SIZE || size > SXLIMIT_MAX_VOLUME_SIZE) {
        msg_set_reason("Invalid volume size %lld: must be between %lld and %lld",
                       (long long)size,
                       (long long)SXLIMIT_MIN_VOLUME_SIZE,
                       (long long)SXLIMIT_MAX_VOLUME_SIZE);
        return EINVAL;
    }

    if(!h->have_hd) /* No hdist, so skip checking if sum of volumes fits in cluster capacity */
        return OK;

    if(get_min_reqs(h, NULL, &vols_size)) {
        msg_set_reason("Failed to get volume sizes");
        return FAIL_EINTERNAL;
    }

    if(sx_hashfs_is_rebalancing(h)) /* NL_PREV will differ from NL_NEXT */
        nodes_size = MIN(get_cluster_capacity(h, NL_PREV), get_cluster_capacity(h, NL_NEXT));
    else
        nodes_size = get_cluster_capacity(h, NL_PREV); /* All dists are equal, doesn't matter which one will we take here */

    if(!nodes_size) {
        msg_set_reason("Failed to get node sizes");
        return FAIL_EINTERNAL;
    }

    /*** temporary work-around for bb#813 ***/
    if(size * (int64_t)replica > nodes_size) {
        msg_set_reason("Invalid volume size %lld (replica %u): must be between %lld and %lld",
                       (long long)size, replica,
                       (long long)SXLIMIT_MIN_VOLUME_SIZE,
                       (long long)nodes_size);
        return EINVAL;
    }
    return OK;
    /*** end ***/

    /* Check if cluster capacity is not reached yet (better error message) */
    if(SXLIMIT_MIN_VOLUME_SIZE > nodes_size - vols_size) {
        msg_set_reason("Invalid volume size %lld: reached cluster capacity", (long long)size);
        return EINVAL;
    }

    /* Total volumes size is greater than cluster capacity */
    if(vols_size + size * (int64_t)replica > nodes_size) {
        msg_set_reason("Invalid volume size %lld (replica %u): must be between %lld and %lld",
                       (long long)size, replica,
                       (long long)SXLIMIT_MIN_VOLUME_SIZE,
                       (long long)(nodes_size - vols_size));
        return EINVAL;
    }

    return OK;
}

rc_ty sx_hashfs_volume_new_finish(sx_hashfs_t *h, const char *volume, int64_t size, unsigned int replica, unsigned int revisions, sx_uid_t uid) {
    unsigned int reqlen = 0;
    rc_ty ret = FAIL_EINTERNAL;
    int64_t volid;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }
    if ((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    if(h->have_hd) {
	unsigned int nnodes = MIN(sx_nodelist_count(sx_hashfs_nodelist(h, NL_PREV)), sx_nodelist_count(sx_hashfs_nodelist(h, NL_NEXT)));
	if(replica < 1 || replica > nnodes) {
	    msg_set_reason("Invalid replica count %d: must be between %d and %d",
			   replica, 1, nnodes);
	    return EINVAL;
	}
    }

    if(revisions < SXLIMIT_MIN_REVISIONS || revisions > SXLIMIT_MAX_REVISIONS) {
	msg_set_reason("Invalid volume revisions: must be between %u and %u", SXLIMIT_MIN_REVISIONS, SXLIMIT_MAX_REVISIONS);
	return EINVAL;
    }

    sqlite3_reset(h->q_addvol);
    sqlite3_reset(h->q_addvolmeta);
    sqlite3_reset(h->q_addvolprivs);

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    /* Check volume size inside transaction to not fall into race */
    if((ret = sx_hashfs_check_volume_size(h, size, replica)) != OK)
        goto volume_new_err;
    ret = FAIL_EINTERNAL;

    if(qbind_text(h->q_addvol, ":volume", volume) ||
       qbind_int(h->q_addvol, ":replica", replica) ||
       qbind_int(h->q_addvol, ":revs", revisions) ||
       qbind_int64(h->q_addvol, ":size", size) ||
       qbind_int64(h->q_addvol, ":owner", uid))
	goto volume_new_err;

    r = qstep(h->q_addvol);
    if(r == SQLITE_CONSTRAINT) {
	const sx_hashfs_volume_t *vol;
	if(sx_hashfs_volume_by_name(h, volume, &vol) == OK)
	    ret = FAIL_VOLUME_EEXIST;
	else
	    ret = FAIL_LOCKED;
    }

    if(r != SQLITE_DONE)
	goto volume_new_err;

    volid = sqlite3_last_insert_rowid(sqlite3_db_handle(h->q_addvol));

    if(h->nmeta) {
	unsigned int nmeta = h->nmeta;
	if(qbind_int64(h->q_addvolmeta, ":volume", volid))
	    goto volume_new_err;

	while(nmeta--) {
	    reqlen += strlen(h->meta[nmeta].key) + 3 + h->meta[nmeta].value_len * 2 + 3; /* "key":"hex(value)", */
	    sqlite3_reset(h->q_addvolmeta);
	    if(qbind_text(h->q_addvolmeta, ":key", h->meta[nmeta].key) ||
	       qbind_blob(h->q_addvolmeta, ":value", h->meta[nmeta].value, h->meta[nmeta].value_len) ||
	       qstep_noret(h->q_addvolmeta))
		goto volume_new_err;
	}
    }

    if(qbind_int64(h->q_addvolprivs, ":volume", volid) ||
       qbind_int64(h->q_addvolprivs, ":user", uid) ||
       qbind_int(h->q_addvolprivs, ":priv", PRIV_READ | PRIV_WRITE) ||
       qstep_noret(h->q_addvolprivs))
	goto volume_new_err;

    if(qcommit(h->db))
	goto volume_new_err;

    ret = OK;

    volume_new_err:
    sqlite3_reset(h->q_addvol);
    sqlite3_reset(h->q_addvolmeta);
    sqlite3_reset(h->q_addvolprivs);

    if(ret != OK)
	qrollback(h->db);

    h->nmeta = 0;

    return ret;
}

rc_ty sx_hashfs_volume_enable(sx_hashfs_t *h, const char *volume) {
    int ret = OK;

    if(qbind_text(h->q_onoffvol, ":volume", volume) ||
       qbind_int(h->q_onoffvol, ":enable", 1) ||
       qstep_noret(h->q_onoffvol))
	ret = FAIL_EINTERNAL;

    return ret;
}

rc_ty sx_hashfs_volume_disable(sx_hashfs_t *h, const char *volume) {
    const sx_hashfs_volume_t *vol;
    unsigned int mdb = 0;
    rc_ty ret;

    if(!h) {
	NULLARG();
	return EFAULT;
    }
    if((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    ret = sx_hashfs_volume_by_name(h, volume, &vol);
    if(ret != OK)
	return ret;

    sqlite3_reset(h->q_onoffvol);

    /* If not a volnode, then disable right away */
    if(!sx_hashfs_is_or_was_my_volume(h, vol)) {
	if(qbind_text(h->q_onoffvol, ":volume", volume) ||
	   qbind_int(h->q_onoffvol, ":enable", 0) ||
	   qstep_noret(h->q_onoffvol))
	    return FAIL_EINTERNAL;
	return OK;
    }

    /* Otherwise make sure the volume is empty */
    if(qbegin(h->db)) {
	ret = FAIL_EINTERNAL;
	goto volume_disable_err;
    }
    for(mdb=0; mdb<METADBS; mdb++) {
	if(qbegin(h->metadb[mdb])) {
	    ret = FAIL_EINTERNAL;
	    goto volume_disable_err;
	}
    }

    ret = sx_hashfs_list_first(h, vol, NULL, NULL, 1);
    if(ret == OK) {
	msg_set_reason("Cannot disable non empty volume");
	ret = ENOTEMPTY;
    }
    if(ret != ITER_NO_MORE)
	goto volume_disable_err;
    ret = OK;

    if(qbind_text(h->q_onoffvol, ":volume", volume) ||
       qbind_int(h->q_onoffvol, ":enable", 0) ||
       qstep_noret(h->q_onoffvol)) {
	ret = FAIL_EINTERNAL;
	goto volume_disable_err;
    }

    if(qcommit(h->db))
	ret = FAIL_EINTERNAL;

 volume_disable_err:
    if(ret != OK)
	qrollback(h->db);

    while(mdb--)
	qrollback(h->metadb[mdb]);

    sqlite3_reset(h->q_onoffvol);

    return ret;
}

rc_ty sx_hashfs_volume_delete(sx_hashfs_t *h, const char *volume, int force) {
    rc_ty ret;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }
    if((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    sqlite3_reset(h->q_getvolstate);
    sqlite3_reset(h->q_delvol);

    if(qbegin(h->db) ||
       qbind_text(h->q_getvolstate, ":volume", volume)) {
	ret = FAIL_EINTERNAL;
	goto volume_delete_err;
    }

    r = qstep(h->q_getvolstate);
    if(r == SQLITE_DONE) {
	ret = ENOENT;
	goto volume_delete_err;
    }
    if(r != SQLITE_ROW) {
	ret = FAIL_EINTERNAL;
	goto volume_delete_err;
    }
    if(!force) {
	r = sqlite3_column_int(h->q_getvolstate, 0);
	if(r) {
	    ret = EPERM;
	    msg_set_reason("Cannot delete an enabled volume");
	    goto volume_delete_err;
	}
    }
    if(qbind_text(h->q_delvol, ":volume", volume) ||
       qstep_noret(h->q_delvol) ||
       qcommit(h->db))
	ret = FAIL_EINTERNAL;
    else
	ret = OK;

 volume_delete_err:
    if(ret != OK)
	qrollback(h->db);

    sqlite3_reset(h->q_getvolstate);
    sqlite3_reset(h->q_delvol);

    return ret;
}

rc_ty sx_hashfs_user_onoff(sx_hashfs_t *h, const char *user, int enable) {
    int ret = OK;

    if(qbind_text(h->q_onoffuser, ":username", user) ||
       qbind_int(h->q_onoffuser, ":enable", enable) ||
       qstep_noret(h->q_onoffuser))
	ret = FAIL_EINTERNAL;
    if (ret == OK)
        INFO("User '%s' %s", user, enable ? "enabled" : "disabled");

    return ret;
}

static rc_ty get_remote_volume_size(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, int64_t *size) {
    rc_ty ret = FAIL_EINTERNAL;
    sx_nodelist_t *volnodes;
    sx_hash_t hash;
    sxc_cluster_lf_t *list = NULL;
    sxi_hostlist_t hosts;
    unsigned int i;

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
        WARN("hashing volume name failed");
        return ret;
    }

    sxi_hostlist_init(&hosts);
    volnodes = sx_hashfs_hashnodes(h, NL_NEXTPREV, &hash, vol->replica_count);
    if(!volnodes) {
        WARN("Failed to get volnodes for volume %s", vol->name);
        goto get_remote_volume_size_err;
    }

    for(i = 0; i < sx_nodelist_count(volnodes); i++) {
        if(sxi_hostlist_add_host(h->sx, &hosts, sx_node_internal_addr(sx_nodelist_get(volnodes, i)))) {
            WARN("Failed to add host to hostlist");
            goto get_remote_volume_size_err;
        }
    }

    list = sxi_conns_listfiles(h->sx_clust, vol->name, &hosts, NULL, 0, size, NULL, NULL, NULL, 0, 1);
    if(!list) {
        WARN("Failed to list volume %s in order to get its size: %s", vol->name, sxc_geterrmsg(h->sx));
        goto get_remote_volume_size_err;
    }

    ret = OK;
get_remote_volume_size_err:
    sxi_hostlist_empty(&hosts);
    sx_nodelist_delete(volnodes);
    sxc_cluster_listfiles_free(list);
    return ret;
}

rc_ty sx_hashfs_volume_first(sx_hashfs_t *h, const sx_hashfs_volume_t **volume, int64_t uid) {
    if(!h || !volume) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    h->curvol.name[0] = '\0';
    h->curvoluid = uid;
    *volume = &h->curvol;
    return sx_hashfs_volume_next(h);
}

rc_ty sx_hashfs_volume_next(sx_hashfs_t *h) {
    const char *name;
    rc_ty res = FAIL_EINTERNAL;
    int r;

    if(!h) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    sqlite3_reset(h->q_nextvol);
    if(qbind_text(h->q_nextvol, ":previous", h->curvol.name))
	goto volume_list_err;
    if(qbind_int64(h->q_nextvol, ":user", h->curvoluid))
	goto volume_list_err;

    r = qstep(h->q_nextvol);
    if(r == SQLITE_DONE)
	res = ITER_NO_MORE;
    if(r != SQLITE_ROW)
	goto volume_list_err;

    name = (const char *)sqlite3_column_text(h->q_nextvol, 1);
    if(!name)
	goto volume_list_err;

    strncpy(h->curvol.name, name, sizeof(h->curvol.name) - 1);
    h->curvol.name[sizeof(h->curvol.name) - 1] = '\0';
    h->curvol.id = sqlite3_column_int64(h->q_nextvol, 0);
    h->curvol.replica_count = sqlite3_column_int(h->q_nextvol, 2);
    h->curvol.size = sqlite3_column_int64(h->q_nextvol, 4);
    h->curvol.owner = sqlite3_column_int64(h->q_nextvol, 5);
    h->curvol.revisions = sqlite3_column_int(h->q_nextvol, 6);

    if(sx_hashfs_is_or_was_my_volume(h, &h->curvol)) {
        /* This host is a volhost, no need to query different hosts */
        h->curvol.cursize = sqlite3_column_int64(h->q_nextvol, 3);
    } else {
        /* Get current volume usage from volhost of current volume */
        res = get_remote_volume_size(h, &h->curvol, &h->curvol.cursize);
        if(res != OK)
            goto volume_list_err;
    }

    res = OK;
    volume_list_err:
    sqlite3_reset(h->q_nextvol);
    return res;
}


static rc_ty volume_get_common(sx_hashfs_t *h, const char *name, int64_t volid, const sx_hashfs_volume_t **volume) {
    sqlite3_stmt *q;
    rc_ty res = FAIL_EINTERNAL;
    int r;

    if(!h || !volume) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    if(name) {
	q = h->q_volbyname;
	sqlite3_reset(q);
	if(qbind_text(q, ":name", name))
	    goto volume_err;
    } else {
	q = h->q_volbyid;
	sqlite3_reset(q);
	if(qbind_int64(q, ":volid", volid))
	    goto volume_err;
    }

    r = qstep(q);
    if(r == SQLITE_DONE)
	res = ENOENT;
    if(r != SQLITE_ROW)
	goto volume_err;

    name = (const char *)sqlite3_column_text(q, 1);
    if(!name)
	goto volume_err;

    strncpy(h->curvol.name, name, sizeof(h->curvol.name) - 1);
    h->curvol.name[sizeof(h->curvol.name) - 1] = '\0';
    h->curvol.id = sqlite3_column_int64(q, 0);
    h->curvol.replica_count = sqlite3_column_int(q, 2);
    h->curvol.cursize = sqlite3_column_int64(q, 3);
    h->curvol.size = sqlite3_column_int64(q, 4);
    h->curvol.owner = sqlite3_column_int64(q, 5);
    h->curvol.revisions = sqlite3_column_int(q, 6);
    *volume = &h->curvol;
    res = OK;

    volume_err:
    sqlite3_reset(q);
    return res;
}

rc_ty sx_hashfs_grant(sx_hashfs_t *h, uint64_t uid, const char *volume, int priv)
{
    if (!h || !volume)
	return EINVAL;

    rc_ty rc = FAIL_EINTERNAL;
    sqlite3_stmt *q = h->q_grant;
    sqlite3_reset(q);
    const sx_hashfs_volume_t *vol = NULL;
    do {
	rc = volume_get_common(h, volume, -1, &vol);
	if (rc) {
	    WARN("Cannot retrieve volume id for '%s': %s", volume, rc2str(rc));
	    break;
	}
	if (qbind_int64(q,":uid", uid))
	    break;
	if (qbind_int64(q,":volid", vol->id))
	    break;
	if (qbind_int64(q,":priv", priv))
	    break;
	if (qstep_noret(q))
	    break;
	INFO("%s: granted priv %d to %ld on %s", h->dir, priv, (long)uid, volume);
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_revoke(sx_hashfs_t *h, uint64_t uid, const char *volume, int privmask)
{
    if (!h || !volume)
	return EINVAL;
    rc_ty rc = FAIL_EINTERNAL;
    sqlite3_stmt *q = h->q_revoke;
    sqlite3_reset(q);
    const sx_hashfs_volume_t *vol = NULL;
    do {
	rc = volume_get_common(h, volume, -1, &vol);
	if (rc) {
	    WARN("Cannot retrieve volume id for '%s': %s", volume, rc2str(rc));
	    break;
	}
	if (qbind_int64(q,":uid", uid))
	    break;
	if (qbind_int64(q,":volid", vol->id))
	    break;
	if (qbind_int64(q,":privmask", privmask))
	    break;
	if (qstep_noret(q))
	    break;
	INFO("%s: revoked privmask %d to %ld on %s", h->dir, privmask, (long)uid, volume);
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_volume_by_name(sx_hashfs_t *h, const char *name, const sx_hashfs_volume_t **volume) {
    if(sx_hashfs_check_volume_name(name)) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    return volume_get_common(h, name, 0, volume);
}

rc_ty sx_hashfs_volume_by_id(sx_hashfs_t *h, int64_t volid, const sx_hashfs_volume_t **volume) {
    return volume_get_common(h, NULL, volid, volume);
}

static void sx_hashfs_getfile_reset(sx_hashfs_t *h)
{
    if(h->get_ndb < METADBS) {
	sqlite3_reset(h->qm_get[h->get_ndb]);
	sqlite3_reset(h->qm_getrev[h->get_ndb]);
    }
}

rc_ty sx_hashfs_getfile_begin(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, sx_hashfs_file_t *filedata, sx_hash_t *etag) {
    const sx_hashfs_volume_t *vol;
    unsigned int content_len, created_at, bsize;
    const char *rev;
    sqlite3_stmt *q;
    int64_t size;
    rc_ty res;
    int r;

    /* reset previous getfile queries */
    sx_hashfs_getfile_end(h);
    res = sx_hashfs_volume_by_name(h, volume, &vol);
    if(res)
	return res;

    if(check_file_name(filename)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    h->get_ndb = getmetadb(filename);
    if(h->get_ndb < 0)
	return FAIL_EINTERNAL;
    /* reset current getfile queries */
    sx_hashfs_getfile_reset(h);

    if(revision) {
	if(check_revision(revision)) {
	    msg_set_reason("Invalid file name");
	    return EINVAL;
	}
	q = h->qm_getrev[h->get_ndb];
	if(qbind_text(q, ":revision", revision))
	    return FAIL_EINTERNAL;
    } else
	q = h->qm_get[h->get_ndb];

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename))
	return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	DEBUG("No such file: %s/%s", volume, filename);
	sx_hashfs_getfile_end(h);
	return ENOENT;
    }
    if(r != SQLITE_ROW) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    h->get_id = sqlite3_column_int64(q, 0);
    size = sqlite3_column_int64(q, 1);
    h->get_nblocks = size_to_blocks(size, NULL, &bsize);
    h->get_content = sqlite3_column_blob(q, 2);
    content_len = sqlite3_column_bytes(q, 2);

    rev = (const char *)sqlite3_column_text(q, 3);
    if(!rev ||
       parse_revision(rev, &created_at) ||
       (etag && hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), rev, strlen(rev), etag))) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }
    if(content_len != sizeof(sx_hash_t) * h->get_nblocks) {
	WARN("Inconsistent entry for %s:%s", volume, filename);
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    h->get_replica = vol->replica_count;

    if(filedata) {
	filedata->file_size = size;
	filedata->block_size = bsize;
	filedata->nblocks = h->get_nblocks;
	filedata->created_at = created_at;
	strncpy(filedata->name, filename, sizeof(filedata->name));
	strncpy(filedata->revision, rev, sizeof(filedata->revision));
    }
    return OK;
}

uint64_t sx_hashfs_getfile_count(sx_hashfs_t *h)
{
    return h->get_nblocks;
}

rc_ty sx_hashfs_getfile_block(sx_hashfs_t *h, const sx_hash_t **hash, sx_nodelist_t **nodes) {
    if(!h || !hash || !nodes || (h->get_nblocks && !h->get_content))
	return EINVAL;

    if(!h->get_nblocks)
	return ITER_NO_MORE;

    /* NEXTPREV would be more efficient
     * (because it's pointless to lookup new blocks in PREV)
     * but PREVNEXT is not prone to the following race condition:
     * 1. client -> next: not found
     * 2. prev -> next: move block
     * 3. client -> prev: not found */
    *nodes = sx_hashfs_hashnodes(h, NL_PREVNEXT, h->get_content, h->get_replica);
    if(!*nodes) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    *hash = h->get_content;
    h->get_content++;
    h->get_nblocks--;
    return OK;
}

void sx_hashfs_getfile_end(sx_hashfs_t *h) {
    sx_hashfs_getfile_reset(h);
    h->get_content = NULL;
    h->get_nblocks = 0;
    h->get_ndb = METADBS;
}

rc_ty sx_hashfs_block_get(sx_hashfs_t *h, unsigned int bs, const sx_hash_t *hash, const uint8_t **block) {
    unsigned int ndb = gethashdb(hash), hs;
    uint64_t dboff;
    int r;

    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", bs);
	return FAIL_BADBLOCKSIZE;
    }

    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", hash, sizeof(*hash)))
	return FAIL_EINTERNAL;

    r = qstep(h->qb_get[hs][ndb]);
    if(r == SQLITE_DONE) {
	char thash[41];
	DEBUG("Hash not in database");
	sqlite3_reset(h->qb_get[hs][ndb]);
	bin2hex(hash->b, 20, thash, 41);
/*        WARN("{%s}: hash %s missing",
	     sx_node_internal_addr(sx_nodelist_get(h->nodes, h->thisnode)), thash);*/
	return ENOENT;
    }
    if(r != SQLITE_ROW) {
	sqlite3_reset(h->qb_get[hs][ndb]);
	return FAIL_EINTERNAL;
    }
    if(!block) {
	sqlite3_reset(h->qb_get[hs][ndb]);
	return OK;
    }
    dboff = sqlite3_column_int64(h->qb_get[hs][ndb], 0);
    sqlite3_reset(h->qb_get[hs][ndb]);
    dboff *= bs;

    if(read_block(h->datafd[hs][ndb], h->blockbuf, dboff, bs))
	return FAIL_EINTERNAL;

    *block = h->blockbuf;
    return OK;
}

rc_ty sx_hashfs_hashop_begin(sx_hashfs_t *h, unsigned bs)
{
    unsigned hs;
    /* FIXME: code duplicated with block_put */
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES)
	return FAIL_BADBLOCKSIZE;
    h->put_hs = hs;
    /* would be nice to begin a transaction here, but the DB we use depends on
     * the hash...*/
    return OK;
}

static rc_ty sx_hashfs_hashop_ishash(sx_hashfs_t *h, unsigned hs, const sx_hash_t *hash)
{
    rc_ty ret;
    unsigned ndb;
    ndb = gethashdb(hash);
    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", hash, sizeof(*hash)))
        return FAIL_EINTERNAL;
    switch (qstep(h->qb_get[hs][ndb])) {
        case SQLITE_ROW:
            ret = OK;
            break;
        case SQLITE_DONE:
            ret = ENOENT;
            break;
        default:
            ret = FAIL_EINTERNAL;
            break;
    }
    sqlite3_reset(h->qb_get[hs][ndb]);
    return ret;
}

static rc_ty sx_hashfs_hashop_moduse(sx_hashfs_t *h, const char *id, unsigned int hs, const sx_hash_t *hash, unsigned replica, int64_t op, uint64_t op_expires_at)
{
    unsigned ndb;
    unsigned int age, n;
    sx_hash_t reserveid, tokenid;
    rc_ty ret = FAIL_EINTERNAL;
    /* FIXME: maybe enable this
    if(!is_hash_local(h, hash, replica_count))
	return ENOENT;*/

    if (!replica) {
        if (op) {
            msg_set_reason("replica is zero is only valid for reservations");
            return EINVAL;
        }
    }

    if (!op) {
        if (replica) {
            msg_set_reason("op=0 is valid only for reservations");
            return EINVAL;
        }
    }

    if (!id) {
        msg_set_reason("missing id");
        return EINVAL;
    }
    ndb = gethashdb(hash);

    sqlite3_reset(h->qb_moduse[hs][ndb]);
    sqlite3_reset(h->qb_op[hs][ndb]);
    sqlite3_reset(h->qb_reserve[hs][ndb]);
    sqlite3_reset(h->qb_add_reserve[hs][ndb]);
    n = strlen(id);
    if (n != SXI_SHA1_TEXT_LEN && n != SXI_SHA1_TEXT_LEN * 2) {
        WARN("wrong id length: %d", n);
        return EINVAL;
    }
    /* groupid = either the fileid, or a token.
     * When there is no activity on the groupid, the reservations on all hashes
     * from that groupid are dropped */
    if (hex2bin(id, SXI_SHA1_TEXT_LEN, reserveid.b, sizeof(reserveid.b))) {
        WARN("Cannot decode hash id: %s", id);
        return FAIL_EINTERNAL;
    }
    if (n == SXI_SHA1_TEXT_LEN*2) {
        if (hex2bin(id + SXI_SHA1_TEXT_LEN, SXI_SHA1_TEXT_LEN, tokenid.b, sizeof(tokenid.b))) {
            WARN("Cannot decode hash id: %s", id);
            return FAIL_EINTERNAL;
        }
    } else {
        memcpy(tokenid.b, reserveid.b, sizeof(tokenid.b));
    }
    DEBUG("moduse %lld, groupid: %s", (long long)op, id);
    DEBUGHASH("reserveid", &reserveid);
    DEBUGHASH("tokenid", &tokenid);
    if (qbegin(h->datadb[hs][ndb]))
        return FAIL_EINTERNAL;
    do {
        if (qbind_blob(h->qb_add_reserve[hs][ndb], ":hash", hash, sizeof(*hash)) ||
            qstep_noret(h->qb_add_reserve[hs][ndb]))
            break;
        sqlite3_reset(h->qb_add_reserve[hs][ndb]);
        if (op) { /* +N or -N */
            if (qbind_blob(h->qb_del_reserve[hs][ndb], ":reserveid", &reserveid, sizeof(reserveid)) ||
                qbind_blob(h->qb_del_reserve[hs][ndb], ":hash", hash, sizeof(*hash)) ||
                qstep_noret(h->qb_del_reserve[hs][ndb]))
                break;
            if (qbind_blob(h->qb_op[hs][ndb], ":hash", hash, sizeof(*hash)) ||
                qbind_blob(h->qb_op[hs][ndb], ":tokenid", &tokenid, sizeof(tokenid)) ||
                qbind_int(h->qb_op[hs][ndb], ":replica",replica) ||
                qbind_int64(h->qb_op[hs][ndb], ":op", op) ||
                qbind_int64(h->qb_op[hs][ndb], ":ttl", op_expires_at) ||
                qstep_noret(h->qb_op[hs][ndb]))
                break;
            sqlite3_reset(h->qb_op[hs][ndb]);
            age = sxi_hdist_version(h->hd);
            if (sqlite3_changes(h->datadb[hs][ndb]->handle)) {
                /* apply only non-dup operations */
                if (qbind_blob(h->qb_moduse[hs][ndb], ":hash", hash, sizeof(*hash)) ||
                    qbind_int(h->qb_moduse[hs][ndb], ":replica", replica) ||
                    qbind_int(h->qb_moduse[hs][ndb], ":age", age) ||
                    qbind_int64(h->qb_moduse[hs][ndb], ":used", op) ||
                    qstep_noret(h->qb_moduse[hs][ndb]))
                    break;
                DEBUGHASH("moduse on", hash);
                DEBUG("op: %ld, replica: %d, age: %d", op, replica, age);
            }
            sqlite3_reset(h->qb_moduse[hs][ndb]);
        } else {
            /* reserve */
            if (qbind_blob(h->qb_reserve[hs][ndb], ":hash", hash, sizeof(*hash)) ||
                qbind_blob(h->qb_reserve[hs][ndb], ":reserveid", &reserveid, sizeof(reserveid)) ||
                qbind_int64(h->qb_reserve[hs][ndb], ":ttl", op_expires_at) ||
                qstep_noret(h->qb_reserve[hs][ndb]))
                break;
            sqlite3_reset(h->qb_reserve[hs][ndb]);
        }
        if (qcommit(h->datadb[hs][ndb]))
            break;
        ret = OK;
    } while(0);
    if (ret)
        qrollback(h->datadb[hs][ndb]);
    return ret;
}

rc_ty sx_hashfs_hashop_finish(sx_hashfs_t *h, rc_ty rc)
{
    if (rc == ENOENT)
        rc = OK;
    /* would have to commit/rollback if we can batch updates */
    return rc;
}

rc_ty sx_hashfs_hashop_perform(sx_hashfs_t *h, unsigned replica_count, enum sxi_hashop_kind kind, const sx_hash_t *hash, const char *id, uint64_t op_expires_at)
{
    unsigned int hs = h->put_hs;
    rc_ty rc;
    if (UNLIKELY(sxi_log_is_debug(&logger))) {
        char debughash[sizeof(sx_hash_t)*2+1];		\
        bin2hex(hash->b, sizeof(*hash), debughash, sizeof(debughash));	\
        DEBUG("processing %s, #%s# (id: %s)",
              kind == HASHOP_RESERVE ? "reserve" :
              kind == HASHOP_INUSE ? "inuse" :
              kind == HASHOP_DELETE ? "decuse" : "??",
              debughash, id ? id : "");
    }
    switch (kind) {
        case HASHOP_CHECK:
            rc = sx_hashfs_hashop_ishash(h, hs, hash);
            break;
        case HASHOP_RESERVE:
            /* we must always reserve, even if ENOENT */
            rc = sx_hashfs_hashop_moduse(h, id, hs, hash, 0, 0, op_expires_at);
            if (rc == OK)
                rc = sx_hashfs_hashop_ishash(h, hs, hash);
            break;
        case HASHOP_INUSE:
            /* we must only moduse if not ENOENT */
            rc = sx_hashfs_hashop_ishash(h, hs, hash);
            if (rc)
                break;
            rc = sx_hashfs_hashop_moduse(h, id, hs, hash, replica_count, 1, op_expires_at);
            break;
        case HASHOP_DELETE:
            rc = sx_hashfs_hashop_moduse(h, id, hs, hash, replica_count, -1, op_expires_at);
            if (rc == OK)
                /* we must always moduse because we might run a rm during a
                 * rebalance, and we want to record the -1 so that it cancels
                 * out when the hash is finally pushed */
                rc = sx_hashfs_hashop_ishash(h, hs, hash);
            break;

        default:
            msg_set_reason("Invalid hashop");
            return EINVAL;
    }
    DEBUG("result: %s", rc2str(rc));
    return rc;
}

rc_ty sx_hashfs_hashop_mod(sx_hashfs_t *h, const sx_hash_t *hash, const char *id, unsigned int bs, unsigned replica, int64_t count, uint64_t op_expires_at)
{
    unsigned int hs;
    rc_ty rc;
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", bs);
	return FAIL_BADBLOCKSIZE;
    }
    rc = sx_hashfs_hashop_moduse(h, id, hs, hash, replica, count, op_expires_at);
    if (rc == OK)
        rc = sx_hashfs_hashop_ishash(h, hs, hash);
    return rc;
}

rc_ty sx_hashfs_block_put(sx_hashfs_t *h, const uint8_t *data, unsigned int bs, unsigned int replica_count, int propagate) {
    sx_nodelist_t *belongsto;
    unsigned int ndb, hs;
    sx_hash_t hash;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES)
	return FAIL_BADBLOCKSIZE;

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), data, bs, &hash)) {
	WARN("hashing failed");
	return FAIL_EINTERNAL;
    }

    DEBUGHASH("Block uploaded by user", &hash);

    /* MODHDIST: lookup is strictly on bidx 0 */
    belongsto = sxi_hdist_locate(h->hd, MurmurHash64(&hash, sizeof(hash), HDIST_SEED), replica_count, 0);
    r = sx_nodelist_lookup(belongsto, &h->node_uuid) == NULL;
    sx_nodelist_delete(belongsto);
    if(r) {
	DEBUGHASH("Block doesn't belong to this node", &hash);
	return ENOENT;
    }

    ndb = gethashdb(&hash);

    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", &hash, sizeof(hash))) {
	WARN("binding hash failed");
	return FAIL_EINTERNAL;
    }

    r = qstep(h->qb_get[hs][ndb]);
    sqlite3_reset(h->qb_get[hs][ndb]);
    if(r == SQLITE_DONE) {
	int64_t dsto, next;

	if(qbegin(h->datadb[hs][ndb])) {
	    WARN("begin failed");
	    return FAIL_EINTERNAL;
	}

	sqlite3_reset(h->qb_nextavail[hs][ndb]);
	sqlite3_reset(h->qb_nextalloc[hs][ndb]);
	sqlite3_reset(h->qb_bumpavail[hs][ndb]);
	sqlite3_reset(h->qb_bumpalloc[hs][ndb]);
	sqlite3_reset(h->qb_add[hs][ndb]);

	r = qstep(h->qb_nextavail[hs][ndb]);
	if(r == SQLITE_ROW) {
	    next = sqlite3_column_int64(h->qb_nextavail[hs][ndb], 0);
	    sqlite3_reset(h->qb_nextavail[hs][ndb]);

	    if(qbind_int64(h->qb_bumpavail[hs][ndb], ":next", next) || qstep_noret(h->qb_bumpavail[hs][ndb])) {
		qrollback(h->datadb[hs][ndb]);
		WARN("bumpavail failed");
		return FAIL_EINTERNAL;
	    }
	} else if(r == SQLITE_DONE) {
	    r = qstep(h->qb_nextalloc[hs][ndb]);
	    if(r == SQLITE_ROW) {
		next = sqlite3_column_int64(h->qb_nextalloc[hs][ndb], 0);
		sqlite3_reset(h->qb_nextalloc[hs][ndb]);

		if(qstep_noret(h->qb_bumpalloc[hs][ndb])) {
		    qrollback(h->datadb[hs][ndb]);
		    WARN("bumpalloc failed");
		    return FAIL_EINTERNAL;
		}
	    }
	}

	if(r != SQLITE_ROW || qcommit(h->datadb[hs][ndb])) {
	    qrollback(h->datadb[hs][ndb]);
	    WARN("nextavail failed");
	    return FAIL_EINTERNAL;
	}

	dsto = next * bs;
	DEBUG("Block stored @%d/%d/%ld", hs, ndb, dsto);

	if(write_block(h->datafd[hs][ndb], data, dsto, bs)) {
	    WARN("write failed");
	    return FAIL_EINTERNAL;
	}

	/* insert it now */
	if(qbind_blob(h->qb_add[hs][ndb], ":hash", &hash, sizeof(hash)) ||
           qbind_int64(h->qb_add[hs][ndb], ":now", time(NULL)) ||
	   qbind_int64(h->qb_add[hs][ndb], ":next", next)) {
	    WARN("add failed");
	    return FAIL_EINTERNAL;
	}
	r = qstep(h->qb_add[hs][ndb]);
        DEBUG("r: %d, changes: %d", r, sqlite3_changes(h->datadb[hs][ndb]->handle));
        if (r == SQLITE_DONE && !sqlite3_changes(h->datadb[hs][ndb]->handle)) {
            DEBUG("checking for race condition");
            /* race condition or missing reserve */
            r = qstep(h->qb_get[hs][ndb]);
            sqlite3_reset(h->qb_get[hs][ndb]);
            if (r == SQLITE_ROW) {
		DEBUG("Race in block_store, falling back");
                ret = EAGAIN;
            } else if (r == SQLITE_DONE) {
                WARNHASH("Hash was not reserved", &hash);
                sqlite3_reset(h->qb_add_reserve[hs][ndb]);
                if (qbind_blob(h->qb_add_reserve[hs][ndb], ":hash", &hash, sizeof(hash)) ||
                    qstep_noret(h->qb_add_reserve[hs][ndb]))
                    ret = FAIL_EINTERNAL;
                else {
                    sqlite3_reset(h->qb_add[hs][ndb]);
                    r = qstep(h->qb_add[hs][ndb]);
                    DEBUG("r: %d, changes: %d", r, sqlite3_changes(h->datadb[hs][ndb]->handle));
                    if (r == SQLITE_DONE && sqlite3_changes(h->datadb[hs][ndb]->handle))
                        ret = OK;
                    else {
                        WARN("r:%d or 0 changes", r);
                        ret = FAIL_EINTERNAL;
                    }
                }
                sqlite3_reset(h->qb_add[hs][ndb]);
                sqlite3_reset(h->qb_add_reserve[hs][ndb]);
            } else
                ret = FAIL_EINTERNAL;
        } else
            ret = OK;
    } else if(r == SQLITE_ROW)
        ret = EAGAIN;
    if(ret != OK && ret != EAGAIN)
	return FAIL_EINTERNAL;

    if(propagate && replica_count > 1) {
	sx_nodelist_t *targets = sx_hashfs_hashnodes(h, NL_NEXT, &hash, replica_count);
	rc_ty ret = sx_hashfs_xfer_tonodes(h, &hash, bs, targets);
	sx_nodelist_delete(targets);
	return ret;
    }
    return OK;
}

static void putfile_reinit(sx_hashfs_t *h) {
    if(!h)
	return;

    h->put_id = 0;
    h->put_putblock = 0;
    h->put_getblock = 0;
    h->put_checkblock = 0;
    h->put_replica = 0;
    h->put_hs = 0;
    h->put_success = 0;
    h->put_token[0] = '\0';
    h->put_blocks = NULL;
    h->put_nidxs = NULL;
    h->put_nblocks = 0;
    h->put_extendsize = -1LL;
    h->put_extendfrom = 0;
    h->nmeta = 0;
}

const char *sx_hashfs_geterrmsg(sx_hashfs_t *h)
{
    return sxc_geterrmsg(h->sx);
}

rc_ty sx_hashfs_createfile_begin(sx_hashfs_t *h) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    putfile_reinit(h);

    h->put_id = -1; /* Fake id so that the blocks are actually accepted */
    return OK;
}

rc_ty sx_hashfs_check_file_size(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const char *filename, int64_t size) {
    int64_t lastsize = 0;
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q;
    int mdb, r;

    if(!vol || size < 0 || !filename) {
        NULLARG();
        return FAIL_EINTERNAL;
    }

    mdb = getmetadb(filename);
    if(mdb < 0) {
        WARN("Failed to get meta db for file name: %s", filename);
        msg_set_reason("Failed to get meta db for file name: %s", filename);
        return FAIL_EINTERNAL;
    }

    q = h->qm_tooold[mdb];
    sqlite3_reset(q);

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename)) {
        WARN("Failed to bind query values");
        goto sh_hashfs_check_file_size_err;
    }

    r = qstep(q);
    if(r == SQLITE_DONE) {
        /* File was not stored yet, whole size should be checked */
    } else if(r == SQLITE_ROW) {
        int nrevs = sqlite3_column_int(h->qm_tooold[mdb], 2);
        if(nrevs >= vol->revisions) {
            /* There are too many revs, the oldest one will be overwritten, we need to take its size */
            lastsize = sqlite3_column_int64(h->qm_tooold[mdb], 3);
        }
    } else {
        WARN("Failed to query last file size");
        msg_set_reason("Failed to query last file size");
        goto sh_hashfs_check_file_size_err;
    }

    if(vol->cursize + size - lastsize > vol->size) {
        /* No space on volume */
        msg_set_reason("Quota exceeded");
        ret = ENOSPC;
        goto sh_hashfs_check_file_size_err;
    }

    ret = OK;
    sh_hashfs_check_file_size_err:
    sqlite3_reset(q);

    return ret;
}

/* Must be called inside transaction on h->db! */
static rc_ty update_volume_current_size(sx_hashfs_t *h, int64_t volume_id, int64_t size) {
    if(!volume_id) {
        CRIT("Invalid volume_id argument");
        return EINVAL;
    }

    sqlite3_reset(h->q_updatevolcursize);
    if(qbind_int64(h->q_updatevolcursize, ":size", size) ||
       qbind_int64(h->q_updatevolcursize, ":volume", volume_id) ||
       qstep_noret(h->q_updatevolcursize)) {
        msg_set_reason("Failed to update volume size");
        return FAIL_EINTERNAL;
    }

    return OK;
}

rc_ty sx_hashfs_putfile_begin(sx_hashfs_t *h, sx_uid_t user_id, const char *volume, const char *file, const sx_hashfs_volume_t **volptr) {
    uint8_t rnd[TOKEN_RAND_BYTES];
    const sx_hashfs_volume_t *vol;
    int flen;
    rc_ty r;

    putfile_reinit(h);

    flen = check_file_name(file);
    if(!h || flen < 0 || file[flen - 1] == '/')
	return EINVAL;

    if((r = sx_hashfs_volume_by_name(h, volume, &vol)))
	return r;

    if(volptr)
        *volptr = vol;

    if(!sx_hashfs_is_or_was_my_volume(h, vol))
	return ENOENT;

    sqlite3_reset(h->qt_new);
    /* non-blocking pseudo-random bytes, i.e. we don't want to block or deplete
     * entropy as we only need a unique sequence of bytes, not a secret one as
     * it is sent in plaintext anyway, and signed with an HMAC */
    if (sxi_rand_pseudo_bytes(rnd, sizeof(rnd)) == -1) {
	/* can also return 0 or 1 but that doesn't matter here */
	WARN("Cannot generate random bytes");
	return FAIL_EINTERNAL;
    }
    if(qbind_int64(h->qt_new, ":volume", vol->id) || qbind_text(h->qt_new, ":name", file) ||
       qbind_blob(h->qt_new, ":random", rnd, sizeof(rnd)) || qstep_noret(h->qt_new)) {
	sqlite3_reset(h->qt_new);
	return FAIL_EINTERNAL;
    }
    sqlite3_reset(h->qt_new);

    h->put_id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qt_new));
    h->put_replica = vol->replica_count;
    return sx_hashfs_countjobs(h, user_id);
}

rc_ty sx_hashfs_putfile_extend_begin(sx_hashfs_t *h, sx_uid_t user_id, const uint8_t *user, const char *token) {
    const sx_hashfs_volume_t *vol;
    const sx_uuid_t *self_uuid;
    struct token_data tkdt;
    const sx_node_t *self;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h)
	return EINVAL;

    putfile_reinit(h);

    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt))
	return EINVAL;

    if(!(self = sx_hashfs_self(h)) || !(self_uuid = sx_node_uuid(self)))
	return FAIL_EINTERNAL;
    if(memcmp(self_uuid->binary, tkdt.uuid.binary, sizeof(tkdt.uuid.binary)))
	return EINVAL;

    sqlite3_reset(h->qt_tokenstats);
    if(qbind_text(h->qt_tokenstats, ":token", tkdt.token))
	goto putfile_extend_err;

    r = qstep(h->qt_tokenstats);
    if(r == SQLITE_DONE)
	ret = ENOENT;
    if(r != SQLITE_ROW)
	goto putfile_extend_err;

    if((ret = sx_hashfs_volume_by_id(h, sqlite3_column_int64(h->qt_tokenstats, 2), &vol)))
	goto putfile_extend_err;

    h->put_id = sqlite3_column_int64(h->qt_tokenstats, 0);
    h->put_replica = vol->replica_count;
    h->put_extendsize = sqlite3_column_int64(h->qt_tokenstats, 1);
    h->put_extendfrom = sqlite3_column_int(h->qt_tokenstats, 3) / sizeof(sx_hash_t);
    ret = sx_hashfs_countjobs(h, user_id);

    putfile_extend_err:
    sqlite3_reset(h->qt_tokenstats);
    return ret;
}

rc_ty sx_hashfs_putfile_putblock(sx_hashfs_t *h, sx_hash_t *hash) {
    if(!h || !hash || !h->put_id)
	return EINVAL;

    if(h->put_putblock >= h->put_nblocks) {
	h->put_nblocks += 128;
	h->put_blocks = wrap_realloc_or_free(h->put_blocks, sizeof(*hash) * h->put_nblocks);
	if(!h->put_blocks)
	    return FAIL_EINTERNAL;
    }
    memcpy(&h->put_blocks[h->put_putblock], hash, sizeof(*hash));
    h->put_putblock++;
    return OK;
}

rc_ty sx_hashfs_putfile_putmeta(sx_hashfs_t *h, const char *key, void *value, unsigned int value_len) {
    rc_ty rc;

    if(!h)
	return FAIL_EINTERNAL;

    if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	return EOVERFLOW;

    if(!value) {
	/* Delete key: check key (and bogus value) */
	char checkme[SXLIMIT_META_MIN_VALUE_LEN + 1];
	memset(checkme, 0, sizeof(checkme));
	rc = sx_hashfs_check_meta(key, checkme, SXLIMIT_META_MIN_VALUE_LEN);
	if(rc)
	    return rc;
	h->meta[h->nmeta].value_len = -1;
    } else {
	/* Add/replace key: check key and value */
	rc = sx_hashfs_check_meta(key, value, value_len);
	if(rc)
	    return rc;
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
    }	

    memcpy(h->meta[h->nmeta].key, key, strlen(key)+1);

    h->nmeta++;
    return OK;
}

struct sort_by_node_t {
    sx_hash_t *hashes;
    unsigned int *nidxs;
    unsigned int sort_replica;
    unsigned int replica_count;
};

static int sort_by_node_func(const void *thunk, const void *a, const void *b) {
     unsigned int hashno_a = *(unsigned int *)a;
     unsigned int hashno_b = *(unsigned int *)b;
     const struct sort_by_node_t *support = (const struct sort_by_node_t *)thunk;

     int nidxa = support->nidxs[support->replica_count * hashno_a + support->sort_replica - 1];
     int nidxb = support->nidxs[support->replica_count * hashno_b + support->sort_replica - 1];

     /* Sort by node first */
     if(nidxa < nidxb)
	 return -1;
     if(nidxa > nidxb)
	 return 1;

     /* Then either sort by hash, if so requested */
     if(support->hashes)
	 return memcmp(&support->hashes[hashno_a], &support->hashes[hashno_b], sizeof(support->hashes[0]));

     /* Or alternatively sort by position */
     if(hashno_a < hashno_b)
	 return -1;
     if(hashno_a > hashno_b)
	 return 1;
     return 0;
}

static void sort_by_node_then_hash(sx_hash_t *hashes, unsigned int *hashnos, unsigned int *nidxs, unsigned int items, unsigned int replica, unsigned int replica_count) {
    struct sort_by_node_t sortsupport = {hashes, nidxs, replica, replica_count};
    sx_qsort(hashnos, items, sizeof(*hashnos), &sortsupport, sort_by_node_func);
}

static void sort_by_node_then_position(unsigned int *hashnos, unsigned int *nidxs, unsigned int items, unsigned int replica, unsigned int replica_count) {
    struct sort_by_node_t sortsupport = {NULL, nidxs, replica, replica_count};
    sx_qsort(hashnos, items, sizeof(*hashnos), &sortsupport, sort_by_node_func);
}

static int sort_by_hash_func(const void *thunk, const void *a, const void *b) {
    unsigned int ia = *(const unsigned int *)a;
    unsigned int ib = *(const unsigned int *)b;
    const sx_hash_t *hashes = (const sx_hash_t *)thunk;
    return cmphash(&hashes[ia], &hashes[ib]);
}

static void build_uniq_hash_index(const sx_hash_t *hashes, unsigned *idxs, unsigned *n)
{
    unsigned i, l, r;
    /* Build an index and an array of first nodes */
    for (i=0; i<*n; i++)
        idxs[i] = i;
    /* Sort by hash */
    sx_qsort(idxs, *n, sizeof(*idxs), hashes, sort_by_hash_func);
    /* Remove duplicates */
    for(l = 0, r = 1; r<*n; r++) {
        if(cmphash(&hashes[idxs[l]], &hashes[idxs[r]])) {
            l++;
            if(l!=r)
                idxs[l] = idxs[r];
        }
    }
    *n = l+1;
}

#ifdef FILEHASH_OPTIMIZATION
static rc_ty filehash_reserve(sx_hashfs_t *h, const sx_hash_t *filehash)
{
    rc_ty ret = FAIL_EINTERNAL, ret_ok = OK;
    unsigned gdb = getgcdb(filehash);
    sqlite3_reset(h->qg_filehash_add[gdb]);
    sqlite3_reset(h->qg_filehash_bump_reserved[gdb]);
    if (qbegin(h->gcdb[gdb]))
        return FAIL_EINTERNAL;
    do {
        int r;
        if (qbind_blob(h->qg_filehash_add[gdb], ":filehash", filehash, sizeof(*filehash)) ||
            qbind_blob(h->qg_filehash_bump_reserved[gdb], ":filehash", filehash, sizeof(*filehash)))
            break;
        r = qstep(h->qg_filehash_add[gdb]);
        if (r == SQLITE_CONSTRAINT) {
            /* just bump expiration timestamp */
            if (qstep_noret(h->qg_filehash_bump_reserved[gdb]))
                break;
            ret_ok = ITER_NO_MORE;/* do not bump the individual hashes */
            /* TODO: only if used > 0? */
        } else if (r != SQLITE_DONE) {
            SQLERR(h->qg_filehash_add[gdb], "filehash_add failed");
            break;
        }
        if (qcommit(h->gcdb[gdb]))
            break;
        ret = ret_ok;
    } while(0);
    if (ret != ret_ok) {
        qrollback(h->gcdb[gdb]);
    }
    sqlite3_reset(h->qg_filehash_add[gdb]);
    sqlite3_reset(h->qg_filehash_bump_reserved[gdb]);
    return ret;
}

static int filehash_is_used(sx_hashfs_t *h, const sx_hash_t *filehash)
{
    unsigned gdb = getgcdb(filehash);
    unsigned used;
    sqlite3_reset(h->qg_filehash_get[gdb]);
    if (qbind_blob(h->qg_filehash_get[gdb], ":filehash", filehash, sizeof(*filehash)) ||
        qstep_ret(h->qg_filehash_get[gdb]))
        return 0;

    used = sqlite3_column_int(h->qg_filehash_get[gdb], 0);
    sqlite3_reset(h->qg_filehash_get[gdb]);
    return used > 0;
}

static rc_ty filehash_mod_used(sx_hashfs_t *h, const sx_hash_t *filehash, int operation)
{
    rc_ty ret = FAIL_EINTERNAL, ret_ok = OK;
    unsigned gdb = getgcdb(filehash);
    sqlite3_reset(h->qg_filehash_mod_used[gdb]);
    sqlite3_reset(h->qg_filehash_get[gdb]);
    sqlite3_reset(h->qg_filehash_delete[gdb]);
    if (qbegin(h->gcdb[gdb]))
        return FAIL_EINTERNAL;
    do {
        unsigned used;
        if (qbind_blob(h->qg_filehash_mod_used[gdb], ":filehash", filehash, sizeof(*filehash)) ||
            qbind_blob(h->qg_filehash_get[gdb], ":filehash", filehash, sizeof(*filehash)) ||
            qbind_blob(h->qg_filehash_delete[gdb], ":filehash", filehash, sizeof(*filehash)) ||
            qbind_int(h->qg_filehash_mod_used[gdb], ":operation", operation) ||
            qstep_noret(h->qg_filehash_mod_used[gdb]) ||
            qstep_ret(h->qg_filehash_get[gdb]))
            break;
        used = sqlite3_column_int(h->qg_filehash_get[gdb], 0);
        if ((used == 1 && operation == 1) || /* used: 0 -> 1: we need to update hash counters */
            (used == 0 && operation == -1)) /* used : 1 -> 0: we need to update hash counters */
            ret_ok = OK;
        else
            ret_ok = ITER_NO_MORE;/* used is > 0, no need to modify it yet */
        if (used == 0)
            qstep_noret(h->qg_filehash_delete[gdb]);

        if (qcommit(h->gcdb[gdb]))
            break;
        ret = ret_ok;
    } while(0);
    if (ret != ret_ok)
        qrollback(h->gcdb[gdb]);
    sqlite3_reset(h->qg_filehash_mod_used[gdb]);
    sqlite3_reset(h->qg_filehash_get[gdb]);
    sqlite3_reset(h->qg_filehash_delete[gdb]);
    return ret;
}
#endif /* FILEHASH_OPTIMIZATION */

static int unique_tmpid(sx_hashfs_t *h, const char *token, sx_hash_t *hash)
{
    sx_uuid_t self;

    if (sx_hashfs_self_uuid(h, &self))
        return 1;
    return hash_buf(self.binary, sizeof(self.binary), token, strlen(token), hash);
}

static int reserve_fileid(sx_hashfs_t *h, int64_t volume_id, const char *name, sx_hash_t *hash)
{
    int ret = 0;
    sxi_md_ctx *hash_ctx = sxi_md_init();
    if (!hash_ctx)
        return 1;
    if (!sxi_sha1_init(hash_ctx))
        return 1;

    if (!sxi_sha1_update(hash_ctx, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary)) ||
        !sxi_sha1_update(hash_ctx, &volume_id, sizeof(volume_id)) ||
        !sxi_sha1_update(hash_ctx, name, strlen(name) + 1) ||
        !sxi_sha1_final(hash_ctx, hash->b, NULL)) {
        ret = 1;
    }

    sxi_md_cleanup(&hash_ctx);
    return ret;
}

rc_ty sx_hashfs_putfile_gettoken(sx_hashfs_t *h, const uint8_t *user, int64_t size_or_seq, const char **token, hash_presence_cb_t hdck_cb, void *hdck_cb_ctx) {
    const char *ptr;
    sqlite3_stmt *q;
    unsigned int i;
    uint64_t total_blocks;
    rc_ty ret = FAIL_EINTERNAL;
    unsigned int blocksize;
#ifdef FILEHASH_OPTIMIZATION
    sx_hash_t filehash;
#endif
    int64_t expires_at;

    if(!h || !h->put_id)
	return EINVAL;

    if(h->put_extendsize < 0) {
	/* creating */
	if(size_or_seq < SXLIMIT_MIN_FILE_SIZE || size_or_seq > SXLIMIT_MAX_FILE_SIZE) {
	    msg_set_reason("Cannot obtain upload token: file size must be between %llu and %llu bytes", SXLIMIT_MIN_FILE_SIZE, SXLIMIT_MAX_FILE_SIZE);
	    return EINVAL;
	}
	q = h->qt_update;
	total_blocks = size_to_blocks(size_or_seq, &h->put_hs, &blocksize);
	/* calculate expiry time of token proportional to the amount of data
	 * uploaded with _this_ token, i.e. we issue a new token for an extend.
	 * */
	sqlite3_reset(q);
	expires_at = time(NULL) + GC_GRACE_PERIOD + blocksize * total_blocks / h->upload_minspeed
            + GC_MIN_LATENCY * size_or_seq / UPLOAD_CHUNK_SIZE / 1000;
	if(qbind_int64(q, ":expiry", expires_at))
	    return FAIL_EINTERNAL;
    } else {
	/* extending */
	if(size_or_seq != h->put_extendfrom) {
	    msg_set_reason("Cannot obtain upload token: out of sequence");
	    return EINVAL;
	}
	q = h->qt_extend;
	total_blocks = size_to_blocks(h->put_extendsize, &h->put_hs, &blocksize);
	size_or_seq *= sizeof(sx_hash_t);
	sqlite3_reset(q);
    }

    if(h->put_putblock + h->put_extendfrom > total_blocks) {
	msg_set_reason("Cannot obtain upload token: cannot extend beyond the file size");
	return EINVAL;
    }

    if(qbind_int64(q, ":id", h->put_id) ||
       qbind_int64(q, ":size", size_or_seq))
	goto gettoken_err;

    if(h->put_putblock) {
	if(qbind_blob(q, ":all", h->put_blocks, sizeof(h->put_blocks[0]) * h->put_putblock))
	    goto gettoken_err;
#ifdef FILEHASH_OPTIMIZATION
        if (hash_buf("", 0, h->put_blocks, sizeof(h->put_blocks[0]) * h->put_putblock, &filehash)) {
            WARN("Failed to calculate file hash");
            goto gettoken_err;
        }
#endif
	h->put_nidxs = wrap_malloc(h->put_putblock * sizeof(h->put_nidxs[0]) * 2);
	if(!h->put_nidxs) {
	    OOM();
	    goto gettoken_err;
	}
	h->put_hashnos = &h->put_nidxs[h->put_putblock];
        build_uniq_hash_index(h->put_blocks, h->put_hashnos, &h->put_putblock);

	/* Lookup first node */
	for(i=0; i<h->put_putblock; i++) {
	    /* We (via hash_nidx_tobuf) check presence on the ultimate target nodes, i.e. NL_NEXT */
	    if(hash_nidx_tobuf(h, &h->put_blocks[h->put_hashnos[i]], 1, &h->put_nidxs[h->put_hashnos[i]]))
		goto gettoken_err;
	}
	if(h->put_putblock > 1) {
	    /* Group by node, then by original position
	     * so that we second the readahead instead of fighting it */
	    sort_by_node_then_position(h->put_hashnos, h->put_nidxs, h->put_putblock, 1, 1);
	}
	if(h->put_extendfrom) {
	    for(i=0; i<h->put_putblock; i++)
		h->put_hashnos[i] += h->put_extendfrom;
	}
	if(qbind_blob(q, ":uniq", h->put_hashnos, sizeof(h->put_hashnos[0]) * h->put_putblock))
	    goto gettoken_err;
	if(h->put_extendfrom) {
	    for(i=0; i<h->put_putblock; i++)
		h->put_hashnos[i] -= h->put_extendfrom;
	}
    } else {
	if(qbind_blob(q, ":all", "", 0) || qbind_blob(q, ":uniq", "", 0))
	    goto gettoken_err;
#ifdef FILEHASH_OPTIMIZATION
        if (hash_buf("", 0, "", 0, &filehash)) {
            WARN("Failed to calculate file hash");
            goto gettoken_err;
        }
#endif
    }

    if(qstep_noret(q))
	goto gettoken_err;
    sqlite3_reset(q);

    if(h->nmeta) {
	int items;
	for(i=0; i<h->nmeta; i++) {
	    sqlite3_stmt *q;
	    if(h->meta[i].value_len < 0)
		q = h->qt_delmeta;
	    else
		q = h->qt_addmeta;

	    sqlite3_reset(q);
	    if(qbind_int64(q, ":id", h->put_id) ||
	       qbind_text(q, ":key", h->meta[i].key) ||
	       (h->meta[i].value_len >=0 && qbind_blob(q, ":value", h->meta[i].value, h->meta[i].value_len)) ||
	       qstep_noret(q))
		goto gettoken_err;
	    sqlite3_reset(q);
	}
	sqlite3_reset(h->qt_countmeta);
	if(qbind_int64(h->qt_countmeta, ":id", h->put_id) ||
	   qstep_ret(h->qt_countmeta)) {
	    sqlite3_reset(h->qt_countmeta);
	    goto gettoken_err;
	}

	items = sqlite3_column_int(h->qt_countmeta, 0);
	sqlite3_reset(h->qt_countmeta);
	if(items > SXLIMIT_META_MAX_ITEMS) {
	    ret = EOVERFLOW;
	    goto gettoken_err;
	}
    }

    sqlite3_reset(h->qt_gettoken);
    if(qbind_int64(h->qt_gettoken, ":id", h->put_id) || qstep_ret(h->qt_gettoken))
	goto gettoken_err;

    ptr = (const char *)sqlite3_column_text(h->qt_gettoken, 0);
    expires_at = sqlite3_column_int64(h->qt_gettoken, 1);
    if(sx_hashfs_make_token(h, user, ptr, h->put_replica, expires_at, token))
	goto gettoken_err;


    if (reserve_fileid(h, sqlite3_column_int64(h->qt_gettoken, 2), (const char*)sqlite3_column_text(h->qt_gettoken, 3), &h->put_reserve_id))
        goto gettoken_err;
    DEBUGHASH("file initial PUT reserveid", &h->put_reserve_id);
    sxi_hashop_begin(&h->hc, h->sx_clust, hdck_cb, HASHOP_RESERVE, 0, NULL, &h->put_reserve_id, hdck_cb_ctx, expires_at);
#ifdef FILEHASH_OPTIMIZATION
    if (filehash_reserve(h, &filehash) == ITER_NO_MORE) {
        h->put_checkblock = h->put_putblock;/* no need to reserve each hash, we bumped the filehash's counter */
        DEBUG("skipping hash reserves");
    }
#endif

    sqlite3_reset(h->qt_gettoken);
    return OK;

    gettoken_err:
    sqlite3_reset(h->qt_addmeta);
    sqlite3_reset(h->qt_delmeta);
    sqlite3_reset(q);
    sqlite3_reset(h->qt_gettoken);
    return ret;
}

/* WARNING: MUST BE CALLED WITHIN A TANSACTION ON META !!! */
static rc_ty create_file(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, const char *revision, sx_hash_t *blocks, unsigned int nblocks, int64_t size, int64_t *file_id) {
    unsigned int nblocks2;
    int r, mdb;

    if(!h || !volume || !name || !revision || (!blocks && nblocks)) {
	NULLARG();
	return EFAULT;
    }

    if(check_file_name(name)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid revision");
	return EINVAL;
    }

    nblocks2 = size_to_blocks(size, NULL, NULL);
    if(nblocks != nblocks2) {
	WARN("Inconsistent size: %u blocks given, %u expected", nblocks, nblocks2);
	return EFAULT;
    }

    mdb = getmetadb(name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    /* Count current file revisions */
    sqlite3_reset(h->qm_tooold[mdb]);
    if(qbind_int64(h->qm_tooold[mdb], ":volume", volume->id) ||
       qbind_text(h->qm_tooold[mdb], ":name", name))
	return FAIL_EINTERNAL;

    r = qstep(h->qm_tooold[mdb]);
    if(r == SQLITE_ROW) {
	/* There are some revs */
	const char *tooold_rev = (const char *)sqlite3_column_text(h->qm_tooold[mdb], 1);
	int nrevs = sqlite3_column_int(h->qm_tooold[mdb], 2);
	int can_replace = tooold_rev && strcmp(revision, tooold_rev) >= 0;
        rc_ty rc = EINVAL;

        do {
            if(nrevs >= volume->revisions) {
                /* There are too many revs */
                if(!can_replace) {
                    /* All existing revs are more recent than this one */
                    msg_set_reason("Newer copies of this file already exist");
                    break;
                }

                /* Remove the oldest */
                if (sx_hashfs_file_delete(h, volume, name, tooold_rev)) {
                    /* Removal failed */
                    msg_set_reason("Failed to delete older file revision: %s", tooold_rev);
                    break;
                }
            }
            rc = OK;
        } while(0);
        sqlite3_reset(h->qm_tooold[mdb]);
        if (rc)
            return rc;
        /* Yay we have a slot now */
    } else {
	sqlite3_reset(h->qm_tooold[mdb]);
	if(r != SQLITE_DONE) /* Something didn't quite work */
	    return FAIL_EINTERNAL;

	/* There are no existing revs */
    }

    sqlite3_reset(h->qm_ins[mdb]);
    if(qbind_int64(h->qm_ins[mdb], ":volume", volume->id) ||
       qbind_text(h->qm_ins[mdb], ":name", name) ||
       qbind_text(h->qm_ins[mdb], ":revision", revision) ||
       qbind_int64(h->qm_ins[mdb], ":size", size) ||
       qbind_blob(h->qm_ins[mdb], ":hashes", nblocks ? (const void *)blocks : "", nblocks * sizeof(blocks[0]))) {
	WARN("Failed to create file '%s' on volume '%s'", name, volume->name);
	sqlite3_reset(h->qm_ins[mdb]);
	return FAIL_EINTERNAL;
    }

    r = qstep(h->qm_ins[mdb]);
    sqlite3_reset(h->qm_ins[mdb]);
    if(r == SQLITE_CONSTRAINT) {
	INFO("File '%s (%s)' on volume '%s' is already here", name, revision, volume->name);
	return EEXIST;
    } else if(r != SQLITE_DONE) {
	WARN("Failed to create file '%s' on volume '%s'", name, volume->name);
	return FAIL_EINTERNAL;
    }

    if(file_id)
	*file_id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qm_ins[mdb]));

    if(size && update_volume_current_size(h, volume->id, size)) {
        WARN("Failed to update volume size");
        return FAIL_EINTERNAL;
    }

    return OK;
}

rc_ty sx_hashfs_createfile_commit(sx_hashfs_t *h, const char *volume, const char *name, const char *revision, int64_t size) {
    const sx_hashfs_volume_t *vol;
    unsigned int i, nblocks;
    int64_t file_id;
    int mdb, flen;
#ifdef FILEHASH_OPTIMIZATION
    sx_hash_t filehash;
#endif
    rc_ty ret = FAIL_EINTERNAL, ret2;

    if(!h || !name || !revision || h->put_id != -1) {
	NULLARG();
	return EFAULT;
    }

    if(check_file_name(name)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid revision");
	return EINVAL;
    }

    flen = check_file_name(name);
    if(flen < 0 || name[flen - 1] == '/') {
	msg_set_reason("Bad file name");
	return EINVAL;
    }

    nblocks = size_to_blocks(size, NULL, NULL);
    if(h->put_putblock != nblocks) {
	msg_set_reason("Blocks do not match the file size");
	return EINVAL;
    }

    if((ret2 = sx_hashfs_volume_by_name(h, volume, &vol)))
	return ret2;

    if(!sx_hashfs_is_or_was_my_volume(h, vol)) {
	msg_set_reason("This volume does not belong here");
	return ENOENT;
    }

    mdb = getmetadb(name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    if(qbegin(h->metadb[mdb]))
	return FAIL_EINTERNAL;

#ifdef FILEHASH_OPTIMIZATION
    /* direct file creation during replication: adjust filehash counters
     * accordingly */
    if (hash_buf("", 0, h->put_blocks, nblocks * sizeof(h->put_blocks[0]), &filehash)) {
        WARN("Failed to calculate file hash");
        return FAIL_EINTERNAL;
    }
    filehash_reserve(h, &filehash);
#endif
    ret2 = create_file(h, vol, name, revision, h->put_blocks, nblocks, size, &file_id);
    if(ret2) {
	ret = ret2;
	goto cretatefile_rollback;
    }
#ifdef FILEHASH_OPTIMIZATION
    filehash_mod_used(h, &filehash, 1);
#endif

    for(i=0; i<h->nmeta; i++) {
	sqlite3_reset(h->qm_metaset[mdb]);
	if(qbind_int64(h->qm_metaset[mdb], ":file", file_id) ||
	   qbind_text(h->qm_metaset[mdb], ":key", h->meta[i].key) ||
	   qbind_blob(h->qm_metaset[mdb], ":value", h->meta[i].value, h->meta[i].value_len) ||
	   qstep_noret(h->qm_metaset[mdb]))
	    break;
    }
    sqlite3_reset(h->qm_metaset[mdb]);
    if(i != h->nmeta)
	goto cretatefile_rollback;

    if(qcommit(h->metadb[mdb]))
	goto cretatefile_rollback;

    ret = OK;

 cretatefile_rollback:
    if(ret != OK)
	qrollback(h->metadb[mdb]);

    if(ret == EEXIST)
	ret = OK;
    sx_hashfs_createfile_end(h);

    return ret;
}

static rc_ty are_blocks_available(sx_hashfs_t *h, sx_hash_t *hashes,
				  sxi_hashop_t *hdck,
				  unsigned int *hashnos, unsigned int *nidxs,
				  unsigned int *current, unsigned int count,
				  unsigned int hash_size, unsigned int check_replica,
				  unsigned int replica_count) {
    /* h: main hashfs
     * hashes: complete array of hashes, unsorted
     * hashnos: array of hash indexes sorted by node id
     * nidxs: array of node indexes
     * current: current hash to be checked
     * count: number of hashes in the list (also the numer of items in hashnos the number of items per replica in nidxs)
     * hash_size: the size (small, medium, big) of the hashes in hashes
     * check_replica: which set to check (1 <= check_replica <= replica_count)
     * replica_count: the number of replica sets
     */

    /* MODHDIST:
     * this is a major PITA. With the current api (and callbacks) we can only check presence
     * on a single set, which, for functional reasons outta be the _next set.
     * In practice this means that the clients (i.e. cluster users) effectively help the
     * migration process with their bandwith. This also means that the clients (i.e. the users) might
     * experience unexpected spikes in resource usage for reasons which are outside their control.
     * A better approach would be to check presence on _prev, then _next and merge the results.
     * This is however not possible without a major api rework :(
     */
    const sx_nodelist_t *nodes = sx_hashfs_nodelist(h, NL_NEXT);
    unsigned int check_item = *current;
    unsigned int thisnode, nextnode, prevnode = nidxs[replica_count * hashnos[check_item] + check_replica - 1];

    if(!count)
	return OK;

    if(check_item >= count) {
	WARN("bad check_item: %d >= %d", check_item, count);
	return FAIL_EINTERNAL;
    }

    /* hash is local */
    if(sx_nodelist_lookup_index(nodes, &h->node_uuid, &thisnode) && prevnode == thisnode) {
	sx_hash_t *hash = &hashes[hashnos[check_item]];
	unsigned int ndb = gethashdb(hash);
	int r;
        rc_ty rc;

	sqlite3_reset(h->qb_get[hash_size][ndb]);
	if(qbind_blob(h->qb_get[hash_size][ndb], ":hash", hash, sizeof(*hash))) {
	    WARN("qbind_blob failed");
	    sqlite3_reset(h->qb_get[hash_size][ndb]);
	    return FAIL_EINTERNAL;
	}
	r = qstep(h->qb_get[hash_size][ndb]);
	sqlite3_reset(h->qb_get[hash_size][ndb]);

	*current = check_item+1;
        if (sxi_hashop_batch_flush(hdck))
            WARN("Failed to query hash: %s", sxc_geterrmsg(h->sx));
	hdck->queries++;
	hdck->finished++;
	if (r == SQLITE_ROW)
	    hdck->ok++;
	else
	    hdck->enoent++;
        rc = sx_hashfs_hashop_begin(h, bsz[hash_size]);
        if (rc) {
            WARN("hashop_begin failed: %s", rc2str(rc));
            return rc;
        }
        rc = sx_hashfs_hashop_perform(h, replica_count, hdck->kind, hash, hdck->id, hdck->op_expires_at);
        rc = sx_hashfs_hashop_finish(h, rc);
        if (rc && rc != ENOENT) {
            WARN("hashop_perform/finish failed: %s", rc2str(rc));
            return rc;
        }
	if (hdck->cb) {
	    int code = r == SQLITE_ROW ? 200 : 404;
	    char thash[SXI_SHA1_TEXT_LEN + 1];
	    if (bin2hex(hash->b, SXI_SHA1_BIN_LEN, thash, sizeof(thash))) {
		WARN("bin2hex failed for hash");
		return FAIL_EINTERNAL;
	    }
	    if (hdck->cb(thash, check_item, code, hdck->context) == -1) {
		WARN("callback returned failure");
		return FAIL_EINTERNAL;
	    }
	}
	return OK;
    }

    const sx_node_t *node = sx_nodelist_get(nodes, prevnode);
    if(!node) {
	WARN("failed to get nodelist");
	return FAIL_EINTERNAL;
    }
    const char *host = sx_node_internal_addr(node);
    DEBUG("preparing request to %s, node #%d, replica count: %d, current replica: %d", host, prevnode, replica_count,
          check_replica);
    DEBUG("nodelist:");
    for (unsigned i=0;i<sx_nodelist_count(nodes);i++) {
        DEBUG("node #%d: %s", i, sx_node_internal_addr(sx_nodelist_get(nodes, i)));
    }
    do {
        if (sxi_hashop_batch_add(hdck, host, check_item, hashes[hashnos[check_item]].b, bsz[hash_size])) {
            WARN("Failed to query hash on %s: %s", host, sxc_geterrmsg(h->sx));
        }
        DEBUGHASH("preparing query for hash", &hashes[hashnos[check_item]]);
        DEBUG("index: %d, blockno: %d", check_item, hashnos[check_item]);
	check_item++;
	if(check_item >= count)
	    break; /* end of set for this replica */
	nextnode = nidxs[replica_count * hashnos[check_item] + check_replica - 1];
    } while (nextnode == prevnode); /* end of current node */

    *current = check_item;
    return OK;
}

rc_ty reserve_replicas(sx_hashfs_t *h, uint64_t op_expires_at)
{
    /*
     * assign more understandable names to pointers and counters
     */
    rc_ty ret = OK;
    sx_hash_t *all_hashes = h->put_blocks;
    sxi_hashop_t *hashop = &h->hc;
    unsigned int *uniq_hash_indexes = h->put_hashnos;
    unsigned uniq_count = h->put_putblock;
    if (!uniq_count)
        return OK;
    unsigned int *node_indexes = wrap_malloc((1+h->put_replica) * h->put_nblocks * sizeof(*node_indexes));
    unsigned hash_size = h->put_hs;
    if (!node_indexes)
        return ENOMEM;

    unsigned i;
    for(i=0; i<uniq_count; i++) {
	/* MODHDIST: pick from _next, bidx=0 */
	if(hash_nidx_tobuf(h, &all_hashes[uniq_hash_indexes[i]],
                           h->put_replica, &node_indexes[uniq_hash_indexes[i]*h->put_replica])) {
	    WARN("hash_nidx_tobuf failed");
            ret = FAIL_EINTERNAL;
	}
    }
    DEBUG("reserve_replicas begin");
    for(i=2; ret == OK && i<=h->put_replica; i++) {
        unsigned int cur_item = 0;
        sort_by_node_then_hash(all_hashes, uniq_hash_indexes, node_indexes, uniq_count, i, h->put_replica);
        memset(hashop, 0, sizeof(*hashop));
        DEBUGHASH("reserve_replicas reserveid", &h->put_reserve_id);
        sxi_hashop_begin(hashop, h->sx_clust, NULL,
                         HASHOP_RESERVE, 0, NULL, &h->put_reserve_id, NULL, op_expires_at);
        while((ret = are_blocks_available(h, all_hashes, hashop,
                                          uniq_hash_indexes, node_indexes,
                                          &cur_item, uniq_count, hash_size,
                                          i, h->put_replica)) == OK) {
            if(cur_item >= uniq_count)
                break;
        }
        if (ret)
            WARN("are_blocks_available failed: %s", rc2str(ret));
        if (!ret && sxi_hashop_end(hashop) == -1) {
            WARN("sxi_hashop_end failed: %s", sxc_geterrmsg(h->sx));
            ret = FAIL_EINTERNAL;
        }
    }
    DEBUG("reserve_replicas end");
    free(node_indexes);
    return ret;
}

rc_ty sx_hashfs_putfile_getblock(sx_hashfs_t *h) {
    rc_ty ret;
    if(!h || !h->put_token[0])
	return EINVAL;

    if(h->put_checkblock >= h->put_putblock) {
        uint64_t op_expires_at = h->hc.op_expires_at;
	if(sxi_hashop_end(&h->hc) == -1) {
	    WARN("hashop_end failed: %s", sxc_geterrmsg(h->sx));
            return FAIL_EINTERNAL;
        } else
	    DEBUG("{%s}: finished:%d, queries:%d, ok:%d, enoent:%d, cbfail:%d",
		  h->node_uuid.string,
		  h->hc.finished, h->hc.queries, h->hc.ok, h->hc.enoent, h->hc.cb_fail);
        ret = reserve_replicas(h, op_expires_at);
        if (ret) {
            WARN("failed to reserve replicas: %s", rc2str(ret));
            return ret;
        }
	h->put_success = 1;
	return ITER_NO_MORE;
    }
    ret = are_blocks_available(h, h->put_blocks, &h->hc, h->put_hashnos, h->put_nidxs, &h->put_checkblock, h->put_putblock, h->put_hs, 1, 1);
    return ret;
}

void sx_hashfs_createfile_end(sx_hashfs_t *h) {
    if(!h)
	return;

    free(h->put_blocks);
    putfile_reinit(h);
}

void sx_hashfs_putfile_end(sx_hashfs_t *h) {
    if(!h)
	return;
    /* ensure no callbacks are running anymore, or they'd access
     * a wrong ctx data */
    sxi_hashop_end(&h->hc);
    memset(&h->hc, 0, sizeof(h->hc));

    free(h->put_blocks);
    free(h->put_nidxs);

    if(!h->put_success && h->put_id) {
	sqlite3_reset(h->qt_delete);
	if(!qbind_int64(h->qt_delete, ":id", h->put_id))
	    qstep_noret(h->qt_delete);
	sqlite3_reset(h->qt_delete);
    }

    putfile_reinit(h);
}

unsigned int sx_hashfs_job_file_timeout(sx_hashfs_t *h, unsigned int ndests, uint64_t size)
{
    unsigned int blocks = size_to_blocks(size, NULL, NULL), timeout = 35;
    timeout += 20 * (ndests-1);
    timeout += 10 * blocks;

    /* Empirically determined very crude cap; to be reworked later */
    if(timeout < 160 * ndests)
	timeout = 160 * ndests;

    if(timeout > JOB_FILE_MAX_TIME)
	timeout = JOB_FILE_MAX_TIME;

    return timeout;
}

rc_ty sx_hashfs_putfile_commitjob(sx_hashfs_t *h, const uint8_t *user, sx_uid_t user_id, const char *token, job_t *job_id) {
    unsigned int expected_blocks, actual_blocks, ndests;
    int64_t tmpfile_id, expected_size, volid;
    rc_ty ret = FAIL_EINTERNAL, ret2;
    sx_nodelist_t *singlenode = NULL, *volnodes = NULL;
    const sx_hashfs_volume_t *vol;
    const sx_uuid_t *self_uuid;
    const sx_node_t *self;
    struct token_data tkdt;
    int r, has_begun = 0;

    if(!h || !user || !job_id) {
	NULLARG();
	return EFAULT;
    }
    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt)) {
	WARN("bad token: %s", token);
	return EINVAL;
    }

    if(!(self = sx_hashfs_self(h)) || !(self_uuid = sx_node_uuid(self)))
	return FAIL_EINTERNAL;
    if(memcmp(self_uuid->binary, tkdt.uuid.binary, sizeof(tkdt.uuid.binary))) {
	WARN("bad token uuid");
	return EINVAL;
    }

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }
    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret) {
	WARN("Cannot add self to nodelist");
	sx_nodelist_delete(singlenode);
	return ret;
    }

    if(qbegin(h->tempdb))
	goto putfile_commitjob_err;
    has_begun = 1;

    sqlite3_reset(h->qt_tokenstats);
    if(qbind_text(h->qt_tokenstats, ":token", tkdt.token))
	goto putfile_commitjob_err;
    r = qstep(h->qt_tokenstats);
    if(r == SQLITE_DONE) {
        msg_set_reason("Token is unknown or already flushed");
	ret = ENOENT;
    }
    if(r != SQLITE_ROW)
	goto putfile_commitjob_err;

    tmpfile_id = sqlite3_column_int64(h->qt_tokenstats, 0);
    expected_size = sqlite3_column_int64(h->qt_tokenstats, 1);
    volid = sqlite3_column_int64(h->qt_tokenstats, 2);
    ret2 = sx_hashfs_volume_by_id(h, volid, &vol);
    if(ret2) {
	WARN("Cannot locate volume %lld for tmp file %lld", (long long)volid, (long long)tmpfile_id);
	ret = ret2;
	goto putfile_commitjob_err;
    }
    /* Flushed tempfiles are propagated to all volnodes (both PREV and NEXT),
     * in no particular order (NL_PREVNEXT would be fine as well) */
    ret2 = sx_hashfs_volnodes(h, NL_NEXTPREV, vol, 0, &volnodes, NULL);
    if(ret2) {
	WARN("Cannot determine volume nodes for '%s'", vol->name);
	ret = ret2;
	goto putfile_commitjob_err;
    }
    actual_blocks = sqlite3_column_int64(h->qt_tokenstats, 3);
    if(actual_blocks % sizeof(sx_hash_t)) {
	msg_set_reason("Corrupted token data");
	goto putfile_commitjob_err;
    }
    actual_blocks /= sizeof(sx_hash_t);
    expected_blocks = size_to_blocks(expected_size, NULL, NULL);
    if(actual_blocks != expected_blocks) {
	/* File was not extended enough to match its size */
	msg_set_reason("Token not extended to its final size");
	ret = EINVAL;
	goto putfile_commitjob_err;
    }

    sqlite3_reset(h->qt_flush);
    if(qbind_int64(h->qt_flush, ":id", tmpfile_id) ||
       qstep_noret(h->qt_flush)) {
	/* The job itself will fail in case a token is still present */
	goto putfile_commitjob_err;
    }

    ndests = sx_nodelist_count(volnodes);
    ret2 = sx_hashfs_job_new_begin(h);
    if(ret2) {
        INFO("Failed to begin jobadd");
	ret = ret2;
	goto putfile_commitjob_err;
    }

    ret2 = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, user_id, job_id, JOBTYPE_REPLICATE_BLOCKS, sx_hashfs_job_file_timeout(h, ndests, expected_size), token, &tmpfile_id, sizeof(tmpfile_id), singlenode);
    if(ret2) {
        INFO("job_new (replicate) returned: %s", rc2str(ret2));
	ret = ret2;
	goto putfile_commitjob_err;
    }

    ret2 = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_FLUSH_FILE, 60*ndests, token, &tmpfile_id, sizeof(tmpfile_id), volnodes);
    if(ret2) {
        INFO("job_new (flush) returned: %s", rc2str(ret2));
	ret = ret2;
	goto putfile_commitjob_err;
    }

    ret2 = sx_hashfs_job_new_end(h);
    if(ret2) {
        INFO("Failed to commit jobadd");
	ret = ret2;
	goto putfile_commitjob_err;
    }

    if(qcommit(h->tempdb))
	goto putfile_commitjob_err;

    ret = OK;
    sx_hashfs_job_trigger(h);

 putfile_commitjob_err:
    if(ret != OK && has_begun)
	qrollback(h->tempdb);

    sqlite3_reset(h->qt_tokenstats);

    sx_nodelist_delete(volnodes);
    sx_nodelist_delete(singlenode);

    return ret;
}

static int tmp_getmissing_cb(const char *hexhash, unsigned int index, int code, void *context) {
    sx_hashfs_tmpinfo_t *mis = (sx_hashfs_tmpinfo_t *)context;
    sx_hash_t binhash;
    unsigned int blockno;

    if(!hexhash || !mis)
	return -1;
    hex2bin(hexhash, SXI_SHA1_TEXT_LEN, binhash.b, sizeof(binhash));
    blockno = mis->uniq_ids[index];
    unsigned int pushingidx = mis->nidxs[blockno * mis->replica_count +mis->current_replica - 1];
    const sx_node_t *pusher = sx_nodelist_get(mis->allnodes, pushingidx);
    DEBUG("nodelist:");

    for (unsigned i=0;i<sx_nodelist_count(mis->allnodes);i++) {
        DEBUG("node #%d: %s", i, sx_node_internal_addr(sx_nodelist_get(mis->allnodes, i)));
    }

    DEBUG("remote hash #%.*s#: %d, index #%d, blockno %d, set %u, node %s (#%d), replica count: %d, current replica: %d", SXI_SHA1_TEXT_LEN, hexhash, code,
          index, blockno,
          mis->current_replica-1, sx_node_internal_addr(pusher), pushingidx, mis->replica_count, mis->current_replica);
    DEBUG("remote hash #%.*s#: %d", SXI_SHA1_TEXT_LEN, hexhash, code);
    if(code != 200)
	return 0;

    if(index >= mis->nuniq) {
	WARN("Index out of bounds");
	return -1;
    }

    hex2bin(hexhash, SXI_SHA1_TEXT_LEN, binhash.b, sizeof(binhash));
    blockno = mis->uniq_ids[index];
    if(memcmp(&mis->all_blocks[blockno], &binhash, sizeof(binhash))) {
	char idxhash[SXI_SHA1_TEXT_LEN + 1];
	bin2hex(&mis->all_blocks[blockno], sizeof(mis->all_blocks[0]), idxhash, sizeof(idxhash));
	WARN("Hash mismatch: called for %.*s but index %d points to %s", SXI_SHA1_TEXT_LEN, hexhash, index, idxhash);
	return -1;
    }

    if(mis->avlblty[blockno * mis->replica_count + mis->current_replica - 1] != 1) {
	mis->avlblty[blockno * mis->replica_count + mis->current_replica - 1] = 1;
	mis->somestatechanged = 1;
        DEBUG("(cb): Block %.*s set %u is NOW available on node %c",
              SXI_SHA1_TEXT_LEN, hexhash, mis->current_replica - 1, 'a' +
              mis->nidxs[blockno * mis->replica_count + mis->current_replica - 1]);
    }
    return 0;
}

static int unique_fileid(sxc_client_t *sx, const sx_hashfs_volume_t *volume, const char *name, const char *revision, sx_hash_t *fileid)
{
    int ret = 0;
    sxi_md_ctx *hash_ctx = sxi_md_init();
    if (!hash_ctx)
        return 1;
    if (!sxi_sha1_init(hash_ctx))
        return 1;

    if (!sxi_sha1_update(hash_ctx, volume->name, strlen(volume->name) + 1) ||
        !sxi_sha1_update(hash_ctx, name, strlen(name) + 1) ||
        !sxi_sha1_update(hash_ctx, revision, strlen(revision)) ||
        !sxi_sha1_final(hash_ctx, fileid->b, NULL)) {
        ret = 1;
    }

    sxi_md_cleanup(&hash_ctx);
    return ret;
}

rc_ty sx_hashfs_tmp_getinfo(sx_hashfs_t *h, int64_t tmpfile_id, sx_hashfs_tmpinfo_t **tmpinfo, int recheck_presence, uint64_t op_expires_at) {
    unsigned int contentsz, nblocks, bs, nuniqs, i, hash_size, navl;
    const unsigned int *uniqs;
    const sx_hashfs_volume_t *volume;
    rc_ty ret = FAIL_EINTERNAL, ret2;
    const sx_hash_t *content;
    sx_hashfs_tmpinfo_t *tbd = NULL;
    const char *name, *revision;
    const uint8_t *avl;
    int64_t file_size;
#ifdef FILEHASH_OPTIMIZATION
    sx_hash_t filehash;
#endif
    int r;
    char token[TOKEN_RAND_BYTES*2 + 1];

    if(!h || !tmpinfo) {
	NULLARG();
	return EFAULT;
    }
    DEBUG("tmp_getinfo for file %ld", tmpfile_id);

    /* Get tmp data */
    sqlite3_reset(h->qt_tmpdata);
    if(qbind_int64(h->qt_tmpdata, ":id", tmpfile_id))
	goto getmissing_err;

    r = qstep(h->qt_tmpdata);
    if(r == SQLITE_DONE) {
	msg_set_reason("Token not found");
	ret = ENOENT;
    }
    if(r != SQLITE_ROW) {
	WARN("Error looking up token");
	goto getmissing_err;
    }

    if(sqlite3_column_int(h->qt_tmpdata, 6) == 0) {
	/* Not yet flushed, need to retry later */
	msg_set_reason("Token not ready yet");
	ret = EAGAIN;
	goto getmissing_err;
    }

    /* Quickly validate tmp data */
    revision = (const char *)sqlite3_column_text(h->qt_tmpdata, 0);
    if(!revision || strlen(revision) >= sizeof(tbd->revision)) {
	WARN("Tmpfile with %s revision", revision ? "bad" : "NULL");
	msg_set_reason("Internal corruption detected (bad revision)");
	ret = EFAULT;
	goto getmissing_err;
    }

    if((ret2 = sx_hashfs_volume_by_id(h, sqlite3_column_int64(h->qt_tmpdata, 3), &volume))) {
	ret = ret2;
	if(ret2 == ENOENT)
	    msg_set_reason("Volume no longer exists");
	goto getmissing_err;
    }

    name = (const char *)sqlite3_column_text(h->qt_tmpdata, 1);
    if(check_file_name(name) < 0) {
	WARN("Tmpfile with bad name");
	msg_set_reason("Internal corruption detected (bad name)");
	ret = EFAULT;
	goto getmissing_err;
    }

    file_size = sqlite3_column_int64(h->qt_tmpdata, 2);
    nblocks = size_to_blocks(file_size, &hash_size, &bs);

    content = sqlite3_column_blob(h->qt_tmpdata, 4);
    contentsz = sqlite3_column_bytes(h->qt_tmpdata, 4);
    if(contentsz % sizeof(sx_hash_t) || contentsz / sizeof(*content) != nblocks) {
	WARN("Tmpfile with bad content length");
	msg_set_reason("Internal corruption detected (bad content)");
	ret = EFAULT;
	goto getmissing_err;
    }
#ifdef FILEHASH_OPTIMIZATION
    if (hash_buf("", 0, content, contentsz, &filehash)) {
        WARN("Cannot calculate file hash");
        goto getmissing_err;
    }
#endif

    uniqs = sqlite3_column_blob(h->qt_tmpdata, 5);
    contentsz = sqlite3_column_bytes(h->qt_tmpdata, 5);
    nuniqs = contentsz / sizeof(*uniqs);
    if(contentsz % sizeof(*uniqs) || nuniqs > nblocks)  {
	WARN("Tmpfile with bad unique length");
	msg_set_reason("Internal corruption detected (bad unique content)");
	ret = EFAULT;
	goto getmissing_err;
    }

#ifdef FILEHASH_OPTIMIZATION
    if (commit) {
        if (filehash_mod_used(h, &filehash, 1) == ITER_NO_MORE) {
            nuniqs = 0; /* do not bump/check any hash counters */
            DEBUG("skipping moduse (commit)");
        }
    } else {
        if (filehash_is_used(h, &filehash)) {
            nuniqs = 0;
            DEBUG("skipping moduse (request)");
        }
    }
#endif

    avl = sqlite3_column_blob(h->qt_tmpdata, 7);
    if(avl) {
	navl = sqlite3_column_bytes(h->qt_tmpdata, 7);
	if(navl != nblocks * volume->replica_count) {
	    WARN("Tmpfile with bad availability length");
	    msg_set_reason("Internal corruption detected (bad availability content)");
	    ret = EFAULT;
	    goto getmissing_err;
	}
    } else
	navl = nblocks * volume->replica_count;

    tbd = wrap_malloc(sizeof(*tbd) + /* The struct itself */
		      nblocks * sizeof(sx_hash_t) + /* all_blocks */
		      nuniqs * sizeof(tbd->uniq_ids[0]) + /* uniq_ids */
		      nblocks * sizeof(tbd->nidxs[0]) * volume->replica_count + /* nidxs */
		      navl); /* avlblty */
    if(!tbd) {
	OOM();
	ret = ENOMEM;
	goto getmissing_err;
    }

    tbd->allnodes = sx_hashfs_nodelist(h, NL_NEXT);

    tbd->volume_id = volume->id;
    tbd->all_blocks = (sx_hash_t *)(tbd+1);
    tbd->uniq_ids = (unsigned int *)&tbd->all_blocks[nblocks];
    tbd->nidxs = &tbd->uniq_ids[nuniqs];
    tbd->avlblty = (uint8_t *)&tbd->nidxs[nblocks * volume->replica_count];
    memcpy(tbd->all_blocks, content, nblocks * sizeof(sx_hash_t));
    memcpy(tbd->uniq_ids, uniqs, nuniqs * sizeof(tbd->uniq_ids[0]));
    memset(tbd->nidxs, -1, nblocks * sizeof(tbd->nidxs[0]) * volume->replica_count);
    for(i=0; i<nuniqs; i++) {
	/* MODHDIST: pick from _next, bidx=0 */
	if(hash_nidx_tobuf(h, &tbd->all_blocks[tbd->uniq_ids[i]], volume->replica_count, &tbd->nidxs[tbd->uniq_ids[i]*volume->replica_count])) {
	    WARN("hash_nidx_tobuf failed");
	    goto getmissing_err;
	}
    }
    if(!avl)
	memset(tbd->avlblty, 0, navl);
    else
	memcpy(tbd->avlblty, avl, navl);
    tbd->nall = nblocks;
    tbd->nuniq = nuniqs;
    tbd->replica_count = volume->replica_count;
    tbd->block_size = bs;
    strcpy(tbd->revision, revision);
    strcpy(tbd->name, name);
    strcpy(tbd->revision, revision);
    tbd->file_size = file_size;
    tbd->tmpfile_id = tmpfile_id;
    tbd->somestatechanged = 0;

    strncpy(token, (const char*)sqlite3_column_text(h->qt_tmpdata, 8), sizeof(token)-1);
    token[sizeof(token)-1] = '\0';
    sqlite3_reset(h->qt_tmpdata); /* Do not deadlock if we need to update this very entry */

    if(nuniqs && recheck_presence) {
	unsigned int r, l;

	/* For each replica set populate tbd->avlblty via hash_presence callback */
	for(i=1; i<=tbd->replica_count; i++) {
            sx_hash_t tmpid, reserveid;
	    unsigned int cur_item = 0;
	    sort_by_node_then_hash(tbd->all_blocks, tbd->uniq_ids, tbd->nidxs, tbd->nuniq, i, tbd->replica_count);
            /* tmpid must match the ID used for reserving hashes in gettoken */
            if (unique_tmpid(h, token, &tmpid))
                goto getmissing_err;
            if (reserve_fileid(h, tbd->volume_id, tbd->name, &reserveid))
                goto getmissing_err;
            DEBUGHASH("tmp_get_info reserveid", &reserveid);
            DEBUGHASH("tmp_get_info tmpid", &tmpid);
            sxi_hashop_begin(&h->hc, h->sx_clust, tmp_getmissing_cb,
                             HASHOP_INUSE, tbd->replica_count, &reserveid, &tmpid, tbd, op_expires_at);
	    tbd->current_replica = i;
            DEBUG("begin queries for replica #%d", i);
	    while((ret2 = are_blocks_available(h,
					       tbd->all_blocks,
					       &h->hc,
					       tbd->uniq_ids,
					       tbd->nidxs,
					       &cur_item,
					       tbd->nuniq,
					       hash_size,
					       i,
					       tbd->replica_count)) == OK) {
		if(cur_item >= nuniqs)
		    break;
	    }
	    if(ret2 != OK) {
		ret = ret2;
		goto getmissing_err;
	    }
	    if(sxi_hashop_end(&h->hc) == -1) {
                ret = EAGAIN;
		goto getmissing_err;
            }
            DEBUG("end queries for replica #%d", i);
	}

	/* Drop all hashes which are already fully replicated */
	for(r=0, l=0; r < tbd->nuniq; r++) {
	    for(i=0; i<tbd->replica_count; i++) {
		if(tbd->avlblty[tbd->uniq_ids[r] * tbd->replica_count + i] != 1)
		    break;
	    }
	    if(i == tbd->replica_count) {
		tbd->somestatechanged = 1; /* Should be implied but explicitly setting this won't harm */
		nuniqs--;
		continue;
	    }
	    if(l != r)
		tbd->uniq_ids[l] = tbd->uniq_ids[r];
	    l++;
	}

	if(tbd->somestatechanged) {
	    /* If we've harvested some hash bring the counter down */
	    tbd->nuniq = nuniqs;

	    /* and update the db so they won't be hashop'd again on the next run */
	    sqlite3_reset(h->qt_updateuniq);
	    if(!qbind_int64(h->qt_updateuniq, ":id", tmpfile_id) &&
	       !qbind_blob(h->qt_updateuniq, ":uniq", tbd->uniq_ids, sizeof(*tbd->uniq_ids) * tbd->nuniq) &&
	       !qbind_blob(h->qt_updateuniq, ":avail", tbd->avlblty, navl))
		qstep_noret(h->qt_updateuniq);
	    sqlite3_reset(h->qt_updateuniq);
	}
    }

    *tmpinfo = tbd;
    ret = OK;

 getmissing_err:
    if(ret != OK) {
        (void)sxi_hashop_end(&h->hc);
	free(tbd);
    }

    sqlite3_reset(h->qt_tmpdata);

    return ret;
}


rc_ty sx_hashfs_tmp_tofile(sx_hashfs_t *h, const sx_hashfs_tmpinfo_t *missing) {
    rc_ty ret = FAIL_EINTERNAL, ret2;
    const sx_hashfs_volume_t *volume;
    int64_t file_id;
    int r, mdb;

    if(!h || !missing) {
	NULLARG();
	return EFAULT;
    }

    mdb = getmetadb(missing->name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    ret2 = sx_hashfs_volume_by_id(h, missing->volume_id, &volume);
    if(ret2) {
	WARN("Cannot locate volume %lld", (long long)missing->volume_id);
	return ret2;
    }

    sqlite3_reset(h->qt_getmeta);

    if(qbegin(h->metadb[mdb]))
	return FAIL_EINTERNAL;

    ret2 = create_file(h, volume, missing->name, missing->revision, missing->all_blocks, missing->nall, missing->file_size, &file_id);
    if(ret2) {
	ret = ret2;
	goto tmp2file_rollback;
    }

    if(qbind_int64(h->qt_getmeta, ":id", missing->tmpfile_id))
	goto tmp2file_rollback;
    while((r = qstep(h->qt_getmeta)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(h->qt_getmeta, 0);
	const void *value = sqlite3_column_blob(h->qt_getmeta, 1);
	int value_len = sqlite3_column_bytes(h->qt_getmeta, 1);

	sqlite3_reset(h->qm_metaset[mdb]);
	if(qbind_int64(h->qm_metaset[mdb], ":file", file_id) ||
	   qbind_text(h->qm_metaset[mdb], ":key", key) ||
	   qbind_blob(h->qm_metaset[mdb], ":value", value, value_len) ||
	   qstep_noret(h->qm_metaset[mdb])) {
	    sqlite3_reset(h->qm_metaset[mdb]);
	    goto tmp2file_rollback;
	}
    }
    sqlite3_reset(h->qm_metaset[mdb]);
    sqlite3_reset(h->qt_getmeta);
    if(r != SQLITE_DONE)
	goto tmp2file_rollback;

    if(qcommit(h->metadb[mdb]))
	goto tmp2file_rollback;

    sqlite3_reset(h->qt_delete);
    if(!qbind_int64(h->qt_delete, ":id", missing->tmpfile_id))
	qstep_noret(h->qt_delete);
    sqlite3_reset(h->qt_delete);

    ret = OK;

 tmp2file_rollback:
    if(ret != OK)
	qrollback(h->metadb[mdb]);

    sqlite3_reset(h->qm_metaset[mdb]);
    sqlite3_reset(h->qt_getmeta);

    return ret;
}


rc_ty sx_hashfs_tmp_getmeta(sx_hashfs_t *h, const char *name, int64_t tmpfile_id, sxc_meta_t *metadata) {
    rc_ty ret = FAIL_EINTERNAL;
    int r, mdb;

    if(!h || !metadata) {
	NULLARG();
	return EFAULT;
    }

    mdb = getmetadb(name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qt_getmeta);
    if(qbind_int64(h->qt_getmeta, ":id", tmpfile_id))
	return FAIL_EINTERNAL;
    while((r = qstep(h->qt_getmeta)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(h->qt_getmeta, 0);
	const void *value = sqlite3_column_blob(h->qt_getmeta, 1);
	int value_len = sqlite3_column_bytes(h->qt_getmeta, 1);

	if(sxc_meta_setval(metadata, key, value, value_len)) {
	    msg_set_reason("Not enough memory to collect file metadata");
	    ret = ENOMEM;
	    goto tmpgetmeta_err;
	}
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Database error collecting file metadata");
	goto tmpgetmeta_err;
    }

    ret = OK;

 tmpgetmeta_err:
    sqlite3_reset(h->qm_metaset[mdb]);

    return ret;
}

static rc_ty get_file_id(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, int64_t *file_id, int *database_number, unsigned int *created_at, sx_hash_t *etag, int64_t *size) {
    const sx_hashfs_volume_t *vol;
    sqlite3_stmt *q;
    int r, ndb;
    rc_ty res;

    if(!h || !volume || !filename || !file_id) {
	NULLARG();
	return EFAULT;
    }

    if(check_file_name(filename)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    res = sx_hashfs_volume_by_name(h, volume, &vol);
    if(res)
	return res;

    ndb = getmetadb(filename);
    if(ndb < 0)
	return FAIL_EINTERNAL;

    if(revision) {
	if(check_revision(revision)) {
	    msg_set_reason("Invalid revision");
	    return EINVAL;
	}
	q = h->qm_getrev[ndb];
	if(qbind_text(q, ":revision", revision))
	    return FAIL_EINTERNAL;
    } else
	q = h->qm_get[ndb];

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename))
	return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_ROW) {
	*file_id = sqlite3_column_int64(q, 0);
	*database_number = ndb;
	res = OK;
	if(created_at || etag) {
	    const char *rev = (const char *)sqlite3_column_text(q, 3);
	    if(!rev ||
	       (created_at && parse_revision(rev, created_at)) ||
	       (etag && hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), rev, strlen(rev), etag)))
		res = FAIL_EINTERNAL;
	}
        if(size)
            *size = sqlite3_column_int64(q, 4);
    } else if(r == SQLITE_DONE)
	res = ENOENT;
    else
	res = FAIL_EINTERNAL;

    sqlite3_reset(q);
    return res;
}

static rc_ty file_unbump(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *file, const char *revision, sx_hashfs_nl_t dist, int current_replica) {
    unsigned int i, idx = 0, *idxs = NULL, uniq;
    char fileidhex[SXI_SHA1_TEXT_LEN+1];
    sx_hashfs_file_t filedata;
    uint64_t op_expires_at;
    sx_hash_t hash;
    rc_ty ret;

    if (unique_fileid(h->sx, volume, file, revision, &hash) ||
	bin2hex(hash.b, sizeof(hash.b), fileidhex, sizeof(fileidhex)))
	return FAIL_EINTERNAL;
    ret = sx_hashfs_getfile_begin(h, volume->name, file, revision, &filedata, NULL);
    if (ret != OK)
	return ret;
    op_expires_at = time(NULL) + sx_hashfs_job_file_timeout(h, current_replica, filedata.file_size);
    sxi_hashop_begin(&h->hc, h->sx_clust, NULL, HASHOP_DELETE, volume->replica_count, NULL, &hash, NULL, op_expires_at);

    if (h->get_nblocks) do {
	    idxs = wrap_malloc(h->get_nblocks * sizeof(*idxs));
	    if (!idxs) {
		ret = ENOMEM;
		break;
	    }

	    const sx_node_t *self = sx_hashfs_self(h);

	    uniq = h->get_nblocks;
	    build_uniq_hash_index(h->get_content, idxs, &uniq);
	    if (uniq > 0) {
		sx_nodelist_t **nodes = wrap_calloc(uniq, sizeof(*nodes));
		if (!nodes) {
		    ret = ENOMEM;
		    break;
		}

		for (i=0; i<uniq; i++) {
		    const sx_hash_t *hash = &h->get_content[idxs[i]];
		    nodes[i] = sx_hashfs_hashnodes(h, NL_NEXT, hash, h->get_replica);
		    const sx_node_t *node = sx_nodelist_get(nodes[i], current_replica);
		    DEBUGHASH("decuse on", hash);
		    if (!sx_node_cmp(node, self)) {
			ret = sx_hashfs_hashop_begin(h, filedata.block_size);
			if (ret == OK) {
			    ret = sx_hashfs_hashop_perform(h, volume->replica_count, HASHOP_DELETE, hash, fileidhex, op_expires_at);
			    if (ret != OK)
				WARN("hashop_perform failed: %s", rc2str(ret));
			    ret = sx_hashfs_hashop_finish(h, ret);
			} else
			    WARN("hashop_begin failed: %s", rc2str(ret));
		    } else {
			ret = sxi_hashop_batch_add(&h->hc, sx_node_internal_addr(node), idx++, hash->b, filedata.block_size);
			if (ret)
			    WARN("hashop_batch_add failed: %s", rc2str(ret));
		    }
		    if (ret)
			WARN("Failed to query hash...: %s", rc2str(ret));
		}
		sx_hashfs_getfile_end(h);
		DEBUG("waiting for hashop_end");
		if (sxi_hashop_end(&h->hc) == -1) {
		    WARN("hashop_end failed: %s", sxc_geterrmsg(h->sx));
		    ret = sxc_geterrnum(h->sx);
		}
		for (i=0; i<uniq;i++)
		    sx_nodelist_delete(nodes[i]);
		free(nodes);
		nodes = NULL;
		free(idxs);
		idxs = NULL;
	    }
	} while(0);
    sx_hashfs_getfile_end(h);
    free(idxs);
    if (ret != OK && ret != ITER_NO_MORE)
	return ret;

    return OK;
}

rc_ty sx_hashfs_file_delete(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *file, const char *revision) {
    sx_nodelist_t *belongsto;
    int ndb, prev_replica, next_replica;
    int64_t file_id, mh;
    sx_hash_t hash;
    int64_t size = 0;
    rc_ty ret;
    int deleted;

    if(!h || !volume || !file) {
	NULLARG();
	return EFAULT;
    }

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    if(check_file_name(file)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid revision");
	return EINVAL;
    }

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), volume->name, strlen(volume->name), &hash)) {
        WARN("Cannot calculate volume hash");
        return FAIL_EINTERNAL;
    }
    mh = MurmurHash64(&hash, sizeof(hash), HDIST_SEED);

    belongsto = sxi_hdist_locate(h->hd, mh, volume->replica_count, 0);
    if(!belongsto) {
        WARN("Cannot get nodes for volume %s", volume->name);
        return FAIL_EINTERNAL;
    }
    if(!sx_nodelist_lookup_index(belongsto, &h->node_uuid, (unsigned int *)&next_replica))
	next_replica = -1;
    sx_nodelist_delete(belongsto);

    if(h->is_rebalancing) {
	belongsto = sxi_hdist_locate(h->hd, mh, volume->replica_count, 1);
	if(!belongsto) {
	    WARN("Cannot get nodes for volume %s", volume->name);
	    return FAIL_EINTERNAL;
	}
	if(!sx_nodelist_lookup_index(belongsto, &h->node_uuid, (unsigned int *)&prev_replica))
	    prev_replica = -1;
	sx_nodelist_delete(belongsto);
    } else
	prev_replica = -1;

    if(next_replica < 0 && prev_replica < 0) {
	msg_set_reason("Wrong node for volume '%s': ...", volume->name);
	return ENOENT;
    }

    ret = get_file_id(h, volume->name, file, revision, &file_id, &ndb, NULL, NULL, &size);
    if(ret)
        return ret;

    /* MODHDIST: decrement hash refcounts from all distibutions */
    if(next_replica >= 0)
	ret = file_unbump(h, volume, file, revision, NL_NEXT, next_replica);
    if(ret == OK && prev_replica >= 0)
	ret = file_unbump(h, volume, file, revision, NL_PREV, prev_replica);

    if(ret != OK) {
	msg_set_reason("File delete failed: cannot update block counters for %s/%s", volume->name, file);
	return ret;
    }

    sqlite3_reset(h->qm_delfile[ndb]);
    if(qbind_int64(h->qm_delfile[ndb], ":file", file_id) ||
       qstep_noret(h->qm_delfile[ndb]))
	ret = FAIL_EINTERNAL;
    else
	ret = OK;
    deleted = sqlite3_changes(h->metadb[ndb]->handle);
    sqlite3_reset(h->qm_delfile[ndb]);

    if(ret == OK && size && deleted && update_volume_current_size(h, volume->id, -size)) {
        WARN("Failed to update volume size");
        return FAIL_EINTERNAL;
    }

    return ret;

}

static rc_ty fill_filemeta(sx_hashfs_t *h, unsigned int metadb, int64_t file_id) {
    sqlite3_stmt *q = h->qm_metaget[metadb];
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    sqlite3_reset(q);
    if(qbind_int64(q, ":file", file_id))
	return FAIL_EINTERNAL;

    h->nmeta = 0;
    while((r = qstep(q)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(q, 0);
	const void *value = sqlite3_column_text(q, 1);
	int value_len = sqlite3_column_bytes(q, 1), key_len;
	if(!key)
	    break;
	key_len = strlen(key);
	if(key_len >= sizeof(h->meta[0].key))
	    break;
	if(!value || value_len > sizeof(h->meta[0].value))
	    break;
	memcpy(h->meta[h->nmeta].key, key, key_len+1);
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
	h->nmeta++;
	if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	    break;
    }

    if(r == SQLITE_DONE)
	ret = OK;

    sqlite3_reset(q);

    return ret;
}


rc_ty sx_hashfs_getfilemeta_begin(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, unsigned int *created_at, sx_hash_t *etag) {
    rc_ty ret;
    int metaget_ndb;
    int64_t file_id;

    if(!h)
	return EINVAL;

    ret = get_file_id(h, volume, filename, revision, &file_id, &metaget_ndb, created_at, etag, NULL);
    if(ret)
	return ret;

    return fill_filemeta(h, metaget_ndb, file_id);
}

rc_ty sx_hashfs_getfilemeta_next(sx_hashfs_t *h, const char **key, const void **value, unsigned int *value_len) {
    if(!h || !key || (value && !value_len)) {
	NULLARG();
	return EFAULT;
    }

    if(!h->nmeta || h->nmeta > SXLIMIT_META_MAX_ITEMS)
	return ITER_NO_MORE;

    h->nmeta--;
    *key = h->meta[h->nmeta].key;
    if(value) {
	*value = h->meta[h->nmeta].value;
	*value_len = h->meta[h->nmeta].value_len;
    }

    return OK;
}

rc_ty sx_hashfs_volumemeta_begin(sx_hashfs_t *h, const sx_hashfs_volume_t *volume) {
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h || !volume) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->q_metaget);
    if(qbind_int64(h->q_metaget, ":volume", volume->id)) {
	sqlite3_reset(h->q_metaget);
	return FAIL_EINTERNAL;
    }

    h->nmeta = 0;
    while((r = qstep(h->q_metaget)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(h->q_metaget, 0);
	const void *value = sqlite3_column_text(h->q_metaget, 1);
	int value_len = sqlite3_column_bytes(h->q_metaget, 1), key_len;
	if(!key || !value) {
	    OOM();
	    goto getvolumemeta_begin_err;
	}
	key_len = strlen(key);
	if(key_len >= sizeof(h->meta[0].key)) {
	    msg_set_reason("Key '%s' is too long: must be <%ld", key, sizeof(h->meta[0].key));
	    goto getvolumemeta_begin_err;
	}
	if(value_len > sizeof(h->meta[0].value)) {
	    /* Do not log the value, might contain sensitive data */
	    msg_set_reason("Value is too long: %d >= %ld", value_len, sizeof(h->meta[0].key));
	    goto getvolumemeta_begin_err;
	}
	memcpy(h->meta[h->nmeta].key, key, key_len+1);
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
	h->nmeta++;
	if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	    break;
    }

    if(r != SQLITE_DONE)
	goto getvolumemeta_begin_err;

    ret = OK;

 getvolumemeta_begin_err:
    sqlite3_reset(h->q_metaget);

    return ret;
}

rc_ty sx_hashfs_volumemeta_next(sx_hashfs_t *h, const char **key, const void **value, unsigned int *value_len) {
    return sx_hashfs_getfilemeta_next(h, key, value, value_len);
}

rc_ty sx_hashfs_get_user_info(sx_hashfs_t *h, const uint8_t *user, sx_uid_t *uid, uint8_t *key, sx_priv_t *basepriv) {
    const uint8_t *kcol;
    rc_ty ret = FAIL_EINTERNAL;
    sx_priv_t userpriv;
    int r;

    if(!h || !user)
	return EINVAL;

    sqlite3_reset(h->q_getuser);
    if(qbind_blob(h->q_getuser, ":user", user, AUTH_UID_LEN))
	goto get_user_info_err;

    r = qstep(h->q_getuser);
    if(r == SQLITE_DONE) {
	ret = ENOENT;
	goto get_user_info_err;
    }
    if(r != SQLITE_ROW)
	goto get_user_info_err;

    switch(sqlite3_column_int(h->q_getuser, 2)) {
    case ROLE_CLUSTER:
	userpriv = PRIV_CLUSTER;
	break;
    case ROLE_ADMIN:
	userpriv = PRIV_ADMIN;
	break;
    case ROLE_USER:
	userpriv = PRIV_NONE;
	break;
    default:
	WARN("Found invalid role");
	goto get_user_info_err;
    }
    if(basepriv)
	*basepriv = userpriv;

    kcol = (const uint8_t *)sqlite3_column_blob(h->q_getuser, 1);
    if(!kcol || sqlite3_column_bytes(h->q_getuser, 1) != AUTH_KEY_LEN) {
	WARN("Found bad key");
	goto get_user_info_err;
    }
    if(key)
	memcpy(key, kcol, AUTH_KEY_LEN);
    if(uid)
	*uid = sqlite3_column_int64(h->q_getuser, 0);
    ret = OK;

get_user_info_err:
    sqlite3_reset(h->q_getuser);
    return ret;
}


static rc_ty get_user_common(sx_hashfs_t *h, sx_uid_t uid, const char *name, uint8_t *user) {
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q;
    int r;

    if(!h || !uid) {
	NULLARG();
	return EFAULT;
    }

    if(!name) {
	q = h->q_getuserbyid;
	sqlite3_reset(q);
	if(qbind_int64(q, ":uid", uid)) 
	    goto get_user_common_fail;
    } else {
	if(sx_hashfs_check_username(name)) {
	    msg_set_reason("Invalid username");
	    return EINVAL;
	}
	q = h->q_getuserbyname;
	sqlite3_reset(q);
	if(qbind_text(q, ":name", name)) 
	    goto get_user_common_fail;
    }

    r = qstep(q);
    if(r == SQLITE_ROW) {
	const void *sqlusr = sqlite3_column_blob(q, 0);
	if(sqlusr && sqlite3_column_bytes(q, 0) == AUTH_UID_LEN) {
	    if(user)
		memcpy(user, sqlusr, AUTH_UID_LEN);
	    ret = OK;
	}
    } else if(r == SQLITE_DONE)
	ret = ENOENT;

 get_user_common_fail:
    sqlite3_reset(q);
    return ret;
}


rc_ty sx_hashfs_get_user_by_uid(sx_hashfs_t *h, sx_uid_t uid, uint8_t *user) {
    return get_user_common(h, uid, NULL, user);
}

rc_ty sx_hashfs_get_user_by_name(sx_hashfs_t *h, const char *name, uint8_t *user) {
    return get_user_common(h, -1, name, user);
}

rc_ty sx_hashfs_get_access(sx_hashfs_t *h, sx_uid_t uid, const char *volume, sx_priv_t *access) {
    const sx_hashfs_volume_t *vol;
    rc_ty ret;
    int r;

    if(!h || !volume || !access)
	return EINVAL;

    ret = sx_hashfs_volume_by_name(h, volume, &vol);
    if(ret)
	return ret;

    sqlite3_reset(h->q_getaccess);
    if(qbind_int64(h->q_getaccess, ":volume", vol->id) ||
       qbind_int64(h->q_getaccess, ":user", uid))
	return FAIL_EINTERNAL;

    r = qstep(h->q_getaccess);
    if(r == SQLITE_DONE) {
	*access = PRIV_NONE;
	return OK;
    }
    if(r != SQLITE_ROW)
	return FAIL_EINTERNAL;

    r = sqlite3_column_int(h->q_getaccess, 0);
    if(!(r & ~(PRIV_READ | PRIV_WRITE))) {
	ret = OK;
	*access = r;
    } else
	WARN("Found invalid priv for user %lld on volume %lld: %d", (long long int)uid, (long long int)vol->id, r);
    if (sqlite3_column_int(h->q_getaccess, 1) == uid)
	*access |= PRIV_ACL;

    sqlite3_reset(h->q_getaccess);
    return ret;
}

sxi_db_t *sx_hashfs_eventdb(sx_hashfs_t *h) {
    return h->eventdb;
}

sxi_db_t *sx_hashfs_xferdb(sx_hashfs_t *h) {
    return h->xferdb;
}

sxc_client_t *sx_hashfs_client(sx_hashfs_t *h) {
    return h->sx;
}

sxi_conns_t *sx_hashfs_conns(sx_hashfs_t *h) {
    return h->sx_clust;
}


rc_ty sx_hashfs_job_result(sx_hashfs_t *h, job_t job, sx_uid_t uid, job_status_t *status, const char **message) {
    int r;

    if(!h || !status || !message) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->qe_getjob);

    if(qbind_int64(h->qe_getjob, ":id", job) ||
       qbind_int64(h->qe_getjob, ":owner", uid))
	return FAIL_EINTERNAL;

    r = qstep(h->qe_getjob);
    if(r == SQLITE_DONE)
	return ENOENT;

    if(r != SQLITE_ROW)
	return FAIL_EINTERNAL;

    if(!sqlite3_column_int(h->qe_getjob, 0)) {
	/* Pending job */
	*status = JOB_PENDING;
	*message = "Job status pending";
    } else {
	/* Completed */
	int result = sqlite3_column_int(h->qe_getjob, 1);
	if(result) {
	    /* Failed */
	    const char *reason = (const char *)sqlite3_column_text(h->qe_getjob, 2);
	    *status = JOB_ERROR;
	    if(!reason || !*reason)
		*message = "Unknown job failure";
	    else {
		strncpy(h->job_message, reason, sizeof(h->job_message));
		h->job_message[sizeof(h->job_message)-1] = '\0';
		*message = h->job_message;
	    }
	} else {
	    /* Succeded */
	    *status = JOB_OK;
	    *message = "Job completed successfully";
	}
    }

    sqlite3_reset(h->qe_getjob);
    return OK;
}


static const char *locknames[] = {
    "VOL", /* JOBTYPE_CREATE_VOLUME */
    "USER", /* JOBTYPE_CREATE_USER */
    "ACL",
    NULL, /* JOBTYPE_REPLICATE_BLOCKS */
    "TOKEN", /* JOBTYPE_FLUSH_FILE */
    "DELFILE", /* JOBTYPE_DELETE_FILE */
    "*", /* JOBTYPE_DISTRIBUTION */
    "STARTREBALANCE", /* JOBTYPE_STARTREBALANCE */
    "FINISHREBALANCE", /* JOBTYPE_FINISHREBALANCE */
    NULL, /* JOBTYPE_JLOCK */
    "REBALANCE_BLOCKS", /* JOBTYPE_REBALANCE_BLOCKS */
    "REBALANCE_FILES", /* JOBTYPE_REBALANCE_FILES */
    "REBALANCE_CLEANUP", /* JOBTYPE_REBALANCE_CLEANUP */
    "USER", /* JOBTYPE_DELETE_USER */
    "VOL", /* JOBTYPE_DELETE_VOLUME */
};


#define MAX_PENDING_JOBS 128
rc_ty sx_hashfs_countjobs(sx_hashfs_t *h, sx_uid_t user_id) {
    rc_ty ret = FAIL_EINTERNAL;

    sqlite3_reset(h->qe_countjobs);
    if(qbind_int64(h->qe_countjobs, ":uid", user_id) ||
       qstep_ret(h->qe_countjobs)) 
	goto countjobs_out;
    if(sqlite3_column_int64(h->qe_countjobs, 0) > MAX_PENDING_JOBS) {
	ret = FAIL_ETOOMANY;
        DEBUG("too many jobs");
	goto countjobs_out;
    }
    ret = OK;

 countjobs_out:
    sqlite3_reset(h->qe_countjobs);
    return ret;
}

rc_ty sx_hashfs_job_new_begin(sx_hashfs_t *h) {
    int r;

    DEBUG("IN %s", __FUNCTION__);
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(h->addjob_begun) {
	msg_set_reason("Internal error: job_new_begin phase error");
	return FAIL_EINTERNAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_ROW) {
	const char *owner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	msg_set_reason("The requested action cannot be completed because a complex operation is being executed on the cluster (by node %s). Please try again later.", owner);
	sqlite3_reset(h->qe_islocked);
	qrollback(h->eventdb);
	return FAIL_LOCKED;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify job lock");
	qrollback(h->eventdb);
	return FAIL_EINTERNAL;
    }

    h->addjob_begun = 1;
    return OK;
}

rc_ty sx_hashfs_job_new_end(sx_hashfs_t *h) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(!h->addjob_begun) {
	msg_set_reason("Internal error: job_new_end phase error");
	return FAIL_EINTERNAL;
    }

    h->addjob_begun = 0;

    if(!qcommit(h->eventdb))
	return OK;

    msg_set_reason("Internal error: failed to commit new job(s) to database");
    qrollback(h->eventdb);

    return FAIL_EINTERNAL;
}

rc_ty sx_hashfs_job_new_notrigger(sx_hashfs_t *h, job_t parent, sx_uid_t user_id, job_t *job_id, jobtype_t type, unsigned int timeout_secs, const char *lock, const void *data, unsigned int datalen, const sx_nodelist_t *targets) {
    job_t id = JOB_FAILURE;
    char *lockstr = NULL;
    unsigned int i, ntargets;
    int r;
    rc_ty ret = FAIL_EINTERNAL, ret2;

    /* FIXME: add support for delayed jobs */
    if(!h || !job_id) {
	msg_set_reason("Internal error: NULL argument given");
        return FAIL_EINTERNAL;
    }

    if(!h->addjob_begun) {
	msg_set_reason("Internal error: job_new phase error");
	return FAIL_EINTERNAL;
    }

    if((datalen && !data) || !targets) {
	msg_set_reason("Internal error: NULL argument given");
	goto addjob_error;
    }

    ntargets = sx_nodelist_count(targets);
    if(!ntargets) {
	msg_set_reason("Internal error: request with no targets");
	goto addjob_error;
    }

    if(!data)
	data = "";

    if (type < 0 || type >= sizeof(locknames) / sizeof(locknames[0])) {
	msg_set_reason("Internal error: bad action type");
	goto addjob_error;
    }
    if(lock && locknames[type]) {
	if(!(lockstr = malloc(2 + strlen(locknames[type]) + strlen(lock) + 1))) {
	    msg_set_reason("Not enough memory to create job");
	    goto addjob_error;
	}
	sprintf(lockstr, "$%s$%s", locknames[type], lock);
    }

    ret2 = sx_hashfs_countjobs(h, user_id);
    if(ret2 != OK) {
	ret = ret2;
	goto addjob_error;
    }

    if(parent == JOB_NOPARENT) {
	if(qbind_null(h->qe_addjob, ":parent"))
	    goto addjob_error;
    } else {
	if(qbind_int64(h->qe_addjob, ":parent", parent))
	    goto addjob_error;
    }

    if(qbind_int(h->qe_addjob, ":type", type) ||
       qbind_int(h->qe_addjob, ":expiry", timeout_secs) ||
       qbind_blob(h->qe_addjob, ":data", data, datalen)) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }
    if(user_id == 0) {
	if(qbind_null(h->qe_addjob, ":uid"))
	    goto addjob_error;
    } else {
	if(qbind_int64(h->qe_addjob, ":uid", user_id))
	    goto addjob_error;
    }

    if(lockstr)
	r = qbind_text(h->qe_addjob, ":lock", lockstr);
    else
	r = qbind_null(h->qe_addjob, ":lock");
    if(r) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }

    r = qstep(h->qe_addjob);
    if(r == SQLITE_CONSTRAINT) {
	msg_set_reason("Resource is temporarily locked");
	ret = FAIL_LOCKED;
	goto addjob_error;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }

    id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qe_addjob));

    if(qbind_int64(h->qe_addact, ":job", id)) {
	msg_set_reason("Internal error: failed to add job action to database");
	goto addjob_error;
    }
    for(i=0; i<ntargets; i++) {
	const sx_node_t *node = sx_nodelist_get(targets, i);
	const sx_uuid_t *uuid = sx_node_uuid(node);
	if(qbind_blob(h->qe_addact, ":node", uuid->binary, sizeof(uuid->binary)) ||
	   qbind_text(h->qe_addact, ":addr", sx_node_addr(node)) ||
	   qbind_text(h->qe_addact, ":int_addr", sx_node_internal_addr(node)) ||
	   qbind_int64(h->qe_addact, ":capa", sx_node_capacity(node)) ||
	   qstep_noret(h->qe_addact)) {
	    msg_set_reason("Internal error: failed to add job action to database");
	    goto addjob_error;
	}
    }

    ret = OK;

 addjob_error:
    if(ret != OK) {
	qrollback(h->eventdb);
	h->addjob_begun = 0;
	id = JOB_FAILURE;
    }

    free(lockstr);
    sqlite3_reset(h->qe_addjob);
    sqlite3_reset(h->qe_addact);

    *job_id = id;
    return ret;
}

rc_ty sx_hashfs_job_new(sx_hashfs_t *h, sx_uid_t user_id, job_t *job_id, jobtype_t type, unsigned int timeout_secs, const char *lock, const void *data, unsigned int datalen, const sx_nodelist_t *targets) {
    rc_ty ret;

    if((ret = sx_hashfs_job_new_begin(h)) == OK && 
       (ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, user_id, job_id, type, timeout_secs, lock, data, datalen, targets)) == OK &&
       (ret = sx_hashfs_job_new_end(h)) == OK)
	sx_hashfs_job_trigger(h);

    return ret;
}

rc_ty sx_hashfs_job_lock(sx_hashfs_t *h, const char *owner) {
    rc_ty ret = FAIL_EINTERNAL;
    sx_uuid_t node;
    int r;

    if(!h || !owner) {
	NULLARG();
	return EFAULT;
    }

    if(uuid_from_string(&node, owner)) {
	msg_set_reason("Invalid lock owner");
	return EINVAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_ROW) {
	const char *curowner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	msg_set_reason("This node is already locked (by node %s). Please try again later.", curowner);
	sqlite3_reset(h->qe_islocked);
	ret = FAIL_LOCKED;
	goto job_lock_err;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify job lock");
	goto job_lock_err;
    }

    sqlite3_reset(h->qe_hasjobs);
    r = qstep(h->qe_hasjobs);
    if(r == SQLITE_ROW) {
	msg_set_reason("There are active jobs on this node and it currently cannot be locked. Please try again later.");
	sqlite3_reset(h->qe_hasjobs);
	ret = FAIL_LOCKED;
	goto job_lock_err;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify active job presence");
	goto job_lock_err;
    }

    sqlite3_reset(h->qe_lock);
    if(qbind_text(h->qe_lock, ":node", owner) ||
       qstep_noret(h->qe_lock) ||
       qcommit(h->eventdb)) {
	msg_set_reason("Internal error: failed to activate cluster locking");
	goto job_lock_err;
    }

    return OK;

 job_lock_err:
    qrollback(h->eventdb);
    return ret;
}

rc_ty sx_hashfs_job_unlock(sx_hashfs_t *h, const char *owner) {
    rc_ty ret = FAIL_EINTERNAL;
    const char *curowner;
    sx_uuid_t node;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(owner && uuid_from_string(&node, owner)) {
	msg_set_reason("Invalid lock owner");
	return EINVAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_DONE) {
	ret = OK;
	goto job_unlock_err;
    }

    if(r != SQLITE_ROW) {
	msg_set_reason("Internal error: failed to verify job lock");
	goto job_unlock_err;
    }

    if(owner) {
	curowner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	r = strcmp(owner, curowner);
	if(r) {
	    msg_set_reason("This node is locked by %s and cannot be unlocked by %s", curowner, owner);
	    sqlite3_reset(h->qe_islocked);
	    goto job_unlock_err;
	}
    }
    sqlite3_reset(h->qe_islocked);

    sqlite3_reset(h->qe_unlock);
    if(qstep_noret(h->qe_unlock) ||
       qcommit(h->eventdb)) {
	msg_set_reason("Internal error: failed to deactivate cluster locking");
	goto job_unlock_err;
    }

    return OK;

 job_unlock_err:
    qrollback(h->eventdb);
    return ret;
}

void sx_hashfs_job_trigger(sx_hashfs_t *h) {
    if(h && h->job_trigger >= 0) {
	int w = write(h->job_trigger, ".", 1);
	w = w;
    }
}

void sx_hashfs_xfer_trigger(sx_hashfs_t *h) {
    if(h && h->xfer_trigger >= 0) {
	int w = write(h->xfer_trigger, ".", 1);
	w = w;
    }
}

void sx_hashfs_gc_trigger(sx_hashfs_t *h) {
    if(h && h->gc_trigger >= 0) {
        INFO("triggered GC");
	int w = write(h->gc_trigger, ".", 1);
	w = w;
    }
}

rc_ty sx_hashfs_xfer_tonodes(sx_hashfs_t *h, sx_hash_t *block, unsigned int size, const sx_nodelist_t *targets) {
    const sx_node_t *self = sx_hashfs_self(h);
    unsigned int i, nnodes;
    rc_ty ret;

    if(!targets) {
	NULLARG();
	return EFAULT;
    }

    if(!self) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }
    if (sx_hashfs_block_get(h, size, block, NULL) != OK) {
        char hash[sizeof(sx_hash_t)*2+1];
        bin2hex(block->b, sizeof(block->b), hash, sizeof(hash));
        DEBUG("Asked to push a hash we don't have: #%s#", hash);
    }

    nnodes = sx_nodelist_count(targets);
    sqlite3_reset(h->qx_add);

    if(qbind_blob(h->qx_add, ":b", block, sizeof(*block))) {
	ret = FAIL_EINTERNAL;
	goto xfer_err;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *target = sx_nodelist_get(targets, i);
	const sx_uuid_t *target_uuid;
	int r;

	if(!sx_node_cmp(target, self))
	    continue;

	target_uuid = sx_node_uuid(target);
	if(qbind_int(h->qx_add, ":s", size) ||
	   qbind_blob(h->qx_add, ":n", target_uuid->binary, sizeof(target_uuid->binary))) {
	    break;
	}
	r = qstep(h->qx_add);
	if(r != SQLITE_DONE && r != SQLITE_CONSTRAINT)
	    break;

	sqlite3_reset(h->qx_add);
    }

    ret = (i == nnodes) ? OK : FAIL_EINTERNAL;
    DEBUG("xfer_to_nodes job added: %s", ret == OK ? "OK" : "Error");

 xfer_err:
    sqlite3_reset(h->qx_add);

    if(ret != OK)
	msg_set_reason("Internal error: failed to add block transfer request to database");

    return ret;
}

/* FIXMERB: unify with rbl_hold ? */
rc_ty sx_hashfs_xfer_tonode(sx_hashfs_t *h, sx_hash_t *block, unsigned int size, const sx_node_t *target) {
    sx_nodelist_t *targets = sx_nodelist_new();
    rc_ty ret;

    if(!targets)
	return ENOMEM;

    ret = sx_nodelist_add(targets, sx_node_dup(target));

    if(ret == OK)
	ret = sx_hashfs_xfer_tonodes(h, block, size, targets);

    sx_nodelist_delete(targets);

    return ret;
}

rc_ty sx_hashfs_gc_periodic(sx_hashfs_t *h, int *terminate, int grace_period)
{
    unsigned i, j, k;
    /* tokens expire when there was no upload activity within GC_GRACE_PERIOD
     * (i.e. ~2 days) */
    uint64_t real_now = time(NULL), now;
    uint64_t gc = 0, gcops = 0, expires = real_now - grace_period;
    int ret1 = 0, ret2 = 0;
    sqlite3_reset(h->qt_gc_tokens);
    if (grace_period < 0) {
        now = 1ll << 62;
        DEBUG("now set to: %lld", (long long)now);
    } else
        now = real_now;
    if (qbind_int64(h->qt_gc_tokens, ":now", now) ||
        qstep_noret(h->qt_gc_tokens))
        return FAIL_EINTERNAL;
    INFO("Deleted %d tokens", sqlite3_changes(h->tempdb->handle));
    for (j=0;j<SIZES && !ret1 && !ret2 && !*terminate;j++) {
        for (i=0;i<HASHDBS && !ret1 && !ret2 && !*terminate;i++) {
            if (qbind_blob(h->qb_find_expired_reservation[j][i], ":lastreserveid", "", 0)) {
                ret1 = -1;
                break;
            }
            /* TODO: eliminate code duplication and use a function */
            ret1 = SQLITE_ROW;
            do {
              if (qbegin(h->datadb[j][i])) {
                    ret1 = -1;
                    break;
              }
              for (k=0;k<gc_max_batch && ret1 == SQLITE_ROW && !*terminate;k++) {
                sx_hash_t last;
                sqlite3_stmt *q = h->qb_find_expired_reservation[j][i];
                sqlite3_stmt *q_gc = h->qb_gc_reserve[j][i];
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                if (qbind_int64(q, ":expires", expires)) {
                    ret1 = -1;
                    break;
                }
                DEBUG("find_expired, expires: %lld", (long long)expires);
                ret1 = qstep(q);
                if (ret1 == SQLITE_ROW) {
                    if (sqlite3_column_bytes(q, 0) != sizeof(last.b)) {
                        WARN("bad reserveid length");
                        ret1 = -1;
                        break;
                    }
                    memcpy(last.b, sqlite3_column_blob(q, 0), sizeof(last.b));
                    sqlite3_reset(q);
                    if (qbind_blob(q, ":lastreserveid", last.b, sizeof(last.b))) {
                        ret1 = -1;
                        break;
                    }
                    if (!qbind_blob(q_gc, ":reserveid", last.b, sizeof(last.b)) &&
                        !qstep_noret(q_gc)) {
                        gc++;
                    }
                }
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                if (ret1 == SQLITE_DONE) {
                    DEBUG("find_expired: done");
                    ret1 = 0;
                }
	     }
             if (qcommit(h->datadb[j][i])) {
                 ret1 = -1;
                 break;
             }
            } while (ret1 == SQLITE_ROW);
            if (ret1) {
               qrollback(h->datadb[j][i]);
               break;
            }
            if (qbind_int64(h->qb_find_expired_reservation2[j][i], ":lastrowid", 0)) {
                ret1 = -1;
                break;
            }
            do {
              if (qbegin(h->datadb[j][i])) {
                    ret1 = -1;
                    break;
              }
              for (k=0;k<gc_max_batch && ret1 == SQLITE_ROW && !*terminate;k++) {
                sqlite3_stmt *q = h->qb_find_expired_reservation2[j][i];
                sqlite3_stmt *q_gc = h->qb_gc_reserve2[j][i];
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                if (qbind_int64(q, ":now", now)) {
                    ret1 = -1;
                    break;
                }
                ret1 = qstep(q);
                if (ret1 == SQLITE_ROW) {
                    int64_t last_rowid = sqlite3_column_int64(q, 0);
                    sqlite3_reset(q);
                    if (qbind_int64(q, ":lastrowid", last_rowid)) {
                        ret1 = -1;
                        break;
                    }
                    if (!qbind_int64(q_gc, ":rowid", last_rowid) &&
                        !qstep_noret(q_gc)) {
                        gc++;
                    }
                }
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                if (ret1 == SQLITE_DONE)
                    ret1 = 0;
             }
             if (qcommit(h->datadb[j][i])) {
                 ret1 = -1;
                 break;
             }
	    } while (ret1 == SQLITE_ROW);
            if (ret1) {
               qrollback(h->datadb[j][i]);
               break;
            }
            ret2 = SQLITE_ROW;
	    do {
              if (qbegin(h->datadb[j][i])) {
                    ret1 = -1;
                    break;
              }
              for (k=0;k<gc_max_batch && ret2 == SQLITE_ROW && !*terminate;k++) {
		sqlite3_stmt *q = h->qb_find_expired_ops[j][i];
                sqlite3_stmt *q_gc = h->qb_gc_op[j][i];
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                if (qbind_int64(q, ":now", real_now)) {
                    ret2 = -1;
                    break;
                }
                ret2 = qstep(q);
                if (ret2 == SQLITE_ROW) {
                    if (!qbind_int64(q_gc, ":rowid", sqlite3_column_int64(q, 0)) &&
                        !qstep_noret(q_gc)) {
                        gcops++;
                    }
                }
                if (ret2 == SQLITE_DONE)
                    ret2 = 0;
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
              }
              if (qcommit(h->datadb[j][i])) {
                 ret2 = -1;
                 break;
              }
            } while (ret2 == SQLITE_ROW);
            if (ret2) {
               qrollback(h->datadb[j][i]);
               break;
            }
        }
    }
    INFO("GCed %lld reservations, %lld operations", (long long)gc, (long long)gcops);
    return (ret1 || ret2) ? FAIL_EINTERNAL : OK;
}

rc_ty sx_hashfs_gc_run(sx_hashfs_t *h, int *terminate)
{
    unsigned i, j, k;
    uint64_t gc = 0;
    int ret = 0;
    int64_t bad = 0;
    for (j=0;j<SIZES && !ret && !*terminate; j++) {
        for (i=0;i<HASHDBS && !ret && !*terminate;i++) {
            sqlite3_reset(h->qb_find_bad[j][i]);
            if (qstep_ret(h->qb_find_bad[j][i])) {
                WARN("Cannot query block counters");
                return FAIL_EINTERNAL;
            }
            bad += sqlite3_column_int64(h->qb_find_bad[j][i], 0);
            sqlite3_reset(h->qb_find_bad[j][i]);
        }
    }
    if (bad > 0) {
        if (sx_hashfs_is_rebalancing(h))
            /* can happen during rebalance without causing data loss */
            INFO("Not running garbage collection, %llu block counters are negative", (long long)bad);
        else
            CRIT("Refusing to run garbage collection to avoid data loss: %llu block counters are negative", (long long)bad);
        return FAIL_EINTERNAL;
    }

    for (j=0;j<SIZES && !ret && !*terminate ;j++) {
        for (i=0;i<HASHDBS && !ret && !*terminate;i++) {
            int64_t last = 0;
            sqlite3_stmt *q = h->qb_find_unused[j][i];
            sqlite3_stmt *q_gc = h->qb_gc1[j][i];
            sqlite3_stmt *q_setfree = h->qb_setfree[j][i];
            do {
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                sqlite3_reset(q_setfree);
                if (qbind_int64(q, ":last", last))
                    break;
                if (qbegin(h->datadb[j][i])) {
                    ret = -1;
                    break;
                }
                for (k=0;k<gc_max_batch && (ret = qstep(q)) == SQLITE_ROW && !*terminate; k++) {
                    int is_null = sqlite3_column_type(q, 1) == SQLITE_NULL;
                    last = sqlite3_column_int64(q, 0);
                    const sx_hash_t *hash = sqlite3_column_blob(q, 2);
                    if (hash && sqlite3_column_bytes(q, 2) == sizeof(*hash)) {
                        if (sx_hashfs_blkrb_can_gc(h, hash, bsz[j]) != OK) {
                            DEBUGHASH("Hash is locked by rebalance", hash);
                            continue;
                        }
                    }
                    if (hash)
                        DEBUGHASH("freeing block with hash", hash);
                    if (!is_null) {
                        int64_t blockno = sqlite3_column_int64(q, 1);
                        DEBUG("freeing blockno %ld, @%d/%d/%ld", blockno, j, i, blockno * bsz[j]);
                        if (qbind_int64(q_setfree, ":blockno", blockno) ||
                            qstep_noret(q_setfree)) {
                            ret = -1;
                            break;
                        }
                    }
                    if (qbind_int64(q_gc, ":blockid", last) ||
                        qstep_noret(q_gc)) {
                        ret = -1;
                        break;
                    }
                    gc++;
                }
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                sqlite3_reset(q_setfree);
                if (ret == SQLITE_DONE)
                    ret = 0;
                if (!ret || ret == SQLITE_ROW) {
                    if (qcommit(h->datadb[j][i])) {
                        ret = -1;
                        break;
                    }
                } else
                    qrollback(h->datadb[j][i]);
            } while (ret == SQLITE_ROW);
        }
    }
    INFO("GCed %lld hashes", (long long)gc);
    return ret ? FAIL_EINTERNAL : OK;
}

static rc_ty print_datadb_count(sx_hashfs_t *h, const char *table, int *terminate)
{
    rc_ty ret = OK;
    uint64_t count = 0;
    unsigned i,j;
    char query[128];
    sqlite3_stmt *q = NULL;
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s", table);
    for (j=0;j<SIZES && !*terminate;j++) {
        for (i=0;i<HASHDBS && !*terminate;i++) {
            qnullify(q);
            if (qprep(h->datadb[j][i], &q, query) || qstep_ret(q)) {
                WARN("print_count failed");
                ret = FAIL_EINTERNAL;
                break;
            }
            count += sqlite3_column_int64(q, 0);
            qnullify(q);
        }
    }
    qnullify(q);
    if (!terminate)
        return ret;
    INFO("Table %s has %lld entries", table, (long long)count);
    return ret;
}

rc_ty sx_hashfs_gc_info(sx_hashfs_t *h, int *terminate)
{
    rc_ty ret = OK;
    struct timeval tv0, tv1;
    gettimeofday(&tv0, NULL);
    if (print_datadb_count(h, "blocks", terminate) ||
        print_datadb_count(h, "reservations", terminate) ||
        print_datadb_count(h, "operations", terminate) ||
        print_datadb_count(h, "use", terminate) ||
        print_datadb_count(h, "avail", terminate))
        ret = FAIL_EINTERNAL;
    gettimeofday(&tv1, NULL);
    INFO("GC info completed in %.1fs", timediff(&tv0, &tv1));
    return ret;
}

rc_ty sx_hashfs_gc_expire_all_reservations(sx_hashfs_t *h)
{
    if(h && h->gc_trigger >= 0 && h->gc_expire_trigger >= 0) {
        INFO("triggered force expire");
        int w = write(h->gc_expire_trigger, ".", 1);
	w = write(h->gc_trigger, ".", 1);
	w = w;
    }
    return OK;
}

rc_ty sx_hashfs_hdist_change_req(sx_hashfs_t *h, const sx_nodelist_t *newdist, job_t *job_id) {
    sxi_hdist_t *newmod;
    unsigned int nnodes, minnodes, i, cfg_len;
    int64_t newclustersize = 0, minclustersize;
    sx_nodelist_t *targets;
    job_t finish_job;
    const void *cfg;
    rc_ty r;

    DEBUG("IN %s", __FUNCTION__);
    if(!h || !newdist || !job_id) {
	NULLARG();
	return EFAULT;
    }

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is still being rebalanced");
	return EINVAL;
    }

    r = get_min_reqs(h, &minnodes, &minclustersize);
    if(r) {
	msg_set_reason("Failed to compute cluster requirements");
	return r;
    }

    nnodes = sx_nodelist_count(newdist);
    if(nnodes < minnodes) {
	msg_set_reason("Invalid distribution: this cluster requires at least %u nodes to operate.", minnodes);
	return EINVAL;
    }

    if((r = sxi_hdist_get_cfg(h->hd, &cfg, &cfg_len)) != OK) {
	msg_set_reason("Failed to duplicate current distribution (get)");
	return r;
    }

    if(!(newmod = sxi_hdist_from_cfg(cfg, cfg_len))) {
	msg_set_reason("Failed to duplicate current distribution (from_cfg)");
	return EINVAL;
    }

    if((r = sxi_hdist_newbuild(newmod))) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to update current distribution");
	return r;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(newdist, i);
	unsigned int j;
	for(j=i+1; j<nnodes; j++) {
	    const sx_node_t *other = sx_nodelist_get(newdist, j);
	    if(!sx_node_cmp(n, other)) {
		sxi_hdist_free(newmod);
		msg_set_reason("Node %s cannot appear more than once", sx_node_uuid_str(n));
		return EINVAL;
	    }
	}
	newclustersize += sx_node_capacity(n);
	r = sxi_hdist_addnode(newmod, sx_node_uuid(n), sx_node_addr(n), sx_node_internal_addr(n), sx_node_capacity(n), NULL);
	if(r) {
	    sxi_hdist_free(newmod);
	    msg_set_reason("Failed to update current distribution");
	    return FAIL_EINTERNAL;
	}
    }

    if(newclustersize < minclustersize) {
	sxi_hdist_free(newmod);
	msg_set_reason("Invalid distribution: this cluster requires a total capacity of at least %lld bytes to operate.", (long long)minclustersize);
	return EINVAL;
    }

    if((r = sxi_hdist_build(newmod)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to build updated distribution");
	return r;
    }

    if((r = sxi_hdist_get_cfg(newmod, &cfg, &cfg_len)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to retrieve updated distribution");
	return r;
    }

    targets = sx_nodelist_new();
    if(!targets) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to setup job targets");
	return ENOMEM;
    }

    if((r = sx_nodelist_addlist(targets, sxi_hdist_nodelist(newmod, 1))) ||
       (r = sx_nodelist_addlist(targets, sxi_hdist_nodelist(newmod, 0)))) {
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to setup job targets");
	return r;
    }

    r = sx_hashfs_job_new_begin(h);
    if(r) {
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to setup job targets");
	return r;
    }

    /* FIXMERB: review TTL and ttl_extend */
    r = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, job_id, JOBTYPE_JLOCK, sx_nodelist_count(targets) * 20, "JLOCK", NULL, 0, targets);
    if(r) {
	INFO("job_new (jlock) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, job_id, JOBTYPE_DISTRIBUTION, sx_nodelist_count(targets) * 120, "DISTRIBUTION", cfg, cfg_len, targets);
    if(r) {
	INFO("job_new (distribution) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, job_id, JOBTYPE_STARTREBALANCE, sx_nodelist_count(targets) * 120, "DISTRIBUTION", NULL, 0, targets);
    if(r) {
	INFO("job_new (startrebalance) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, &finish_job, JOBTYPE_FINISHREBALANCE, JOB_NO_EXPIRY, "DISTRIBUTION", NULL, 0, targets);
    sx_nodelist_delete(targets);
    sxi_hdist_free(newmod);
    if(r) {
	INFO("job_new (finishrebalance) returned: %s", rc2str(r));
	return r;
    }

    r = sx_hashfs_job_new_end(h);
    if(r)
        INFO("Failed to commit jobadd");
    else
	sx_hashfs_job_trigger(h);

    return r;
}

rc_ty sx_hashfs_hdist_change_add(sx_hashfs_t *h, const void *cfg, unsigned int cfg_len) {
    int64_t newclustersize = 0, minclustersize;
    unsigned int nnodes, minnodes, i;
    sxi_hdist_t *newmod;
    const sx_nodelist_t *nodes;
    sqlite3_stmt *q = NULL;
    rc_ty ret;

    DEBUG("IN %s", __FUNCTION__);
    if(!h || !cfg) {
	NULLARG();
	return EINVAL;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is still being rebalanced");
	return EEXIST;
    }	

    newmod = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!newmod) {
	msg_set_reason("Failed to load the new distribution");
	return EINVAL;
    }

    if(sxi_hdist_buildcnt(newmod) != 2 ||
       (h->have_hd && (!sxi_hdist_same_origin(newmod, h->hd) || sxi_hdist_version(newmod) != sxi_hdist_version(h->hd) + 1))) {
	sxi_hdist_free(newmod);
	msg_set_reason("The new model is not a direct descendent of the current model");
	return EINVAL;
    }

    if(qbegin(h->db)) {
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = get_min_reqs(h, &minnodes, &minclustersize);
    if(ret) {
	msg_set_reason("Failed to compute cluster requirements");
	goto change_add_fail;
    }

    nodes = sxi_hdist_nodelist(newmod, 0);
    if(!nodes || !(nnodes = sx_nodelist_count(nodes))) {
	msg_set_reason("Failed to retrieve the list of the updated nodes");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(nnodes < minnodes) {
	msg_set_reason("Invalid distribution: this cluster requires at least %u nodes to operate.", minnodes);
	ret = EINVAL;
	goto change_add_fail;
    }

    for(i = 0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(nodes, i);
	unsigned int j;
	for(j=i+1; j<nnodes; j++) {
	    const sx_node_t *other = sx_nodelist_get(nodes, j);
	    if(!sx_node_cmp(n, other)) {
		msg_set_reason("Node %s cannot appear more than once", sx_node_uuid_str(n));
		ret = EINVAL;
		goto change_add_fail;
	    }
	}
	newclustersize += sx_node_capacity(n);
    }

    if(newclustersize < minclustersize) {
	msg_set_reason("Invalid distribution: this cluster requires a total capacity of at least %lld bytes to operate.", (long long)minclustersize);
	ret = EINVAL;
	goto change_add_fail;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)")) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(h->have_hd) {
	const void *cur_cfg;
	unsigned int cur_cfg_len;

	ret = sxi_hdist_get_cfg(h->hd, &cur_cfg, &cur_cfg_len);
	if(ret) {
	    msg_set_reason("Failed to retrieve the current distribution model");
	    goto change_add_fail;
	}
	if(qbind_text(q, ":k", "current_dist") ||
	   qbind_blob(q, ":v", cur_cfg, cur_cfg_len) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto change_add_fail;
	}

	sqlite3_reset(q);
	if(qbind_text(q, ":k", "current_dist_rev") ||
	   qbind_int64(q, ":v", sxi_hdist_version(h->hd)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto change_add_fail;
	}
	sqlite3_reset(q);
    }

    if(qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", cfg, cfg_len) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dist_rev") ||
       qbind_int64(q, ":v", sxi_hdist_version(newmod)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(sx_hashfs_set_rbl_info(h, 1, 0, "Updating distribution model")) {
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(qcommit(h->db)) {
	msg_set_reason("Failed to save distribution model");
	ret = FAIL_EINTERNAL;
    } else {
	ret = OK;
	DEBUG("Distribution change added from %lld to %lld", (long long)h->hd_rev, (long long)sxi_hdist_version(newmod));
    }
 change_add_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sxi_hdist_free(newmod);

    return ret;
}

rc_ty sx_hashfs_hdist_change_revoke(sx_hashfs_t *h) {
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    if(h->have_hd) {
	/* Revert to previous distribution */
	if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('dist_rev', 'dist')") || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);

	if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist' WHERE key = 'current_dist'") || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);

	if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist_rev' WHERE key = 'current_dist_rev'") || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);
    } else {
	/* Revirgin the node */
	if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist_rev', 'current_dist', 'dist_rev', 'dist', 'cluster_name')") || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);

	if(qprep(h->db, &q, "INSERT INTO hashfs (key, value) VALUES (:k , :v)"))
	    goto change_revoke_fail;

	if(qbind_text(q, ":k", "current_dist_rev") || qbind_int64(q, ":v", 0) || qstep_noret(q))
	    goto change_revoke_fail;
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "current_dist") || qbind_blob(q, ":v", "", 0) || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);
    }

    if(sx_hashfs_set_rbl_info(h, 0, 0, NULL))
	goto change_revoke_fail;

    ret = sx_hashfs_job_unlock(h, NULL);
    if(ret)
	goto change_revoke_fail;

    if(qcommit(h->db))
	goto change_revoke_fail;

    ret = OK;

 change_revoke_fail:
    qnullify(q);

    if(ret != OK) {
	msg_set_reason("Failed to rewoke the updated distribution model");
	qrollback(h->db);
    }

    return ret;
}

rc_ty sx_hashfs_hdist_set_rebalanced(sx_hashfs_t *h) {
    sxi_hdist_t *rebalanced;
    const void *cfg;
    unsigned int cfg_len;
    sqlite3_stmt *q = NULL;
    rc_ty ret;

    if(!h) {
	NULLARG();
	return EINVAL;
    }

    if(!h->have_hd) {
	msg_set_reason("The cluster is already rebalanced");
	return EINVAL;
    }

    if(!h->is_rebalancing) {
	msg_set_reason("The cluster is already rebalanced");
	return EINVAL;
    }	

    ret = sxi_hdist_get_cfg(h->hd, &cfg, &cfg_len);
    if(ret) {
	msg_set_reason("Failed to retrieve the current distribution model");
	return ret;
    }

    rebalanced = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!rebalanced) {
	msg_set_reason("Failed to clone the current distribution model");
	return FAIL_EINTERNAL;
    }

    if(sxi_hdist_rebalanced(rebalanced)) {
	msg_set_reason("Failed to flat the current distribution");
	sxi_hdist_free(rebalanced);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_get_cfg(rebalanced, &cfg, &cfg_len);
    if(ret) {
	msg_set_reason("Failed to retrieve the rebalanced distribution model");
	sxi_hdist_free(rebalanced);
	return ret;
    }

    if(qbegin(h->db)) {
	sxi_hdist_free(rebalanced);
	return FAIL_EINTERNAL;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)")) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    if(qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", cfg, cfg_len) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dist_rev") ||
       qbind_int64(q, ":v", sxi_hdist_version(rebalanced)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    qnullify(q);
    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist', 'current_dist_rev')") ||
       qstep_noret(q)) {
	msg_set_reason("Failed to enable the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    if(qcommit(h->db)) {
	msg_set_reason("Failed to commit the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    ret = OK;
    INFO("Distribution rebalanced (version changed from %lld to %lld)", (long long)h->hd_rev, (long long)sxi_hdist_version(rebalanced));

 rebalanced_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sxi_hdist_free(rebalanced);

    return ret;
}

rc_ty sx_hashfs_hdist_change_commit(sx_hashfs_t *h) {
    sqlite3_stmt *q;
    rc_ty s = OK;

    DEBUG("IN %s", __FUNCTION__);
    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist', 'current_dist_rev')") ||
       qstep_noret(q)) {
	msg_set_reason("Failed to enable new distribution model");
	s = FAIL_EINTERNAL;
    } else if((s = sx_hashfs_job_unlock(h, NULL)) != OK)
	WARN("Failed to unlock jobs after enabling new model");
    else
	DEBUG("Distribution change committed");

    qnullify(q);
    return s;
}

rc_ty sx_hashfs_hdist_rebalance(sx_hashfs_t *h) {
    job_t job_id;
    sx_nodelist_t *singlenode = NULL;
    const sx_node_t *self = sx_hashfs_self(h);
    rc_ty ret;

    DEBUG("IN %s", __FUNCTION__);

    sqlite3_reset(h->qx_wipehold);
    if(qstep_noret(h->qx_wipehold)) {
	WARN("Failed to wipe hold list");
	return FAIL_EINTERNAL;
    }

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }

    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret) {
	WARN("Cannot add self to nodelist");
	goto hdistreb_fail;
    }

    ret = sx_hashfs_job_new_begin(h);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, &job_id, JOBTYPE_REBALANCE_BLOCKS, JOB_NO_EXPIRY, "BLOCKRB", NULL, 0, singlenode);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_notrigger(h, job_id, 0, &job_id, JOBTYPE_REBALANCE_FILES, JOB_NO_EXPIRY, "FILERB", NULL, 0, singlenode);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_end(h);
    if(ret)
	goto hdistreb_fail;

    ret = OK;

 hdistreb_fail:
    sx_nodelist_delete(singlenode);
    return ret;
}

rc_ty sx_hashfs_challenge_gen(sx_hashfs_t *h, sx_hash_challenge_t *c, int random_challenge) {
    unsigned char md[SXI_SHA1_BIN_LEN];
    unsigned int mdlen;
    sxi_hmac_sha1_ctx *hmac_ctx;
    rc_ty ret;

    if(random_challenge) {
	if(sxi_rand_pseudo_bytes(c->challenge, sizeof(c->challenge)) == -1) {
	    WARN("Cannot generate random bytes");
	    msg_set_reason("Failed to generate random nounce");
	    return FAIL_EINTERNAL;
	}
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if (!hmac_ctx)
        return 1;
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, &h->tokenkey, sizeof(h->tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, c->challenge, sizeof(c->challenge)) ||
       !sxi_hmac_sha1_update(hmac_ctx, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary)) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &mdlen) ||
       mdlen != sizeof(c->response)) {
	msg_set_reason("Failed to compute nounce hmac");
	CRIT("Cannot genearate nounce hmac");
	ret = FAIL_EINTERNAL;
    } else {
	memcpy(c->response, md, sizeof(c->response));
	ret = OK;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);

    return ret;
}

/* MODHDIST: this has got a lot in common with sx_storage_activate
 * except it's the entry for the cluster instead od sxadm */
rc_ty sx_hashfs_setnodedata(sx_hashfs_t *h, const char *name, const sx_uuid_t *node_uuid, uint16_t port, int use_ssl, const char *ssl_ca_crt) {
    rc_ty ret = FAIL_EINTERNAL;
    char *ssl_ca_file = NULL;
    sqlite3_stmt *q = NULL;
    int rollback = 0;

    if(!h || !name || !node_uuid || !*name) {
	NULLARG();
	return EFAULT;
    }
    if(!sx_storage_is_bare(h)) {
	msg_set_reason("Storage was already activated");
	return EINVAL;
    }

    if(use_ssl && ssl_ca_crt) {
	unsigned int cafilen = strlen(h->dir) + sizeof("/sxcert.012345678.pem");
	unsigned int cadatalen = strlen(ssl_ca_crt);
	if(cadatalen) {
	    ssl_ca_file = wrap_malloc(cafilen);
	    if(!ssl_ca_file) {
		OOM();
		return ENOMEM;
	    }
	    snprintf(ssl_ca_file, cafilen, "%s/sxcert.pem", h->dir);
	    while(1) {
		int fd = open(ssl_ca_file, O_CREAT|O_EXCL|O_WRONLY, 0640);
		if(fd < 0) {
		    if(errno == EEXIST) {
			snprintf(ssl_ca_file, cafilen, "%s/sxcert.%08x.pem", h->dir, rand());
			continue;
		    }
		    msg_set_reason("Cannot create CA certificate file");
		    goto setnodedata_fail;
		}
		while(cadatalen) {
		    ssize_t done = write(fd, ssl_ca_crt, cadatalen);
		    if(done < 0) {
			close(fd);
			msg_set_reason("Cannot write CA certificate file");
			goto setnodedata_fail;
		    }
		    cadatalen -= done;
		    ssl_ca_crt += done;
		}
		if(close(fd)) {
		    msg_set_reason("Cannot close CA certificate file");
		    goto setnodedata_fail;
		}
		break;
	    }
	}
    }

    if(qbegin(h->db))
	goto setnodedata_fail;
    rollback = 1;

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto setnodedata_fail;

    if(qbind_text(q, ":k", "ssl_ca_file") || qbind_text(q, ":v", ssl_ca_file ? ssl_ca_file : "") || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "cluster_name") || qbind_text(q, ":v", name) || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "http_port") || qbind_int(q, ":v", port) || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "node") || qbind_blob(q, ":v", node_uuid->binary, sizeof(node_uuid->binary)) || qstep_noret(q))
	goto setnodedata_fail;
    qnullify(q);

    if(qprep(h->db, &q, "DELETE FROM users WHERE uid <> 0") || qstep_noret(q))
	goto setnodedata_fail;

    if(qcommit(h->db))
	goto setnodedata_fail;

    ret = OK;
    rollback = 0;
 setnodedata_fail:
    if(rollback)
	qrollback(h->db);

    sqlite3_finalize(q);
    free(ssl_ca_file);
    return ret;
}


static int64_t dbfilesize(sxi_db_t *db) {
    const char *dbfile;
    struct stat st;
    if(!db || !db->handle)
	return 0;

    dbfile = sqlite3_db_filename(db->handle, "main");
    if(!dbfile)
	return 0;

    if(stat(dbfile, &st))
	return 0;

    return st.st_size;
}

void sx_storage_usage(sx_hashfs_t *h, int64_t *allocated, int64_t *committed) {
    unsigned int i, j;
    int64_t al, ci;

    /* The allocated size is the amount of space taken on disk.
     * - for the DB files this it the size of the .db files
     * - for the DATA files this is the size of the .bin files
     *
     * The committed size is the amount of space actually used.
     * - for the DB files it's the same as for allocated (*)
     * - for the DATA files this is the number of blocks * the block size
     * (*) we could use PRAGMA (page_size * page_count) here but, considering that
     * the outcome could be puzzling to the casual user and that, in the end, the
     * difference would be pretty tiny compared to the overall space usage, it's
     * better to just account for the file size here too
     */

    al = dbfilesize(h->db);
    al += dbfilesize(h->tempdb);
    al += dbfilesize(h->tempdb);
    al += dbfilesize(h->eventdb);
    al += dbfilesize(h->xferdb);

    for(i=0; i<METADBS; i++)
	al += dbfilesize(h->metadb[i]);

    ci = al;

    for(j=0; j<SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    int64_t rows = get_count(h->datadb[j][i], "blocks");
	    int64_t dbsize = dbfilesize(h->datadb[j][i]);
	    struct stat st;

	    al += dbsize;
	    ci += dbsize + rows * bsz[j];
	    if(!fstat(h->datafd[j][i], &st))
		al += st.st_size;
	}
    }
    if(allocated)
	*allocated = al;
    if(committed)
	*committed = ci;
}

const sx_uuid_t *sx_hashfs_distinfo(sx_hashfs_t *h, unsigned int *version, uint64_t *checksum) {
    const sx_uuid_t *ret;
    if(!h || !h->have_hd)
	return NULL;

    ret = sxi_hdist_uuid(h->hd);
    if(!ret)
	return NULL;

    if(checksum)
	*checksum = sxi_hdist_checksum(h->hd);
    if(version)
	*version = sxi_hdist_version(h->hd);

    return ret;
}

/* starts iterating from the beginning */
static rc_ty sx_hashfs_blocks_restart(sx_hashfs_t *h, unsigned rebalance_version)
{
    DEBUG("iteration reset with rebalance_version: %d", rebalance_version);
    unsigned i,j;
    for(j=0;j<SIZES;j++) {
        for(i=0;i<HASHDBS;i++) {
            sqlite3_reset(h->rit.q[j][i]);
            sqlite3_reset(h->qb_get_meta[j][i]);
            sqlite3_reset(h->qb_deleteold[j][i]);
            sqlite3_clear_bindings(h->rit.q[j][i]);
            sqlite3_clear_bindings(h->qb_get_meta[j][i]);
            sqlite3_clear_bindings(h->qb_deleteold[j][i]);
        }
    }
    for(j=0;j<SIZES;j++) {
        for(i=0;i<HASHDBS;i++) {
            if (qbind_blob(h->rit.q[j][i], ":prevhash", "", 0))
                return FAIL_EINTERNAL;
            if (qbind_int(h->qb_get_meta[j][i], ":current_age", rebalance_version))
                return FAIL_EINTERNAL;
            if (qbind_int(h->qb_deleteold[j][i], ":current_age", rebalance_version))
                return FAIL_EINTERNAL;
        }
    }
    h->rit.sizeidx = h->rit.ndbidx = h->rit.iter_no_more = 0;
    h->rit.deleted = h->rit.ignored = h->rit.all = 0;
    h->rit.restart_count++;
    return OK;
}

void sx_hashfs_blockmeta_free(block_meta_t **blockmetaptr)
{
    if (!blockmetaptr)
        return;
    block_meta_t* blockmeta = *blockmetaptr;
    free(blockmeta->entries);
    blockmeta->entries = NULL;
    blockmeta->count = 0;
    free(blockmeta);
    *blockmetaptr = NULL;
}

static rc_ty fill_block_meta(sx_hashfs_t *h, sqlite3_stmt *qmeta, block_meta_t *blockmeta, int64_t blockid)
{
    int ret;
    if (qbind_int64(qmeta, ":blockid", blockid))
        return FAIL_EINTERNAL;
    blockmeta->count = 0;
    sqlite3_reset(qmeta);
    while ((ret = qstep(qmeta)) == SQLITE_ROW) {
        int64_t count = sqlite3_column_int64(qmeta, 1);
        if (!count)
            continue;
        blockmeta->entries = wrap_realloc_or_free(blockmeta->entries, ++blockmeta->count * sizeof(*blockmeta->entries));
        if (!blockmeta->entries)
            return ENOMEM;
        block_meta_entry_t *e = &blockmeta->entries[blockmeta->count - 1];
        e->replica = sqlite3_column_int(qmeta, 0);
        e->count = count;
    }
    sqlite3_reset(qmeta);
    if (ret != SQLITE_DONE)
        return FAIL_EINTERNAL;
    return OK;
}

/* iterate over all hashes once */
static rc_ty sx_hashfs_blocks_next(sx_hashfs_t *h, block_meta_t *blockmeta)
{
    int ret;
    memset(blockmeta, 0, sizeof(*blockmeta));
    for (;h->rit.sizeidx < SIZES; h->rit.sizeidx++) {
        for (;h->rit.ndbidx < HASHDBS; h->rit.ndbidx++) {
            sqlite3_stmt *q = h->rit.q[h->rit.sizeidx][h->rit.ndbidx];
            sqlite3_stmt *qmeta = h->qb_get_meta[h->rit.sizeidx][h->rit.ndbidx];
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            do {
                ret = qstep(q);
                DEBUG("iterating on size=%d,ndb=%d => %s", h->rit.sizeidx, h->rit.ndbidx,
                      ret == SQLITE_DONE ? "DONE" :
                      ret == SQLITE_ROW ? "ROW" :
                      "ERROR");
                if (ret == SQLITE_DONE) {
                    /* reset iterator for current db, so next time it starts
                     * from the beginning */
                    sqlite3_reset(q);
                    if (qbind_blob(q, ":prevhash", "", 0)) {
                        sqlite3_reset(q);
                        sqlite3_reset(qmeta);
                        return FAIL_EINTERNAL;
                    }
                } else if (ret == SQLITE_ROW) {
                    int64_t blockid = sqlite3_column_int64(q, 0);
                    if (sqlite3_column_bytes(q, 1) != sizeof(blockmeta->hash)) {
                        WARN("bad blob size");
                        sqlite3_reset(q);
                        sqlite3_reset(qmeta);
                        return FAIL_EINTERNAL;
                    }
                    memcpy(blockmeta, sqlite3_column_blob(q, 1), sizeof(blockmeta->hash));
                    blockmeta->blocksize = bsz[h->rit.sizeidx];
                    sqlite3_reset(q);
                    if (qbind_blob(q, ":prevhash", blockmeta->hash.b, sizeof(blockmeta->hash.b))) {
                        sqlite3_reset(q);
                        sqlite3_reset(qmeta);
                        return FAIL_EINTERNAL;
                    }
                    if (fill_block_meta(h, qmeta, blockmeta, blockid)) {
                        sqlite3_reset(q);
                        sqlite3_reset(qmeta);
                        WARN("failed to fill block meta");
                        return FAIL_EINTERNAL;
                    }
                    if (blockmeta->count) {
                        DEBUG("hs:%d,ndb:%d,blockmeta count=%ld", h->rit.sizeidx, h->rit.ndbidx, blockmeta->count);
                        DEBUGHASH("_next: ", &blockmeta->hash);
                        return OK;
                    }
                    DEBUGHASH("blockmeta.count=0", &blockmeta->hash);
                    /* blockmeta.count = 0 means this hash has no OLD counters
                     * so skip it
                     * */
                } else {
                    WARN("iteration failed");
                    sqlite3_reset(q);
                    sqlite3_reset(qmeta);
                    return FAIL_EINTERNAL;
                }
            } while (ret == SQLITE_ROW);
        }
        h->rit.ndbidx = 0;
    }
    DEBUG("iteration cycle finished");
    return ITER_NO_MORE; /* end of all hashes */
}

rc_ty sx_hashfs_br_init(sx_hashfs_t *h)
{
    unsigned rebalance_version = sxi_hdist_version(h->hd);
    DEBUG("block rebalance initialized");
    h->rit.rebalance_ver = rebalance_version;
    h->rit.restart_count = 0;
    return OK;
}

rc_ty sx_hashfs_br_begin(sx_hashfs_t *h)
{
    unsigned rebalance_version = sxi_hdist_version(h->hd);
    if (rebalance_version != h->rit.rebalance_ver)
        sx_hashfs_br_init(h);
    DEBUG("previous internal iteration: %lld deleted, %lld ignored, %lld total",
          (long long)h->rit.deleted, (long long)h->rit.ignored, (long long)h->rit.all);
    if (h->rit.deleted + h->rit.ignored > h->rit.all)
        WARN("bad deleted/ignored counters");
    /* iteration finished, check whether we processed all hashes */
    if (h->rit.iter_no_more && h->rit.restart_count && h->rit.deleted + h->rit.ignored == h->rit.all) {
        DEBUG("outer iteration done!");
        return ITER_NO_MORE;
    }
    if (h->rit.iter_no_more || !h->rit.restart_count)
        /* reset and iterate again */
        return sx_hashfs_blocks_restart(h, h->rit.rebalance_ver);
    else
        return OK; /* continue iterating from previous position */
}

rc_ty sx_hashfs_br_next(sx_hashfs_t *h, block_meta_t **blockmetaptr)
{
    rc_ty ret;
    if (!blockmetaptr) {
        NULLARG();
        return EFAULT;
    }
    block_meta_t *blockmeta = *blockmetaptr = wrap_calloc(1, sizeof(*blockmeta));
    if (!blockmeta)
        return ENOMEM;
    ret = sx_hashfs_blocks_next(h, blockmeta);
    if (ret == OK) {
        DEBUGHASH("br_next", &blockmeta->hash);
        h->rit.all++;
        return OK;
    }
    sx_hashfs_blockmeta_free(blockmetaptr);
    if (ret == ITER_NO_MORE) {
        DEBUG("internal iteration done: %lld deleted, %lld ignored, %lld total",
              (long long)h->rit.deleted, (long long)h->rit.ignored, (long long)h->rit.all);
        if (h->rit.deleted + h->rit.ignored > h->rit.all)
            WARN("bad deleted/ignored counters");
        h->rit.iter_no_more = 1;
    }
    return ret;
}

rc_ty sx_hashfs_br_delete(sx_hashfs_t *h, const block_meta_t *blockmeta)
{
    unsigned int ndb, hs;
    rc_ty ret;

    if (!h || !blockmeta) {
        NULLARG();
        return EFAULT;
    }
    ndb = gethashdb(&blockmeta->hash);
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == blockmeta->blocksize)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", blockmeta->blocksize);
	return FAIL_BADBLOCKSIZE;
    }
    sqlite3_stmt *q = h->qb_deleteold[hs][ndb];
    sqlite3_reset(q);
    if (qbind_blob(q, ":hash", blockmeta->hash.b, sizeof(blockmeta->hash.b)))
        return FAIL_EINTERNAL; 
    /* :current_age bound in blocks_restart */
    ret = qstep_noret(q);
    sqlite3_reset(q);
    if (ret == OK) {
        h->rit.deleted++;
        DEBUGHASH("_delete on", &blockmeta->hash);
    }
    return ret;
}

rc_ty sx_hashfs_br_ignore(sx_hashfs_t *h, const block_meta_t *blockmeta)
{
    DEBUGHASH("_ignore on", &blockmeta->hash);
    h->rit.ignored++;
    return OK;
}


rc_ty sx_hashfs_blkrb_hold(sx_hashfs_t *h, const sx_hash_t *block, unsigned int blocksize, const sx_node_t *node) {
    const sx_uuid_t *uuid;

    if(!h || !block || !node) {
        NULLARG();
        return EFAULT;
    }

    if(sx_hashfs_check_blocksize(blocksize) != OK)
	return FAIL_BADBLOCKSIZE;

    uuid = sx_node_uuid(node);
    if(!uuid) {
	msg_set_reason("Invalid node target for block to put on hold");
	return EINVAL;
    }

    sqlite3_reset(h->qx_hold);
    if(qbind_blob(h->qx_hold, ":b", block, sizeof(*block)) ||
       qbind_int(h->qx_hold, ":s", blocksize) ||
       qbind_blob(h->qx_hold, ":n", uuid->binary, sizeof(uuid->binary)) ||
       qstep_noret(h->qx_hold))
	return FAIL_EINTERNAL;

    return OK;
}

rc_ty sx_hashfs_blkrb_can_gc(sx_hashfs_t *h, const sx_hash_t *block, unsigned int blocksize) {
    rc_ty ret;
    int r;

    if(!h || !block) {
        NULLARG();
        return EFAULT;
    }

    if(sx_hashfs_check_blocksize(blocksize) != OK)
	return FAIL_BADBLOCKSIZE;

    sqlite3_reset(h->qx_isheld);
    if(qbind_blob(h->qx_isheld, ":b", block, sizeof(*block)) ||
       qbind_int(h->qx_isheld, ":s", blocksize))
	return FAIL_EINTERNAL;

    r = qstep(h->qx_isheld);
    if(r == SQLITE_ROW) {
	sqlite3_reset(h->qx_isheld);
	ret = FAIL_LOCKED;
    } else if(r == SQLITE_DONE)
	ret = OK;
    else
	ret = FAIL_EINTERNAL;

    return ret;
}

rc_ty sx_hashfs_blkrb_release(sx_hashfs_t *h, uint64_t pushq_id) {
    if(!h) {
        NULLARG();
        return EFAULT;
    }

    sqlite3_reset(h->qx_isheld);
    if(qbind_int64(h->qx_release, ":pushid", pushq_id) ||
       qstep_noret(h->qx_release))
	return FAIL_EINTERNAL;

    return OK;
}

rc_ty sx_hashfs_blkrb_is_complete(sx_hashfs_t *h) {
    int s;

    sqlite3_reset(h->qx_hasheld);
    s = qstep(h->qx_hasheld);
    sqlite3_reset(h->qx_hasheld);

    if(s == SQLITE_ROW)
	return EEXIST;
    else if(s == SQLITE_DONE)
	return OK;
    else
	return FAIL_EINTERNAL;
}

rc_ty sx_hashfs_relocs_populate(sx_hashfs_t *h) {
    const sx_hashfs_volume_t *vol;
    sx_uuid_t selfuuid;
    unsigned int i;
    rc_ty r;

    for(i=0; i<METADBS; i++) {
	sqlite3_reset(h->qm_wiperelocs[i]);
	if(qstep_noret(h->qm_wiperelocs[i])) {
	    WARN("Failed to wipe relocation queue on db %u", i);
	    return FAIL_EINTERNAL;
	}
    }

    if(sx_hashfs_self_uuid(h, &selfuuid))
	return FAIL_EINTERNAL;
	
    r = sx_hashfs_volume_first(h, &vol, 0);
    while(r == OK) {
	sx_nodelist_t *prevnodes, *nextnodes;
	const sx_node_t *prev, *next;
	sx_hash_t hash;

	if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
	    WARN("hashing volume name failed");
	    return FAIL_EINTERNAL;
	}

	prevnodes = sx_hashfs_hashnodes(h, NL_PREV, &hash, vol->replica_count);
	if(!prevnodes) {
	    WARN("cannot determine the previous owner of block: %s", msg_get_reason());
	    return FAIL_EINTERNAL;
	}

	nextnodes = sx_hashfs_hashnodes(h, NL_NEXT, &hash, vol->replica_count);
	if(!nextnodes) {
	    WARN("cannot determine the next owner of block");
	    sx_nodelist_delete(prevnodes);
	    return FAIL_EINTERNAL;
	}

	prev = sx_nodelist_lookup_index(prevnodes, &selfuuid, &i);
	if(prev) {
	    /* I was the i-th owner of this volume */
	    next = sx_nodelist_get(nextnodes, i);
	    if(next && !sx_nodelist_lookup(prevnodes, sx_node_uuid(next))) {
		const sx_uuid_t *uuid = sx_node_uuid(next);

		INFO("Setting files in volume %s to be relocated from here to %s(%s) for replica %u", vol->name, sx_node_uuid_str(next), sx_node_addr(next), i);
		/* The upcoming i-th owner of this volume wans't already an owner:
		 * all volume files are setup for relocation */
		for(i=0; i<METADBS; i++) {
		    sqlite3_reset(h->qm_addrelocs[i]);
		    if(qbind_blob(h->qm_addrelocs[i], ":node", uuid->binary, sizeof(uuid->binary)) ||
		       qbind_int64(h->qm_addrelocs[i], ":volid", vol->id) ||
		       qstep_noret(h->qm_addrelocs[i])) {
			WARN("Failed to add relocation queue on db %u for volume %llu", i, (long long)vol->id);
			sx_nodelist_delete(prevnodes);
			sx_nodelist_delete(nextnodes);
			return FAIL_EINTERNAL;
		    }
		}
	    }
	}

	sx_nodelist_delete(prevnodes);
	sx_nodelist_delete(nextnodes);
	r = sx_hashfs_volume_next(h);
    }

    return r == ITER_NO_MORE ? OK : r;
}


void sx_hashfs_relocs_begin(sx_hashfs_t *h) {
    h->relocdb_start = h->relocdb_cur = rand() % METADBS;
    h->relocid = 0;
}

static rc_ty relocs_delete(sx_hashfs_t *h, unsigned int relocdb, int64_t relocid) {
    sqlite3_reset(h->qm_delreloc[relocdb]);
    if(qbind_int64(h->qm_delreloc[relocdb], ":fileid", relocid) ||
       qstep_noret(h->qm_delreloc[relocdb]))
	return FAIL_EINTERNAL;
    return OK;
}


void sx_hashfs_reloc_free(const sx_reloc_t *reloc) {
    sx_reloc_t *dr = (sx_reloc_t *)reloc;
    if(!dr)
	return;
    free(dr->blocks);
    sxc_meta_free(dr->metadata);
    free(dr);
}


rc_ty sx_hashfs_relocs_next(sx_hashfs_t *h, const sx_reloc_t **reloc) {
    if(!h || !reloc) {
	NULLARG();
	return EFAULT;
    }

    *reloc = NULL;
    while(1) {
	unsigned int ndb = h->relocdb_cur;
	sqlite3_stmt *q = h->qm_getreloc[ndb];
	const sx_hashfs_volume_t *volume;
    	const char *name, *rev;
	const void *content;
	unsigned int content_len, i;
	sx_uuid_t targetid;
	sx_reloc_t *rlc;
	int64_t volid;
	rc_ty ret;
	int r;

	sqlite3_reset(q);
	if(qbind_int64(q, ":prev", h->relocid))
	    return FAIL_EINTERNAL;

	r = qstep(q);
	if(r == SQLITE_DONE) {
	    h->relocid = 0;
	    h->relocdb_cur = (ndb + 1) % METADBS;
	    if(h->relocdb_cur == h->relocdb_start)
		return ITER_NO_MORE;
	    continue;
	}

	if(r != SQLITE_ROW)
	    return FAIL_EINTERNAL;

	h->relocid = sqlite3_column_int64(q, 0);
	if(sqlite3_column_type(q, 2) == SQLITE_NULL) {
	    /* This file no longer exists */
	    sqlite3_reset(q);
	    ret = relocs_delete(h, ndb, h->relocid);
	    if(ret != OK)
		return ret;
	    continue;
	}

	volid = sqlite3_column_int64(q, 2);
	name = (const char *)sqlite3_column_text(q, 3);
	rev = (const char *)sqlite3_column_text(q, 5);
	content_len = sqlite3_column_bytes(q, 6);
	content = sqlite3_column_blob(q, 6);
	if(!name ||
	   sqlite3_column_bytes(q, 1) != sizeof(targetid.binary) ||
	   !rev ||
	   (!content && content_len) ||
	   content_len % sizeof(sx_hash_t)) {
	    WARN("Bad file %lld in %u", (long long)h->relocid, ndb);
	    sqlite3_reset(q);
	    return FAIL_EINTERNAL;
	}

	rlc = wrap_calloc(1, sizeof(*rlc));
	if(!rlc) {
	    sqlite3_reset(q);
	    return ENOMEM;
	}
	if(content_len) {
	    rlc->blocks = wrap_malloc(content_len);
	    if(!rlc->blocks) {
		sqlite3_reset(q);
		sx_hashfs_reloc_free(rlc);
		return ENOMEM;
	    }
	}
	rlc->metadata = sxc_meta_new(h->sx);
	if(!rlc->metadata) {
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return ENOMEM;
	}

	if(parse_revision(rev, &rlc->file.created_at)) {
	    WARN("Bad revision on file %lld in %u", (long long)h->relocid, ndb);
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return FAIL_EINTERNAL;
	}

	rlc->file.file_size = sqlite3_column_int64(q, 4);
	uuid_from_binary(&targetid, sqlite3_column_blob(q, 1));
	rlc->target = sx_nodelist_lookup(sx_hashfs_nodelist(h, NL_NEXT), &targetid);
	if(!rlc->target) {
	    WARN("File id %lld in %u has invalid target %s", (long long)h->relocid, ndb, targetid.string);
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return FAIL_EINTERNAL;
	}

	strncpy(rlc->file.name, name, sizeof(rlc->file.name));
	strncpy(rlc->file.revision, rev, sizeof(rlc->file.revision));
	rlc->file.revision[sizeof(rlc->file.revision)-1] = '\0';
	rlc->file.nblocks = size_to_blocks(rlc->file.file_size, NULL, &rlc->file.block_size);
	if(content_len)
	    memcpy(rlc->blocks, content, content_len);

	ret = sx_hashfs_volume_by_id(h, volid, &volume);
	if(ret) {
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return ret;
	}
	memcpy(&rlc->volume, volume, sizeof(*volume));

	ret = fill_filemeta(h, ndb, h->relocid);
	sqlite3_reset(q);
	if(ret != OK) {
	    WARN("Failed to load metadata for file %lld in %u", (long long)h->relocid, ndb);
	    sx_hashfs_reloc_free(rlc);
	    return ret;
	}

	for(i=0; i<h->nmeta; i++) {
	    if(sxc_meta_setval(rlc->metadata, h->meta[i].key, h->meta[i].value, h->meta[i].value_len)) {
		sx_hashfs_reloc_free(rlc);
		return ENOMEM;
	    }
	}

	rlc->reloc_id = h->relocid;
	rlc->reloc_db = ndb;

	*reloc = rlc;
	return OK;
    }
}

rc_ty sx_hashfs_relocs_delete(sx_hashfs_t *h, const sx_reloc_t *reloc) {
    if(!h || !reloc) {
	NULLARG();
	return EFAULT;
    }

    return relocs_delete(h, reloc->reloc_db, reloc->reloc_id);
}


rc_ty sx_hashfs_rb_cleanup(sx_hashfs_t *h) {
    const sx_hashfs_volume_t *vol;
    sx_uuid_t selfuuid;
    unsigned int i;
    rc_ty r;

    sqlite3_reset(h->qx_wipehold);
    if(qstep_noret(h->qx_wipehold)) {
	WARN("Failed to wipe hold list");
	return FAIL_EINTERNAL;
    }

    for(i=0; i<METADBS; i++) {
	sqlite3_reset(h->qm_wiperelocs[i]);
	if(qstep_noret(h->qm_wiperelocs[i])) {
	    WARN("Failed to wipe relocation queue on db %u", i);
	    return FAIL_EINTERNAL;
	}
    }

    if(sx_hashfs_self_uuid(h, &selfuuid))
	return FAIL_EINTERNAL;
 
    r = sx_hashfs_volume_first(h, &vol, 0);
    while(r == OK) {
	sx_nodelist_t *volnodes;
	sx_hash_t hash;

	if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
	    WARN("hashing volume name failed");
	    return FAIL_EINTERNAL;
	}

	volnodes = sx_hashfs_hashnodes(h, NL_NEXT, &hash, vol->replica_count);
	if(!volnodes)
	    return FAIL_EINTERNAL;

	do {
	    unsigned int i;
	    INFO("Nodes for volume %s:", vol->name);
	    for(i=0; i<sx_nodelist_count(volnodes); i++) {
		const sx_node_t *n = sx_nodelist_get(volnodes, i);
		INFO(" - %s(%s)", sx_node_uuid_str(n), sx_node_addr(n));
	    }
	} while(0);

	if(!sx_nodelist_lookup(volnodes, &selfuuid)) {
	    INFO("Removing all files from %s which no longer belong in here", vol->name);
	    for(i=0; i<METADBS; i++) {
		sqlite3_reset(h->qm_delbyvol[i]);
		if(qbind_int64(h->qm_delbyvol[i], ":volid", vol->id) ||
		   qstep_noret(h->qm_delbyvol[i])) {
                    WARN("Failed to delete relocated files on %u for volume %llu", i, (long long)vol->id);
                    return FAIL_EINTERNAL;
                }
	    }
            sqlite3_reset(h->q_setvolcursize);
            if(qbind_int64(h->q_setvolcursize, ":size", 0) ||
               qbind_int64(h->q_setvolcursize, ":volume", vol->id) ||
               qstep_noret(h->q_setvolcursize)) {
                WARN("Failed to reset volume size for volume %lld", (long long)vol->id);
                return FAIL_EINTERNAL;
            }
	}
	sx_nodelist_delete(volnodes);
	r = sx_hashfs_volume_next(h);
    }

    if(r == ITER_NO_MORE)
	r = OK;

    return r;
}

rc_ty sx_hashfs_get_rbl_info(sx_hashfs_t *h, int *complete, const char **description) {
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "rebalance_message"))
	goto getrblinfo_fail;
    r = qstep(h->q_getval);
    if(r != SQLITE_ROW) {
	if(r == SQLITE_DONE)
	    ret = ENOENT;
	goto getrblinfo_fail;
    }

    if(description) {
	const char *desc = (const char *)sqlite3_column_text(h->q_getval, 0);
	if(!desc)
	    desc = "Status description not available";
	strncpy(h->job_message, desc, sizeof(h->job_message));
	h->job_message[sizeof(h->job_message)-1] = '\0';
	*description = h->job_message;
    }

    /* Small race in here but it's not worth a transaction
     * "rebalance_complete" has got the last saying */
    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "rebalance_complete"))
	goto getrblinfo_fail;
    r = qstep(h->q_getval);
    if(r != SQLITE_ROW) {
	if(description)
	    *description = NULL;
	if(r == SQLITE_DONE)
	    ret = ENOENT;
	goto getrblinfo_fail;
    }

    if(complete)
	*complete = sqlite3_column_int(h->q_getval, 0);
    ret = OK;

 getrblinfo_fail:
    sqlite3_reset(h->q_getval);
    if(ret == FAIL_EINTERNAL)
	msg_set_reason("Failed to retrieve rebalance state info from the database");

    return ret;
}

rc_ty sx_hashfs_set_rbl_info(sx_hashfs_t *h, int active, int complete, const char *description) {
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(!active) {
	if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('rebalance_message', 'rebalance_complete')") ||
	   qstep_noret(q))
	    msg_set_reason("Failed to set rebalance state info to inactive");
	else
	    ret = OK;
	goto setrblinfo_fail;
    }

    if(!description)
	description = "Status description not available";

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto setrblinfo_fail;
    if(qbind_text(q, ":k", "rebalance_message") || qbind_text(q, ":v", description) || qstep_noret(q))
	goto setrblinfo_fail;
    if(qbind_text(q, ":k", "rebalance_complete") || qbind_int(q, ":v", (complete != 0)) || qstep_noret(q))
	goto setrblinfo_fail;
    ret = OK;

 setrblinfo_fail:
    sqlite3_finalize(q);

    if(ret == FAIL_EINTERNAL)
	msg_set_reason("Failed to update rebalance state info");

    return ret;
}


rc_ty sx_hashfs_hdist_endrebalance(sx_hashfs_t *h) {
    job_t job_id;
    sx_nodelist_t *singlenode = NULL;
    const sx_node_t *self = sx_hashfs_self(h);
    rc_ty ret;

    DEBUG("IN %s", __FUNCTION__);

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }

    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret)
	WARN("Cannot add self to nodelist");
    else
	ret = sx_hashfs_job_new(h, 0, &job_id, JOBTYPE_REBALANCE_CLEANUP, JOB_NO_EXPIRY, "CLEANRB", NULL, 0, singlenode);

    sx_nodelist_delete(singlenode);
    return ret;
}

