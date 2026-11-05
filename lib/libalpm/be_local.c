/*
 *  be_local.c : LMDB backend for the local database
 *
 *  Full replacement of the directory-per-package local DB.
 *  Everything lives in a single LMDB environment under the local db path.
 *
 *  Key schema:
 *    \x00version          -> version string
 *    p:<name>             -> "name-version" (package index)
 *    d:<name>             -> desc blob (same text format as old desc file)
 *    f:<name>             -> files blob
 *    s:<name>             -> install scriptlet
 *    m:<name>             -> mtree
 *    c:<name>             -> changelog
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <limits.h>
#include <lmdb.h>

#include <archive.h>
#include <archive_entry.h>

#include "db.h"
#include "alpm_list.h"
#include "libarchive-compat.h"
#include "log.h"
#include "util.h"
#include "alpm.h"
#include "handle.h"
#include "package.h"
#include "deps.h"
#include "filelist.h"

size_t ALPM_LOCAL_DB_VERSION = 9;

#define LMDB_ENV_NAME "data.mdb"
#define LMDB_MAPSIZE  (512UL * 1024 * 1024) /* 512 MiB */

static int local_db_read(alpm_pkg_t *info, int inforeq);

#define LAZY_LOAD(info) \
	do { \
		if(!(pkg->infolevel & info)) { \
			local_db_read(pkg, info); \
		} \
	} while(0)

/* ---------- LMDB helpers ---------- */

static int lmdb_env_open(alpm_db_t *db, MDB_env **env_out, int rdonly)
{
	char path[PATH_MAX];
	const char *dbpath;
	MDB_env *env;
	int rc, flags = MDB_NOSUBDIR;

	dbpath = _alpm_db_path(db);
	if(!dbpath) return -1;

	snprintf(path, sizeof(path), "%s%s", dbpath, LMDB_ENV_NAME);

	rc = mdb_env_create(&env);
	if(rc) return -1;
	rc = mdb_env_set_mapsize(env, LMDB_MAPSIZE);
	if(rc) { mdb_env_close(env); return -1; }
	if(rdonly) flags |= MDB_RDONLY;
	rc = mdb_env_open(env, path, flags, 0644);
	if(rc) {
		mdb_env_close(env);
		return -1;
	}
	*env_out = env;
	return 0;
}

static int lmdb_get(alpm_db_t *db, const char *key, char **val, size_t *vlen)
{
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val k, d;
	int rc, ret = -1;

	*val = NULL;
	if(vlen) *vlen = 0;

	if(lmdb_env_open(db, &env, 1) != 0) return -1;
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if(rc) goto out;
	rc = mdb_dbi_open(txn, NULL, 0, &dbi);
	if(rc) goto out;

	k.mv_data = (void *)key;
	k.mv_size = strlen(key);
	rc = mdb_get(txn, dbi, &k, &d);
	if(rc == MDB_NOTFOUND) { ret = 1; goto out; }
	if(rc) goto out;

	MALLOC(*val, d.mv_size + 1, goto out);
	memcpy(*val, d.mv_data, d.mv_size);
	(*val)[d.mv_size] = '\0';
	if(vlen) *vlen = d.mv_size;
	ret = 0;
out:
	if(txn) mdb_txn_abort(txn);
	if(env) mdb_env_close(env);
	return ret;
}

static int lmdb_put(alpm_db_t *db, const char *key, const void *data, size_t len)
{
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val k, d;
	int rc, ret = -1;

	if(lmdb_env_open(db, &env, 0) != 0) return -1;
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if(rc) goto out;
	rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
	if(rc) goto out;

	k.mv_data = (void *)key;
	k.mv_size = strlen(key);
	d.mv_data = (void *)data;
	d.mv_size = len;
	rc = mdb_put(txn, dbi, &k, &d, 0);
	if(rc) goto out;
	rc = mdb_txn_commit(txn);
	txn = NULL;
	if(rc) goto out;
	ret = 0;
out:
	if(txn) mdb_txn_abort(txn);
	if(env) mdb_env_close(env);
	return ret;
}

static int lmdb_del_prefix(alpm_db_t *db, const char *name)
{
	/* delete p:, d:, f:, s:, m:, c: for this package */
	static const char *prefs[] = { "p:", "d:", "f:", "s:", "m:", "c:" };
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	int rc, i, ret = -1;
	char key[512];

	if(lmdb_env_open(db, &env, 0) != 0) return -1;
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if(rc) goto out;
	rc = mdb_dbi_open(txn, NULL, 0, &dbi);
	if(rc) goto out;

	for(i = 0; i < 6; i++) {
		snprintf(key, sizeof(key), "%s%s", prefs[i], name);
		MDB_val k = { .mv_data = key, .mv_size = strlen(key) };
		mdb_del(txn, dbi, &k, NULL); /* ignore notfound */
	}
	rc = mdb_txn_commit(txn);
	txn = NULL;
	if(rc) goto out;
	ret = 0;
out:
	if(txn) mdb_txn_abort(txn);
	if(env) mdb_env_close(env);
	return ret;
}

/* store arbitrary blob (scriptlet/mtree/changelog) */
int _alpm_local_db_set_blob(alpm_db_t *db, alpm_pkg_t *info,
		const char *kind, const void *data, size_t len)
{
	char key[512];
	const char *pref = NULL;

	if(strcmp(kind, "install") == 0) pref = "s:";
	else if(strcmp(kind, "mtree") == 0) pref = "m:";
	else if(strcmp(kind, "changelog") == 0) pref = "c:";
	else return -1;

	snprintf(key, sizeof(key), "%s%s", pref, info->name);
	return lmdb_put(db, key, data, len);
}

/* ---------- lazy accessors (unchanged shape) ---------- */

static const char *_cache_get_base(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->base; }
static const char *_cache_get_desc(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->desc; }
static const char *_cache_get_url(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->url; }
static alpm_time_t _cache_get_builddate(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->builddate; }
static alpm_time_t _cache_get_installdate(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->installdate; }
static const char *_cache_get_packager(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->packager; }
static const char *_cache_get_arch(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->arch; }
static off_t _cache_get_isize(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->isize; }
static alpm_pkgreason_t _cache_get_reason(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->reason; }
static int _cache_get_validation(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->validation; }
static int _cache_has_scriptlet(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_SCRIPTLET); return pkg->scriptlet; }
static alpm_list_t *_cache_get_licenses(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->licenses; }
static alpm_list_t *_cache_get_groups(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->groups; }
static alpm_list_t *_cache_get_depends(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->depends; }
static alpm_list_t *_cache_get_optdepends(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->optdepends; }
static alpm_list_t *_cache_get_makedepends(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->makedepends; }
static alpm_list_t *_cache_get_checkdepends(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->checkdepends; }
static alpm_list_t *_cache_get_conflicts(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->conflicts; }
static alpm_list_t *_cache_get_provides(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->provides; }
static alpm_list_t *_cache_get_replaces(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->replaces; }
static alpm_filelist_t *_cache_get_files(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_FILES); return &pkg->files; }
static alpm_list_t *_cache_get_backup(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_FILES); return pkg->backup; }
static alpm_list_t *_cache_get_xdata(alpm_pkg_t *pkg) { LAZY_LOAD(INFRQ_DESC); return pkg->xdata; }

/* materialize blob to temp path under db/.run/ for scriptlet execution */
static char *materialize_blob(alpm_db_t *db, const char *name, const char *kind)
{
	char key[512], dir[PATH_MAX], path[PATH_MAX];
	char *data = NULL;
	size_t len = 0;
	const char *pref;
	FILE *fp;
	const char *dbpath = _alpm_db_path(db);

	if(strcmp(kind, "install") == 0) pref = "s:";
	else if(strcmp(kind, "mtree") == 0) pref = "m:";
	else if(strcmp(kind, "changelog") == 0) pref = "c:";
	else return NULL;

	snprintf(key, sizeof(key), "%s%s", pref, name);
	if(lmdb_get(db, key, &data, &len) != 0 || !data) return NULL;

	snprintf(dir, sizeof(dir), "%s.run", dbpath);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/%s.%s", dir, name, kind);
	fp = fopen(path, "w");
	if(!fp) { free(data); return NULL; }
	fwrite(data, 1, len, fp);
	fclose(fp);
	free(data);
	return strdup(path);
}

static void *_cache_changelog_open(alpm_pkg_t *pkg)
{
	char *path = materialize_blob(alpm_pkg_get_db(pkg), pkg->name, "changelog");
	FILE *f = path ? fopen(path, "r") : NULL;
	free(path);
	return f;
}
static size_t _cache_changelog_read(void *ptr, size_t size, const alpm_pkg_t UNUSED *pkg, void *fp)
{ return fread(ptr, 1, size, (FILE *)fp); }
static int _cache_changelog_close(const alpm_pkg_t UNUSED *pkg, void *fp)
{ return fclose((FILE *)fp); }

static struct archive *_cache_mtree_open(alpm_pkg_t *pkg)
{
	struct archive *mtree;
	char *mtfile = materialize_blob(alpm_pkg_get_db(pkg), pkg->name, "mtree");
	if(!mtfile) return NULL;

	if((mtree = archive_read_new()) == NULL) {
		free(mtfile);
		return NULL;
	}
	_alpm_archive_read_support_filter_all(mtree);
	archive_read_support_format_mtree(mtree);
	if(_alpm_archive_read_open_file(mtree, mtfile, ALPM_BUFFER_SIZE)) {
		_alpm_archive_read_free(mtree);
		free(mtfile);
		return NULL;
	}
	free(mtfile);
	return mtree;
}
static int _cache_mtree_next(const alpm_pkg_t UNUSED *pkg, struct archive *mtree, struct archive_entry **entry)
{ return archive_read_next_header(mtree, entry); }
static int _cache_mtree_close(const alpm_pkg_t UNUSED *pkg, struct archive *mtree)
{ return _alpm_archive_read_free(mtree); }

static int _cache_force_load(alpm_pkg_t *pkg) { return local_db_read(pkg, INFRQ_ALL); }

static const struct pkg_operations local_pkg_ops = {
	.get_base = _cache_get_base,
	.get_desc = _cache_get_desc,
	.get_url = _cache_get_url,
	.get_builddate = _cache_get_builddate,
	.get_installdate = _cache_get_installdate,
	.get_packager = _cache_get_packager,
	.get_arch = _cache_get_arch,
	.get_isize = _cache_get_isize,
	.get_reason = _cache_get_reason,
	.get_validation = _cache_get_validation,
	.has_scriptlet = _cache_has_scriptlet,
	.get_licenses = _cache_get_licenses,
	.get_groups = _cache_get_groups,
	.get_depends = _cache_get_depends,
	.get_optdepends = _cache_get_optdepends,
	.get_makedepends = _cache_get_makedepends,
	.get_checkdepends = _cache_get_checkdepends,
	.get_conflicts = _cache_get_conflicts,
	.get_provides = _cache_get_provides,
	.get_replaces = _cache_get_replaces,
	.get_files = _cache_get_files,
	.get_backup = _cache_get_backup,
	.get_xdata = _cache_get_xdata,
	.changelog_open = _cache_changelog_open,
	.changelog_read = _cache_changelog_read,
	.changelog_close = _cache_changelog_close,
	.mtree_open = _cache_mtree_open,
	.mtree_next = _cache_mtree_next,
	.mtree_close = _cache_mtree_close,
	.force_load = _cache_force_load,
};

/* ---------- validate / create / populate ---------- */

static int local_db_create(alpm_db_t *db, const char *dbpath)
{
	char path[PATH_MAX];
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val k, d;
	char ver[32];
	int rc;

	if(mkdir(dbpath, 0755) != 0 && errno != EEXIST) {
		RET_ERR(db->handle, ALPM_ERR_DB_CREATE, -1);
	}
	snprintf(path, sizeof(path), "%s%s", dbpath, LMDB_ENV_NAME);
	rc = mdb_env_create(&env);
	if(rc) RET_ERR(db->handle, ALPM_ERR_DB_CREATE, -1);
	mdb_env_set_mapsize(env, LMDB_MAPSIZE);
	rc = mdb_env_open(env, path, MDB_NOSUBDIR, 0644);
	if(rc) { mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_CREATE, -1); }
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if(rc) { mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_CREATE, -1); }
	rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
	if(rc) { mdb_txn_abort(txn); mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_CREATE, -1); }

	snprintf(ver, sizeof(ver), "%zu", ALPM_LOCAL_DB_VERSION);
	k.mv_data = "\0version"; k.mv_size = 8;
	d.mv_data = ver; d.mv_size = strlen(ver);
	mdb_put(txn, dbi, &k, &d, 0);
	mdb_txn_commit(txn);
	mdb_env_close(env);
	return 0;
}

static int local_db_validate(alpm_db_t *db)
{
	const char *dbpath;
	char path[PATH_MAX];
	char *ver = NULL;
	size_t version;

	if(db->status & DB_STATUS_VALID) return 0;
	if(db->status & DB_STATUS_INVALID) return -1;

	dbpath = _alpm_db_path(db);
	if(!dbpath) RET_ERR(db->handle, ALPM_ERR_DB_OPEN, -1);

	snprintf(path, sizeof(path), "%s%s", dbpath, LMDB_ENV_NAME);
	if(access(path, F_OK) != 0) {
		if(local_db_create(db, dbpath) == 0) {
			db->status |= DB_STATUS_VALID | DB_STATUS_EXISTS;
			db->status &= ~(DB_STATUS_INVALID | DB_STATUS_MISSING);
			return 0;
		}
		db->status |= DB_STATUS_MISSING;
		db->status &= ~DB_STATUS_EXISTS;
		return -1;
	}

	db->status |= DB_STATUS_EXISTS;
	db->status &= ~DB_STATUS_MISSING;

	if(lmdb_get(db, "\0version", &ver, NULL) != 0 || !ver) {
		db->status |= DB_STATUS_INVALID;
		db->handle->pm_errno = ALPM_ERR_DB_VERSION;
		return -1;
	}
	version = strtoul(ver, NULL, 10);
	free(ver);
	if(version != ALPM_LOCAL_DB_VERSION) {
		db->status |= DB_STATUS_INVALID;
		db->handle->pm_errno = ALPM_ERR_DB_VERSION;
		return -1;
	}
	db->status |= DB_STATUS_VALID;
	db->status &= ~DB_STATUS_INVALID;
	return 0;
}

static int local_db_populate(alpm_db_t *db)
{
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_cursor *cur = NULL;
	MDB_val key, data;
	size_t count = 0, est = 0;
	int rc;

	if(db->status & DB_STATUS_INVALID)
		RET_ERR(db->handle, ALPM_ERR_DB_INVALID, -1);
	if(db->status & DB_STATUS_MISSING)
		RET_ERR(db->handle, ALPM_ERR_DB_NOT_FOUND, -1);

	if(lmdb_env_open(db, &env, 1) != 0)
		RET_ERR(db->handle, ALPM_ERR_DB_OPEN, -1);
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if(rc) { mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_OPEN, -1); }
	rc = mdb_dbi_open(txn, NULL, 0, &dbi);
	if(rc) { mdb_txn_abort(txn); mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_OPEN, -1); }
	rc = mdb_cursor_open(txn, dbi, &cur);
	if(rc) { mdb_txn_abort(txn); mdb_env_close(env); RET_ERR(db->handle, ALPM_ERR_DB_OPEN, -1); }

	/* count packages */
	while(mdb_cursor_get(cur, &key, &data, MDB_NEXT) == 0) {
		if(key.mv_size > 2 && memcmp(key.mv_data, "p:", 2) == 0) est++;
	}

	db->pkgcache = _alpm_pkghash_create(est ? est : 16);
	if(!db->pkgcache) {
		mdb_cursor_close(cur); mdb_txn_abort(txn); mdb_env_close(env);
		RET_ERR(db->handle, ALPM_ERR_MEMORY, -1);
	}

	rc = mdb_cursor_get(cur, &key, &data, MDB_FIRST);
	while(rc == 0) {
		if(key.mv_size > 2 && memcmp(key.mv_data, "p:", 2) == 0) {
			char namever[512];
			alpm_pkg_t *pkg;

			if(data.mv_size >= sizeof(namever)) { rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT); continue; }
			memcpy(namever, data.mv_data, data.mv_size);
			namever[data.mv_size] = '\0';

			pkg = _alpm_pkg_new();
			if(!pkg) {
				mdb_cursor_close(cur); mdb_txn_abort(txn); mdb_env_close(env);
				RET_ERR(db->handle, ALPM_ERR_MEMORY, -1);
			}
			if(_alpm_splitname(namever, &pkg->name, &pkg->version, &pkg->name_hash) != 0) {
				_alpm_pkg_free(pkg);
				rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
				continue;
			}
			if(_alpm_pkghash_find(db->pkgcache, pkg->name)) {
				_alpm_pkg_free(pkg);
				rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
				continue;
			}
			pkg->origin = ALPM_PKG_FROM_LOCALDB;
			pkg->origin_data.db = db;
			pkg->ops = &local_pkg_ops;
			pkg->handle = db->handle;

			if(local_db_read(pkg, INFRQ_BASE) == -1) {
				_alpm_log(db->handle, ALPM_LOG_ERROR, _("corrupted database entry '%s'\n"), namever);
				_alpm_pkg_free(pkg);
				rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
				continue;
			}
			_alpm_pkg_check_meta(pkg);
			if(_alpm_pkghash_add(&db->pkgcache, pkg) == NULL) {
				_alpm_pkg_free(pkg);
				mdb_cursor_close(cur); mdb_txn_abort(txn); mdb_env_close(env);
				RET_ERR(db->handle, ALPM_ERR_MEMORY, -1);
			}
			count++;
		}
		rc = mdb_cursor_get(cur, &key, &data, MDB_NEXT);
	}

	mdb_cursor_close(cur);
	mdb_txn_abort(txn);
	mdb_env_close(env);

	if(count > 0)
		db->pkgcache->list = alpm_list_msort(db->pkgcache->list, count, _alpm_pkg_cmp);
	_alpm_log(db->handle, ALPM_LOG_DEBUG, "lmdb: added %zu packages for db '%s'\n",
			count, db->treename);
	return 0;
}

/* path helper: for scriptlet/mtree/changelog materialize; for NULL return db root */
char *_alpm_local_db_pkgpath(alpm_db_t *db, alpm_pkg_t *info, const char *filename)
{
	if(!filename) {
		/* no more per-pkg dirs; return db path itself */
		const char *dbpath = _alpm_db_path(db);
		return dbpath ? strdup(dbpath) : NULL;
	}
	if(strcmp(filename, "install") == 0 ||
	   strcmp(filename, "mtree") == 0 ||
	   strcmp(filename, "changelog") == 0) {
		return materialize_blob(db, info->name, filename);
	}
	/* desc/files no longer on disk */
	return NULL;
}

/* ---------- read from LMDB blobs ---------- */

#define READ_NEXT() do { \
	if(!*cursor || (*cursor == '\0')) break; \
	{ char *nl = strchr(cursor, '\n'); \
	  if(nl) { *nl = '\0'; line = cursor; cursor = nl + 1; } \
	  else { line = cursor; cursor = cursor + strlen(cursor); } \
	} \
} while(0)

static int parse_desc_blob(alpm_pkg_t *info, char *blob)
{
	char *cursor = blob;
	char *line = NULL;
	alpm_handle_t *handle = info->handle;

	while(cursor && *cursor) {
		READ_NEXT();
		if(!line || !*line) continue;

		if(strcmp(line, "%NAME%") == 0) {
			READ_NEXT();
		} else if(strcmp(line, "%VERSION%") == 0) {
			READ_NEXT();
		} else if(strcmp(line, "%BASE%") == 0) {
			READ_NEXT(); STRDUP(info->base, line, return -1);
		} else if(strcmp(line, "%DESC%") == 0) {
			READ_NEXT(); STRDUP(info->desc, line, return -1);
		} else if(strcmp(line, "%URL%") == 0) {
			READ_NEXT(); STRDUP(info->url, line, return -1);
		} else if(strcmp(line, "%ARCH%") == 0) {
			READ_NEXT(); STRDUP(info->arch, line, return -1);
		} else if(strcmp(line, "%BUILDDATE%") == 0) {
			READ_NEXT(); info->builddate = _alpm_parsedate(line);
		} else if(strcmp(line, "%INSTALLDATE%") == 0) {
			READ_NEXT(); info->installdate = _alpm_parsedate(line);
		} else if(strcmp(line, "%PACKAGER%") == 0) {
			READ_NEXT(); STRDUP(info->packager, line, return -1);
		} else if(strcmp(line, "%SIZE%") == 0) {
			READ_NEXT(); info->isize = (off_t)strtoll(line, NULL, 10);
		} else if(strcmp(line, "%REASON%") == 0) {
			READ_NEXT();
			if(strcmp(line, "0") == 0) info->reason = ALPM_PKG_REASON_EXPLICIT;
			else if(strcmp(line, "1") == 0) info->reason = ALPM_PKG_REASON_DEPEND;
			else info->reason = ALPM_PKG_REASON_UNKNOWN;
		} else if(strcmp(line, "%GROUPS%") == 0) {
			while(1) {
				char *dup;
				READ_NEXT();
				if(!line || !*line) break;
				STRDUP(dup, line, return -1);
				info->groups = alpm_list_add(info->groups, dup);
			}
		} else if(strcmp(line, "%LICENSE%") == 0) {
			while(1) {
				char *dup;
				READ_NEXT();
				if(!line || !*line) break;
				STRDUP(dup, line, return -1);
				info->licenses = alpm_list_add(info->licenses, dup);
			}
		} else if(strcmp(line, "%VALIDATION%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				if(strcmp(line, "none") == 0) info->validation |= ALPM_PKG_VALIDATION_NONE;
				else if(strcmp(line, "sha256") == 0) info->validation |= ALPM_PKG_VALIDATION_SHA256SUM;
				else if(strcmp(line, "pgp") == 0) info->validation |= ALPM_PKG_VALIDATION_SIGNATURE;
			}
		} else if(strcmp(line, "%DEPENDS%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				info->depends = alpm_list_add(info->depends, alpm_dep_from_string(line));
			}
		} else if(strcmp(line, "%OPTDEPENDS%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				info->optdepends = alpm_list_add(info->optdepends, alpm_dep_from_string(line));
			}
		} else if(strcmp(line, "%CONFLICTS%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				info->conflicts = alpm_list_add(info->conflicts, alpm_dep_from_string(line));
			}
		} else if(strcmp(line, "%PROVIDES%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				info->provides = alpm_list_add(info->provides, alpm_dep_from_string(line));
			}
		} else if(strcmp(line, "%REPLACES%") == 0) {
			while(1) {
				READ_NEXT();
				if(!line || !*line) break;
				info->replaces = alpm_list_add(info->replaces, alpm_dep_from_string(line));
			}
		} else if(strcmp(line, "%XDATA%") == 0) {
			while(1) {
				char *eq, *n, *v;
				alpm_pkg_xdata_t *xd;
				READ_NEXT();
				if(!line || !*line) break;
				eq = strchr(line, '=');
				if(!eq) continue;
				*eq = '\0';
				STRDUP(n, line, return -1);
				STRDUP(v, eq + 1, return -1);
				MALLOC(xd, sizeof(*xd), return -1);
				xd->name = n; xd->value = v;
				info->xdata = alpm_list_add(info->xdata, xd);
			}
		}
	}
	(void)handle;
	return 0;
}

static int parse_files_blob(alpm_pkg_t *info, char *blob)
{
	char *cursor = blob;
	char *line = NULL;
	alpm_list_t *files_list = NULL;

	while(cursor && *cursor) {
		READ_NEXT();
		if(!line || !*line) continue;
		if(strcmp(line, "%FILES%") == 0) {
			while(1) {
				char *dup;
				READ_NEXT();
				if(!line || !*line) break;
				STRDUP(dup, line, return -1);
				files_list = alpm_list_add(files_list, dup);
			}
		} else if(strcmp(line, "%BACKUP%") == 0) {
			while(1) {
				char *tab, *name, *hash;
				alpm_backup_t *bak;
				READ_NEXT();
				if(!line || !*line) break;
				tab = strchr(line, '\t');
				if(!tab) continue;
				*tab = '\0';
				STRDUP(name, line, return -1);
				STRDUP(hash, tab + 1, return -1);
				CALLOC(bak, 1, sizeof(*bak), return -1);
				bak->name = name;
				bak->hash = hash;
				info->backup = alpm_list_add(info->backup, bak);
			}
		}
	}

	if(files_list) {
		size_t n = alpm_list_count(files_list), i = 0;
		alpm_list_t *lp;
		MALLOC(info->files.files, n * sizeof(alpm_file_t), return -1);
		info->files.count = n;
		for(lp = files_list; lp; lp = lp->next) {
			info->files.files[i].name = lp->data;
			info->files.files[i].size = 0;
			info->files.files[i].mode = 0;
			i++;
		}
		/* ownership of strings transferred */
		alpm_list_free(files_list);
	}
	return 0;
}

static int local_db_read(alpm_pkg_t *info, int inforeq)
{
	alpm_db_t *db = info->origin_data.db;
	char key[512];
	char *blob = NULL;

	if((info->infolevel & inforeq) == inforeq) return 0;
	if(info->infolevel & INFRQ_ERROR) return -1;

	if(inforeq & INFRQ_DESC && !(info->infolevel & INFRQ_DESC)) {
		snprintf(key, sizeof(key), "d:%s", info->name);
		if(lmdb_get(db, key, &blob, NULL) != 0 || !blob) goto error;
		if(parse_desc_blob(info, blob) != 0) { free(blob); goto error; }
		free(blob); blob = NULL;
		info->infolevel |= INFRQ_DESC | INFRQ_BASE;
	}

	if(inforeq & INFRQ_FILES && !(info->infolevel & INFRQ_FILES)) {
		snprintf(key, sizeof(key), "f:%s", info->name);
		if(lmdb_get(db, key, &blob, NULL) == 0 && blob) {
			if(parse_files_blob(info, blob) != 0) { free(blob); goto error; }
			free(blob); blob = NULL;
		}
		info->infolevel |= INFRQ_FILES;
	}

	if(inforeq & INFRQ_SCRIPTLET && !(info->infolevel & INFRQ_SCRIPTLET)) {
		snprintf(key, sizeof(key), "s:%s", info->name);
		if(lmdb_get(db, key, &blob, NULL) == 0 && blob) {
			info->scriptlet = 1;
			free(blob); blob = NULL;
		} else {
			info->scriptlet = 0;
		}
		info->infolevel |= INFRQ_SCRIPTLET;
	}

	return 0;
error:
	info->infolevel |= INFRQ_ERROR;
	return -1;
}

/* ---------- write ---------- */

static void write_deps_mem(FILE *fp, const char *header, alpm_list_t *deplist)
{
	alpm_list_t *lp;
	if(!deplist) return;
	fputs(header, fp); fputc('\n', fp);
	for(lp = deplist; lp; lp = lp->next) {
		char *s = alpm_dep_compute_string(lp->data);
		fputs(s, fp); fputc('\n', fp);
		free(s);
	}
	fputc('\n', fp);
}

int _alpm_local_db_prepare(alpm_db_t *db, alpm_pkg_t UNUSED *info)
{
	/* ensure env exists */
	return local_db_validate(db);
}

int _alpm_local_db_write(alpm_db_t *db, alpm_pkg_t *info, int inforeq)
{
	char key[512], namever[512];
	char *buf = NULL;
	size_t buflen = 0;
	FILE *fp;
	alpm_list_t *lp;
	int retval = 0;

	if(!db || !info || !(db->status & DB_STATUS_LOCAL)) return -1;

	snprintf(namever, sizeof(namever), "%s-%s", info->name, info->version);

	/* index entry */
	snprintf(key, sizeof(key), "p:%s", info->name);
	if(lmdb_put(db, key, namever, strlen(namever)) != 0) return -1;

	if(inforeq & INFRQ_DESC) {
		fp = open_memstream(&buf, &buflen);
		if(!fp) return -1;

		fprintf(fp, "%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n", info->name, info->version);
		if(info->base) fprintf(fp, "%%BASE%%\n%s\n\n", info->base);
		if(info->desc) fprintf(fp, "%%DESC%%\n%s\n\n", info->desc);
		if(info->url) fprintf(fp, "%%URL%%\n%s\n\n", info->url);
		if(info->arch) fprintf(fp, "%%ARCH%%\n%s\n\n", info->arch);
		if(info->builddate) fprintf(fp, "%%BUILDDATE%%\n%jd\n\n", (intmax_t)info->builddate);
		if(info->installdate) fprintf(fp, "%%INSTALLDATE%%\n%jd\n\n", (intmax_t)info->installdate);
		if(info->packager) fprintf(fp, "%%PACKAGER%%\n%s\n\n", info->packager);
		if(info->isize) fprintf(fp, "%%SIZE%%\n%jd\n\n", (intmax_t)info->isize);
		if(info->reason) fprintf(fp, "%%REASON%%\n%u\n\n", info->reason);
		if(info->groups) {
			fputs("%GROUPS%\n", fp);
			for(lp = info->groups; lp; lp = lp->next) { fputs(lp->data, fp); fputc('\n', fp); }
			fputc('\n', fp);
		}
		if(info->licenses) {
			fputs("%LICENSE%\n", fp);
			for(lp = info->licenses; lp; lp = lp->next) { fputs(lp->data, fp); fputc('\n', fp); }
			fputc('\n', fp);
		}
		if(info->validation) {
			fputs("%VALIDATION%\n", fp);
			if(info->validation & ALPM_PKG_VALIDATION_NONE) fputs("none\n", fp);
			if(info->validation & ALPM_PKG_VALIDATION_SHA256SUM) fputs("sha256\n", fp);
			if(info->validation & ALPM_PKG_VALIDATION_SIGNATURE) fputs("pgp\n", fp);
			fputc('\n', fp);
		}
		write_deps_mem(fp, "%REPLACES%", info->replaces);
		write_deps_mem(fp, "%DEPENDS%", info->depends);
		write_deps_mem(fp, "%OPTDEPENDS%", info->optdepends);
		write_deps_mem(fp, "%CONFLICTS%", info->conflicts);
		write_deps_mem(fp, "%PROVIDES%", info->provides);
		if(info->xdata) {
			fputs("%XDATA%\n", fp);
			for(lp = info->xdata; lp; lp = lp->next) {
				alpm_pkg_xdata_t *pd = lp->data;
				fprintf(fp, "%s=%s\n", pd->name, pd->value);
			}
			fputc('\n', fp);
		}
		fclose(fp);
		snprintf(key, sizeof(key), "d:%s", info->name);
		if(lmdb_put(db, key, buf, buflen) != 0) retval = -1;
		free(buf); buf = NULL; buflen = 0;
	}

	if(inforeq & INFRQ_FILES) {
		fp = open_memstream(&buf, &buflen);
		if(!fp) return -1;
		if(info->files.count) {
			size_t i;
			fputs("%FILES%\n", fp);
			for(i = 0; i < info->files.count; i++) {
				fputs(info->files.files[i].name, fp);
				fputc('\n', fp);
			}
			fputc('\n', fp);
		}
		if(info->backup) {
			fputs("%BACKUP%\n", fp);
			for(lp = info->backup; lp; lp = lp->next) {
				const alpm_backup_t *b = lp->data;
				fprintf(fp, "%s\t%s\n", b->name, b->hash);
			}
			fputc('\n', fp);
		}
		fclose(fp);
		snprintf(key, sizeof(key), "f:%s", info->name);
		if(lmdb_put(db, key, buf, buflen) != 0) retval = -1;
		free(buf);
	}

	return retval;
}

int _alpm_local_db_remove(alpm_db_t *db, alpm_pkg_t *info)
{
	return lmdb_del_prefix(db, info->name);
}

int SYMEXPORT alpm_pkg_set_reason(alpm_pkg_t *pkg, alpm_pkgreason_t reason)
{
	ASSERT(pkg != NULL, return -1);
	ASSERT(pkg->origin == ALPM_PKG_FROM_LOCALDB,
			RET_ERR(pkg->handle, ALPM_ERR_WRONG_ARGS, -1));
	ASSERT(pkg->origin_data.db == pkg->handle->db_local,
			RET_ERR(pkg->handle, ALPM_ERR_WRONG_ARGS, -1));

	if(alpm_pkg_get_reason(pkg) == reason) return 0;
	pkg->reason = reason;
	if(_alpm_local_db_write(pkg->handle->db_local, pkg, INFRQ_DESC))
		RET_ERR(pkg->handle, ALPM_ERR_DB_WRITE, -1);
	return 0;
}

static const struct db_operations local_db_ops = {
	.validate = local_db_validate,
	.populate = local_db_populate,
	.unregister = _alpm_db_unregister,
};

alpm_db_t *_alpm_db_register_local(alpm_handle_t *handle)
{
	alpm_db_t *db;

	_alpm_log(handle, ALPM_LOG_DEBUG, "registering local database (lmdb)\n");
	db = _alpm_db_new("local", 1);
	if(!db) {
		handle->pm_errno = ALPM_ERR_DB_CREATE;
		return NULL;
	}
	db->ops = &local_db_ops;
	db->handle = handle;
	db->usage = ALPM_DB_USAGE_ALL;

	if(local_db_validate(db)) {
		_alpm_db_free(db);
		return NULL;
	}
	handle->db_local = db;
	return db;
}
