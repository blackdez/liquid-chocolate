/*
 * Copyright (C) 2005-2010 Junjiro R. Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * inode private data
 */

#include "aufs.h"

struct inode *au_h_iptr(struct inode *inode, aufs_bindex_t bindex)
{
	struct inode *h_inode;

	IiMustAnyLock(inode);

	h_inode = au_ii(inode)->ii_hinode[0 + bindex].hi_inode;
	AuDebugOn(h_inode && atomic_read(&h_inode->i_count) <= 0);
	return h_inode;
}

/* todo: hard/soft set? */
void au_hiput(struct au_hinode *hinode)
{
	au_hn_free(hinode);
	dput(hinode->hi_whdentry);
	iput(hinode->hi_inode);
}

unsigned int au_hi_flags(struct inode *inode, int isdir)
{
	unsigned int flags;
	const unsigned int mnt_flags = au_mntflags(inode->i_sb);

	flags = 0;
	if (au_opt_test(mnt_flags, XINO))
		au_fset_hi(flags, XINO);
	if (isdir && au_opt_test(mnt_flags, UDBA_HNOTIFY))
		au_fset_hi(flags, HNOTIFY);
	return flags;
}

void au_set_h_iptr(struct inode *inode, aufs_bindex_t bindex,
		   struct inode *h_inode, unsigned int flags)
{
	struct au_hinode *hinode;
	struct inode *hi;
	struct au_iinfo *iinfo = au_ii(inode);

	IiMustWriteLock(inode);

	hinode = iinfo->ii_hinode + bindex;
	hi = hinode->hi_inode;
	AuDebugOn(h_inode && atomic_read(&h_inode->i_count) <= 0);

	if (hi)
		au_hiput(hinode);
	hinode->hi_inode = h_inode;
	if (h_inode) {
		int err;
		struct super_block *sb = inode->i_sb;
		struct au_branch *br;

		if (bindex == iinfo->ii_bstart)
			au_cpup_igen(inode, h_inode);
		br = au_sbr(sb, bindex);
		hinode->hi_id = br->br_id;
		if (au_ftest_hi(flags, XINO)) {
			err = au_xino_write(sb, bindex, h_inode->i_ino,
					    inode->i_ino);
			if (unlikely(err))
				AuIOErr1("failed au_xino_write() %d\n", err);
		}

		if (au_ftest_hi(flags, HNOTIFY)
		    && au_br_hnotifyable(br->br_perm)) {
			err = au_hn_alloc(hinode, inode, h_inode);
			if (unlikely(err))
				AuIOErr1("au_hn_alloc() %d\n", err);
		}
	}
}

void au_set_hi_wh(struct inode *inode, aufs_bindex_t bindex,
		  struct dentry *h_wh)
{
	struct au_hinode *hinode;

	IiMustWriteLock(inode);

	hinode = au_ii(inode)->ii_hinode + bindex;
	AuDebugOn(hinode->hi_whdentry);
	hinode->hi_whdentry = h_wh;
}

void au_update_iigen(struct inode *inode)
{
	atomic_set(&au_ii(inode)->ii_generation, au_sigen(inode->i_sb));
	/* smp_mb(); */ /* atomic_set */
}

/* it may be called at remount time, too */
void au_update_ibrange(struct inode *inode, int do_put_zero)
{
	struct au_iinfo *iinfo;

	iinfo = au_ii(inode);
	if (!iinfo || iinfo->ii_bstart < 0)
		return;

	IiMustWriteLock(inode);

	if (do_put_zero) {
		aufs_bindex_t bindex;

		for (bindex = iinfo->ii_bstart; bindex <= iinfo->ii_bend;
		     bindex++) {
			struct inode *h_i;

			h_i = iinfo->ii_hinode[0 + bindex].hi_inode;
			if (h_i && !h_i->i_nlink)
				au_set_h_iptr(inode, bindex, NULL, 0);
		}
	}

	iinfo->ii_bstart = -1;
	while (++iinfo->ii_bstart <= iinfo->ii_bend)
		if (iinfo->ii_hinode[0 + iinfo->ii_bstart].hi_inode)
			break;
	if (iinfo->ii_bstart > iinfo->ii_bend) {
		iinfo->ii_bstart = -1;
		iinfo->ii_bend = -1;
		return;
	}

	iinfo->ii_bend++;
	while (0 <= --iinfo->ii_bend)
		if (iinfo->ii_hinode[0 + iinfo->ii_bend].hi_inode)
			break;
	AuDebugOn(iinfo->ii_bstart > iinfo->ii_bend || iinfo->ii_bend < 0);
}

/* ---------------------------------------------------------------------- */

void au_icntnr_init_once(void *_c)
{
	struct au_icntnr *c = _c;
	struct au_iinfo *iinfo = &c->iinfo;
	static struct lock_class_key aufs_ii;

	au_rw_init(&iinfo->ii_rwsem);
	au_rw_class(&iinfo->ii_rwsem, &aufs_ii);
	inode_init_once(&c->vfs_inode);
}

int au_iinfo_init(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct super_block *sb;
	int nbr, i;

	sb = inode->i_sb;
	iinfo = &(container_of(inode, struct au_icntnr, vfs_inode)->iinfo);
	nbr = au_sbend(sb) + 1;
	if (unlikely(nbr <= 0))
		nbr = 1;
	iinfo->ii_hinode = kcalloc(nbr, sizeof(*iinfo->ii_hinode), GFP_NOFS);
	if (iinfo->ii_hinode) {
		for (i = 0; i < nbr; i++)
			iinfo->ii_hinode[i].hi_id = -1;

		atomic_set(&iinfo->ii_generation, au_sigen(sb));
		/* smp_mb(); */ /* atomic_set */
		iinfo->ii_bstart = -1;
		iinfo->ii_bend = -1;
		iinfo->ii_vdir = NULL;
		return 0;
	}
	return -ENOMEM;
}

int au_ii_realloc(struct au_iinfo *iinfo, int nbr)
{
	int err, sz;
	struct au_hinode *hip;

	AuRwMustWriteLock(&iinfo->ii_rwsem);

	err = -ENOMEM;
	sz = sizeof(*hip) * (iinfo->ii_bend + 1);
	if (!sz)
		sz = sizeof(*hip);
	hip = au_kzrealloc(iinfo->ii_hinode, sz, sizeof(*hip) * nbr, GFP_NOFS);
	if (hip) {
		iinfo->ii_hinode = hip;
		err = 0;
	}

	return err;
}

void au_iinfo_fin(struct inode *inode)
{
	struct au_iinfo *iinfo;
	struct au_hinode *hi;
	struct super_block *sb;
	aufs_bindex_t bindex, bend;
	const unsigned char unlinked = !inode->i_nlink;

	iinfo = au_ii(inode);
	/* bad_inode case */
	if (!iinfo)
		return;

	sb = inode->i_sb;
	if (si_pid_test(sb))
		au_xino_delete_inode(inode, unlinked);
	else {
		/*
		 * it is safe to hide the dependency between sbinfo and
		 * sb->s_umount.
		 */
		lockdep_off();
		si_noflush_read_lock(sb);
		au_xino_delete_inode(inode, unlinked);
		si_read_unlock(sb);
		lockdep_on();
	}

	if (iinfo->ii_vdir)
		au_vdir_free(iinfo->ii_vdir);

	bindex = iinfo->ii_bstart;
	if (bindex >= 0) {
		hi = iinfo->ii_hinode + bindex;
		bend = iinfo->ii_bend;
		while (bindex++ <= bend) {
			if (hi->hi_inode)
				au_hiput(hi);
			hi++;
		}
	}
	kfree(iinfo->ii_hinode);
	AuRwDestroy(&iinfo->ii_rwsem);
}
