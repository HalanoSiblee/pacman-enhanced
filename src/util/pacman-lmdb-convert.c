/*
 * pacman-lmdb-convert — one-shot local DB dir → LMDB migrator
 *
 * Usage: pacman-lmdb-convert [/var/lib/pacman/local]
 *
 * Refuses if data.mdb already exists.
 * Reads package-* dirs (desc/files/install/mtree/changelog) into LMDB.
 * gcc -O2 -o pacman-lmdb-convert pacman-lmdb-convert.c -llmdb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <lmdb.h>

#define MAPSIZE (512UL * 1024 * 1024)
#define ENVNAME "data.mdb"

static int put_file(MDB_txn *txn, MDB_dbi dbi, const char *key,
		const char *path)
{
	FILE *fp;
	char *buf = NULL;
	size_t len = 0, cap = 0;
	MDB_val k, d;
	int rc;

	fp = fopen(path, "rb");
	if(!fp) return 0; /* optional files */

	while(1) {
		if(len + 4096 > cap) {
			cap = cap ? cap * 2 : 8192;
			char *n = realloc(buf, cap);
			if(!n) { free(buf); fclose(fp); return -1; }
			buf = n;
		}
		size_t n = fread(buf + len, 1, 4096, fp);
		len += n;
		if(n < 4096) break;
	}
	fclose(fp);

	k.mv_data = (void *)key;
	k.mv_size = strlen(key);
	d.mv_data = buf;
	d.mv_size = len;
	rc = mdb_put(txn, dbi, &k, &d, 0);
	free(buf);
	return rc ? -1 : 0;
}

static int is_pkg_dir(const char *name)
{
	/* skip . .. ALPM_DB_VERSION data.mdb lock.mdb .run */
	if(name[0] == '.') return 0;
	if(strcmp(name, "ALPM_DB_VERSION") == 0) return 0;
	if(strcmp(name, ENVNAME) == 0) return 0;
	if(strcmp(name, "lock.mdb") == 0) return 0;
	/* must contain at least one '-' for name-version */
	return strchr(name, '-') != NULL;
}

/* split "foo-1.2-3" → name "foo", keep full as namever */
static int split_name(const char *namever, char *name, size_t nlen)
{
	const char *p = strrchr(namever, '-');
	if(!p || p == namever) return -1;
	/* walk back over pkgrel if needed: name-ver-rel */
	const char *q = p;
	while(q > namever && *(q - 1) != '-') q--;
	if(q > namever && *(q - 1) == '-') {
		/* q points at start of version */
		size_t len = (size_t)(q - 1 - namever);
		if(len == 0 || len >= nlen) return -1;
		memcpy(name, namever, len);
		name[len] = '\0';
		return 0;
	}
	size_t len = (size_t)(p - namever);
	if(len == 0 || len >= nlen) return -1;
	memcpy(name, namever, len);
	name[len] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	const char *dbpath = argc > 1 ? argv[1] : "/var/lib/pacman/local";
	char envpath[PATH_MAX];
	char path[PATH_MAX];
	char key[512];
	char name[256];
	DIR *dir;
	struct dirent *ent;
	MDB_env *env = NULL;
	MDB_txn *txn = NULL;
	MDB_dbi dbi;
	MDB_val k, d;
	int rc, count = 0;
	FILE *verfp;
	char verbuf[64] = "9";

	snprintf(envpath, sizeof(envpath), "%s/%s", dbpath, ENVNAME);
	if(access(envpath, F_OK) == 0) {
		fprintf(stderr, "refuse: %s already exists\n", envpath);
		return 1;
	}

	dir = opendir(dbpath);
	if(!dir) {
		fprintf(stderr, "opendir %s: %s\n", dbpath, strerror(errno));
		return 1;
	}

	/* read ALPM_DB_VERSION if present */
	snprintf(path, sizeof(path), "%s/ALPM_DB_VERSION", dbpath);
	verfp = fopen(path, "r");
	if(verfp) {
		if(fgets(verbuf, sizeof(verbuf), verfp)) {
			size_t n = strlen(verbuf);
			while(n && (verbuf[n - 1] == '\n' || verbuf[n - 1] == '\r'))
				verbuf[--n] = '\0';
		}
		fclose(verfp);
	}

	rc = mdb_env_create(&env);
	if(rc) { fprintf(stderr, "mdb_env_create: %s\n", mdb_strerror(rc)); return 1; }
	mdb_env_set_mapsize(env, MAPSIZE);
	rc = mdb_env_open(env, envpath, MDB_NOSUBDIR, 0644);
	if(rc) {
		fprintf(stderr, "mdb_env_open %s: %s\n", envpath, mdb_strerror(rc));
		mdb_env_close(env);
		return 1;
	}
	rc = mdb_txn_begin(env, NULL, 0, &txn);
	if(rc) {
		fprintf(stderr, "mdb_txn_begin: %s\n", mdb_strerror(rc));
		mdb_env_close(env);
		return 1;
	}
	rc = mdb_dbi_open(txn, NULL, MDB_CREATE, &dbi);
	if(rc) {
		fprintf(stderr, "mdb_dbi_open: %s\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		mdb_env_close(env);
		return 1;
	}

	/* version key */
	k.mv_data = "\0version";
	k.mv_size = 8;
	d.mv_data = verbuf;
	d.mv_size = strlen(verbuf);
	if(mdb_put(txn, dbi, &k, &d, 0)) {
		fprintf(stderr, "put version failed\n");
		goto fail;
	}

	while((ent = readdir(dir)) != NULL) {
		struct stat st;

		if(!is_pkg_dir(ent->d_name)) continue;
		snprintf(path, sizeof(path), "%s/%s", dbpath, ent->d_name);
		if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		if(split_name(ent->d_name, name, sizeof(name)) != 0) {
			fprintf(stderr, "skip bad name: %s\n", ent->d_name);
			continue;
		}

		/* p:<name> → name-version */
		snprintf(key, sizeof(key), "p:%s", name);
		k.mv_data = key;
		k.mv_size = strlen(key);
		d.mv_data = (void *)ent->d_name;
		d.mv_size = strlen(ent->d_name);
		if(mdb_put(txn, dbi, &k, &d, 0)) {
			fprintf(stderr, "put index %s failed\n", name);
			goto fail;
		}

		snprintf(path, sizeof(path), "%s/%s/desc", dbpath, ent->d_name);
		snprintf(key, sizeof(key), "d:%s", name);
		if(put_file(txn, dbi, key, path) < 0) {
			fprintf(stderr, "put desc %s failed\n", name);
			goto fail;
		}

		snprintf(path, sizeof(path), "%s/%s/files", dbpath, ent->d_name);
		snprintf(key, sizeof(key), "f:%s", name);
		if(put_file(txn, dbi, key, path) < 0) {
			fprintf(stderr, "put files %s failed\n", name);
			goto fail;
		}

		snprintf(path, sizeof(path), "%s/%s/install", dbpath, ent->d_name);
		snprintf(key, sizeof(key), "s:%s", name);
		if(put_file(txn, dbi, key, path) < 0) {
			fprintf(stderr, "put install %s failed\n", name);
			goto fail;
		}

		snprintf(path, sizeof(path), "%s/%s/mtree", dbpath, ent->d_name);
		snprintf(key, sizeof(key), "m:%s", name);
		if(put_file(txn, dbi, key, path) < 0) {
			fprintf(stderr, "put mtree %s failed\n", name);
			goto fail;
		}

		snprintf(path, sizeof(path), "%s/%s/changelog", dbpath, ent->d_name);
		snprintf(key, sizeof(key), "c:%s", name);
		if(put_file(txn, dbi, key, path) < 0) {
			fprintf(stderr, "put changelog %s failed\n", name);
			goto fail;
		}

		count++;
	}
	closedir(dir);

	rc = mdb_txn_commit(txn);
	txn = NULL;
	if(rc) {
		fprintf(stderr, "commit: %s\n", mdb_strerror(rc));
		mdb_env_close(env);
		return 1;
	}
	mdb_env_close(env);
	printf("converted %d packages → %s\n", count, envpath);
	return 0;

fail:
	if(txn) mdb_txn_abort(txn);
	mdb_env_close(env);
	unlink(envpath);
	return 1;
}
