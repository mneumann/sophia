
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <libss.h>
#include <libsf.h>
#include <libsr.h>
#include <libso.h>
#include <libsv.h>
#include <libsl.h>
#include <libsd.h>
#include <libsi.h>
#include <libsx.h>
#include <libsy.h>
#include <libse.h>

int se_recoverbegin(sedb *db)
{
	/* open and recover repository */
	se_statusset(&db->status, SE_RECOVER);
	se *e = se_of(&db->o);
	/* do not allow to recover existing databases
	 * during online (only create), since logpool
	 * reply is required. */
	if (se_status(&e->status) == SE_ONLINE)
		db->scheme.path_fail_on_exists = 1;
	int rc = si_open(&db->index, &db->scheme);
	if (ssunlikely(rc == -1))
		goto error;
	db->created = rc;
	return 0;
error:
	se_dbmalfunction(db);
	return -1;
}

int se_recoverend(sedb *db)
{
	se_statusset(&db->status, SE_ONLINE);
	return 0;
}

static int
se_recoverlog(se *e, sl *log)
{
	so *tx = NULL;
	sedb *db = NULL;
	ssiter i;
	ss_iterinit(sl_iter, &i);
	int rc = ss_iteropen(sl_iter, &i, &e->r, &log->file, 1);
	if (ssunlikely(rc == -1))
		return -1;
	for (;;)
	{
		sv *v = ss_iteratorof(&i);
		if (ssunlikely(v == NULL))
			break;

		/* reply transaction */
		uint64_t lsn = sv_lsn(v);
		tx = so_begin(&e->o);
		if (ssunlikely(tx == NULL))
			goto error;

		while (ss_iteratorhas(&i)) {
			v = ss_iteratorof(&i);
			assert(sv_lsn(v) == lsn);
			/* match a database */
			uint32_t dsn = sl_vdsn(v);
			if (db == NULL || db->scheme.id != dsn)
				db = (sedb*)se_dbmatch_id(e, dsn);
			if (ssunlikely(db == NULL)) {
				sr_malfunction(&e->error, "%s",
				               "database id %" PRIu32 "is not declared", dsn);
				goto rlb;
			}
			so *o = so_object(&db->o);
			if (ssunlikely(o == NULL))
				goto rlb;
			so_setstring(o, "raw", sv_pointer(v), sv_size(v));
			so_setstring(o, "log", log, 0);
			
			int flags = sv_flags(v);
			if (flags == SVDELETE) {
				rc = so_delete(tx, o);
			} else
			if (flags == SVUPDATE) {
				rc = so_update(tx, o);
			} else {
				assert(flags == 0);
				rc = so_set(tx, o);
			}
			if (ssunlikely(rc == -1))
				goto rlb;
			ss_gcmark(&log->gc, 1);
			ss_iteratornext(&i);
		}
		if (ssunlikely(sl_iter_error(&i)))
			goto rlb;

		so_setint(tx, "lsn", lsn);
		rc = so_commit(tx);
		if (ssunlikely(rc != 0))
			goto error;
		rc = sl_iter_continue(&i);
		if (ssunlikely(rc == -1))
			goto error;
		if (rc == 0)
			break;
	}
	ss_iteratorclose(&i);
	return 0;
rlb:
	so_destroy(tx);
error:
	ss_iteratorclose(&i);
	return -1;
}

static inline int
se_recoverlogpool(se *e)
{
	sslist *i;
	ss_listforeach(&e->lp.list, i) {
		sl *log = sscast(i, sl, link);
		int rc = se_recoverlog(e, log);
		if (ssunlikely(rc == -1))
			return -1;
		ss_gccomplete(&log->gc);
	}
	return 0;
}

int se_recover(se *e)
{
	slconf *lc = &e->lpconf;
	lc->enable         = e->meta.log_enable;
	lc->path           = e->meta.log_path;
	lc->rotatewm       = e->meta.log_rotate_wm;
	lc->sync_on_rotate = e->meta.log_rotate_sync;
	lc->sync_on_write  = e->meta.log_sync;
	int rc = sl_poolopen(&e->lp, lc);
	if (ssunlikely(rc == -1))
		return -1;
	if (e->meta.two_phase_recover)
		return 0;
	/* recover log files */
	rc = se_recoverlogpool(e);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sl_poolrotate(&e->lp);
	if (ssunlikely(rc == -1))
		goto error;
	return 0;
error:
	se_statusset(&e->status, SE_MALFUNCTION);
	return -1;
}

int se_recover_repository(se *e)
{
	syconf *rc = &e->repconf;
	rc->path        = e->meta.path;
	rc->path_create = e->meta.path_create;
	rc->path_backup = e->meta.backup_path;
	rc->sync = 0;
	return sy_open(&e->rep, &e->r, rc);
}
