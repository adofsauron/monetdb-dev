/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2021 MonetDB B.V.
 */

/*
 * @a M. L. Kersten, P. Boncz, N. J. Nes
 * @* BAT Buffer Pool (BBP)
 * The BATs created and loaded are collected in a BAT buffer pool.
 * The Bat Buffer Pool has a number of functions:
 * @table @code
 *
 * @item administration and lookup
 * The BBP is a directory which contains status information about all
 * known BATs.  This interface may be used very heavily, by
 * data-intensive applications.  To eliminate all overhead, read-only
 * access to the BBP may be done by table-lookups. The integer index
 * type for these lookups is @emph{bat}, as retrieved by
 * @emph{b->batCacheid}. The @emph{bat} zero is reserved for the nil
 * bat.
 *
 * @item persistence
 * The BBP is made persistent by saving it to the dictionary file
 * called @emph{BBP.dir} in the database.
 *
 * When the number of BATs rises, having all files in one directory
 * becomes a bottleneck.  The BBP therefore implements a scheme that
 * distributes all BATs in a growing directory tree with at most 64
 * BATs stored in one node.
 *
 * @item buffer management
 * The BBP is responsible for loading and saving of BATs to disk. It
 * also contains routines to unload BATs from memory when memory
 * resources get scarce. For this purpose, it administers BAT memory
 * reference counts (to know which BATs can be unloaded) and BAT usage
 * statistics (it unloads the least recently used BATs).
 *
 * @item recovery
 * When the database is closed or during a run-time syncpoint, the
 * system tables must be written to disk in a safe way, that is immune
 * for system failures (like disk full). To do so, the BBP implements
 * an atomic commit and recovery protocol: first all files to be
 * overwritten are moved to a BACKUP/ dir. If that succeeds, the
 * writes are done. If that also fully succeeds the BACKUP/ dir is
 * renamed to DELETE_ME/ and subsequently deleted.  If not, all files
 * in BACKUP/ are moved back to their original location.
 *
 * @item unloading
 * Bats which have a logical reference (ie. a lrefs > 0) but no memory
 * reference (refcnt == 0) can be unloaded. Unloading dirty bats
 * means, moving the original (committed version) to the BACKUP/ dir
 * and saving the bat. This complicates the commit and recovery/abort
 * issues.  The commit has to check if the bat is already moved. And
 * The recovery has to always move back the files from the BACKUP/
 * dir.
 *
 * @item reference counting
 * Bats use have two kinds of references: logical and physical
 * (pointer) ones.  The logical references are administered by
 * BBPretain/BBPrelease, the physical ones by BBPfix/BBPunfix.
 *
 * @item share counting
 * Views use the heaps of there parent bats. To save guard this, the
 * parent has a shared counter, which is incremented and decremented
 * using BBPshare and BBPunshare. These functions make sure the parent
 * is memory resident as required because of the 'pointer' sharing.
 * @end table
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_private.h"
#include "mutils.h"
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode)	(((mode) & _S_IFMT) == _S_IFDIR)
#endif

/*
 * The BBP has a fixed address, so re-allocation due to a growing BBP
 * caused by one thread does not disturb reads to the old entries by
 * another.  This is implemented using anonymous virtual memory;
 * extensions on the same address are guaranteed because a large
 * non-committed VM area is requested initially. New slots in the BBP
 * are found in O(1) by keeping a freelist that uses the 'next' field
 * in the BBPrec records.
 */
BBPrec *BBP[N_BBPINIT];		/* fixed base VM address of BBP array */
bat BBPlimit = 0;		/* current committed VM BBP array */
static ATOMIC_TYPE BBPsize = ATOMIC_VAR_INIT(0); /* current used size of BBP array */

struct BBPfarm_t BBPfarms[MAXFARMS];

#define KITTENNAP 1		/* used to suspend processing */
#define BBPNONAME "."		/* filler for no name in BBP.dir */
/*
 * The hash index uses a bucket index (int array) of size mask that is
 * tuned for perfect hashing (1 lookup). The bucket chain uses the
 * 'next' field in the BBPrec records.
 */
static MT_Lock BBPnameLock = MT_LOCK_INITIALIZER(BBPnameLock);
static bat *BBP_hash = NULL;		/* BBP logical name hash buckets */
static bat BBP_mask = 0;		/* number of buckets = & mask */
#define BBP_THREADMASK	0		/* originally: 63 */
#if SIZEOF_SIZE_T == 8
#define threadmask(y)	((int) (mix_lng(y) & BBP_THREADMASK))
#else
#define threadmask(y)	((int) (mix_int(y) & BBP_THREADMASK))
#endif
static struct {
	MT_Lock cache;
	bat free;
} GDKbbpLock[BBP_THREADMASK + 1];
#define GDKcacheLock(y)	GDKbbpLock[y].cache
#define BBP_free(y)	GDKbbpLock[y].free

static gdk_return BBPfree(BAT *b);
static void BBPdestroy(BAT *b);
static void BBPuncacheit(bat bid, bool unloaddesc);
static gdk_return BBPprepare(bool subcommit);
static BAT *getBBPdescriptor(bat i, bool lock);
static gdk_return BBPbackup(BAT *b, bool subcommit);
static gdk_return BBPdir_init(void);

/* two lngs of extra info in BBP.dir */
/* these two need to be atomic because of their use in AUTHcommit() */
static ATOMIC_TYPE BBPlogno = ATOMIC_VAR_INIT(0);
static ATOMIC_TYPE BBPtransid = ATOMIC_VAR_INIT(0);

#ifdef HAVE_HGE
/* start out by saying we have no hge, but as soon as we've seen one,
 * we'll always say we do have it */
static bool havehge = false;
#endif

#define BBPtmpcheck(s)	(strncmp(s, "tmp_", 4) == 0)

#define BBPnamecheck(s) (BBPtmpcheck(s) ? strtol((s) + 4, NULL, 8) : 0)

static void
BBP_insert(bat i)
{
	bat idx = (bat) (strHash(BBP_logical(i)) & BBP_mask);

	BBP_next(i) = BBP_hash[idx];
	BBP_hash[idx] = i;
}

static void
BBP_delete(bat i)
{
	bat *h = BBP_hash;
	const char *s = BBP_logical(i);
	bat idx = (bat) (strHash(s) & BBP_mask);

	for (h += idx; (i = *h) != 0; h = &BBP_next(i)) {
		if (strcmp(BBP_logical(i), s) == 0) {
			*h = BBP_next(i);
			break;
		}
	}
}

bat
getBBPsize(void)
{
	return (bat) ATOMIC_GET(&BBPsize);
}

lng
getBBPlogno(void)
{
	return (lng) ATOMIC_GET(&BBPlogno);
}

lng
getBBPtransid(void)
{
	return (lng) ATOMIC_GET(&BBPtransid);
}


/*
 * @+ BBP Consistency and Concurrency
 * While GDK provides the basic building blocks for an ACID system, in
 * itself it is not such a system, as we this would entail too much
 * overhead that is often not needed. Hence, some consistency control
 * is left to the user. The first important user constraint is that if
 * a user updates a BAT, (s)he himself must assure that no-one else
 * accesses this BAT.
 *
 * Concerning buffer management, the BBP carries out a swapping
 * policy.  BATs are kept in memory till the memory is full. If the
 * memory is full, the malloc functions initiate BBP trim actions,
 * that unload the coldest BATs that have a zero reference count. The
 * second important user constraint is therefore that a user may only
 * manipulate live BAT data in memory if it is sure that there is at
 * least one reference count to that BAT.
 *
 * The main BBP array is protected by two locks:
 * @table @code
 * @item GDKcacheLock]
 * this lock guards the free slot management in the BBP array.  The
 * BBP operations that allocate a new slot for a new BAT
 * (@emph{BBPinit},@emph{BBPcacheit}), delete the slot of a destroyed
 * BAT (@emph{BBPreclaim}), or rename a BAT (@emph{BBPrename}), hold
 * this lock. It also protects all BAT (re)naming actions include
 * (read and write) in the hash table with BAT names.
 * @item GDKswapLock
 * this lock guards the swap (loaded/unloaded) status of the
 * BATs. Hence, all BBP routines that influence the swapping policy,
 * or actually carry out the swapping policy itself, acquire this lock
 * (e.g. @emph{BBPfix},@emph{BBPunfix}).  Note that this also means
 * that updates to the BBP_status indicator array must be protected by
 * GDKswapLock.
 *
 * To reduce contention GDKswapLock was split into multiple locks; it
 * is now an array of lock pointers which is accessed by
 * GDKswapLock(bat)
 * @end table
 *
 * Routines that need both locks should first acquire the locks in the
 * GDKswapLock array (in ascending order) and then GDKcacheLock (and
 * release them in reverse order).
 *
 * To obtain maximum speed, read operations to existing elements in
 * the BBP are unguarded. As said, it is the users responsibility that
 * the BAT that is being read is not being modified. BBP update
 * actions that modify the BBP data structure itself are locked by the
 * BBP functions themselves. Hence, multiple concurrent BBP read
 * operations may be ongoing while at the same time at most one BBP
 * write operation @strong{on a different BAT} is executing.  This
 * holds for accesses to the public (quasi-) arrays @emph{BBPcache},
 * @emph{BBPstatus} and @emph{BBPrefs}.
 * These arrays are called quasi as now they are
 * actually stored together in one big BBPrec array called BBP, that
 * is allocated in anonymous VM space, so we can reallocate this
 * structure without changing the base address (a crucial feature if
 * read actions are to go on unlocked while other entries in the BBP
 * may be modified).
 */
static volatile MT_Id locked_by = 0;

/* use a lock instead of atomic instructions so that we wait for
 * BBPlock/BBPunlock */
#define BBP_unload_inc()			\
	do {					\
		MT_lock_set(&GDKunloadLock);	\
		BBPunloadCnt++;			\
		MT_lock_unset(&GDKunloadLock);	\
	} while (0)

#define BBP_unload_dec()			\
	do {					\
		MT_lock_set(&GDKunloadLock);	\
		--BBPunloadCnt;			\
		assert(BBPunloadCnt >= 0);	\
		MT_lock_unset(&GDKunloadLock);	\
	} while (0)

static int BBPunloadCnt = 0;
static MT_Lock GDKunloadLock = MT_LOCK_INITIALIZER(GDKunloadLock);

void
BBPtmlock(void)
{
	MT_lock_set(&GDKtmLock);
}

void
BBPtmunlock(void)
{
	MT_lock_unset(&GDKtmLock);
}

void
BBPlock(void)
{
	int i;

	/* wait for all pending unloads to finish */
	MT_lock_set(&GDKunloadLock);
	while (BBPunloadCnt > 0) {
		MT_lock_unset(&GDKunloadLock);
		MT_sleep_ms(1);
		MT_lock_set(&GDKunloadLock);
	}

	MT_lock_set(&GDKtmLock);
	for (i = 0; i <= BBP_THREADMASK; i++)
		MT_lock_set(&GDKcacheLock(i));
	for (i = 0; i <= BBP_BATMASK; i++)
		MT_lock_set(&GDKswapLock(i));
	locked_by = MT_getpid();

	MT_lock_unset(&GDKunloadLock);
}

void
BBPunlock(void)
{
	int i;

	for (i = BBP_BATMASK; i >= 0; i--)
		MT_lock_unset(&GDKswapLock(i));
	for (i = BBP_THREADMASK; i >= 0; i--)
		MT_lock_unset(&GDKcacheLock(i));
	locked_by = 0;
	MT_lock_unset(&GDKtmLock);
}

static gdk_return
BBPinithash(int j, bat size)
{
	assert(j >= 0 && j <= BBP_THREADMASK);
	for (BBP_mask = 1; (BBP_mask << 1) <= BBPlimit; BBP_mask <<= 1)
		;
	BBP_hash = (bat *) GDKzalloc(BBP_mask * sizeof(bat));
	if (BBP_hash == NULL) {
		return GDK_FAIL;
	}
	BBP_mask--;

	while (--size > 0) {
		const char *s = BBP_logical(size);

		if (s) {
			if (*s != '.' && !BBPtmpcheck(s)) {
				BBP_insert(size);
			}
		} else {
			BBP_next(size) = BBP_free(j);
			BBP_free(j) = size;
			if (++j > BBP_THREADMASK)
				j = 0;
		}
	}
	return GDK_SUCCEED;
}

int
BBPselectfarm(role_t role, int type, enum heaptype hptype)
{
	int i;

	(void) type;		/* may use in future */
	(void) hptype;		/* may use in future */

	if (GDKinmemory(0))
		return 0;

#ifndef PERSISTENTHASH
	if (hptype == hashheap)
		role = TRANSIENT;
#endif
#ifndef PERSISTENTIDX
	if (hptype == orderidxheap)
		role = TRANSIENT;
#endif
	for (i = 0; i < MAXFARMS; i++)
		if (BBPfarms[i].roles & (1U << (int) role))
			return i;
	/* must be able to find farms for TRANSIENT and PERSISTENT */
	assert(role != TRANSIENT && role != PERSISTENT);
	return -1;
}

static gdk_return
BBPextend(int idx, bool buildhash, bat newsize)
{
	if (newsize >= N_BBPINIT * BBPINIT) {
		GDKerror("trying to extend BAT pool beyond the "
			 "limit (%d)\n", N_BBPINIT * BBPINIT);
		return GDK_FAIL;
	}

	/* make sure the new size is at least BBPsize large */
	while (BBPlimit < newsize) {
		BUN limit = BBPlimit >> BBPINITLOG;
		assert(BBP[limit] == NULL);
		BBP[limit] = GDKzalloc(BBPINIT * sizeof(BBPrec));
		if (BBP[limit] == NULL) {
			GDKerror("failed to extend BAT pool\n");
			return GDK_FAIL;
		}
		for (BUN i = 0; i < BBPINIT; i++) {
			ATOMIC_INIT(&BBP[limit][i].status, 0);
			BBP[limit][i].pid = ~(MT_Id)0;
		}
		BBPlimit += BBPINIT;
	}

	if (buildhash) {
		int i;

		GDKfree(BBP_hash);
		BBP_hash = NULL;
		for (i = 0; i <= BBP_THREADMASK; i++)
			BBP_free(i) = 0;
		if (BBPinithash(idx, newsize) != GDK_SUCCEED)
			return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

static gdk_return
recover_dir(int farmid, bool direxists)
{
	if (direxists) {
		/* just try; don't care about these non-vital files */
		if (GDKunlink(farmid, BATDIR, "BBP", "bak") != GDK_SUCCEED)
			TRC_WARNING(GDK, "unlink of BBP.bak failed\n");
		if (GDKmove(farmid, BATDIR, "BBP", "dir", BATDIR, "BBP", "bak", false) != GDK_SUCCEED)
			TRC_WARNING(GDK, "rename of BBP.dir to BBP.bak failed\n");
	}
	return GDKmove(farmid, BAKDIR, "BBP", "dir", BATDIR, "BBP", "dir", true);
}

static gdk_return BBPrecover(int farmid);
static gdk_return BBPrecover_subdir(void);
static bool BBPdiskscan(const char *, size_t);

static int
heapinit(BAT *b, const char *buf, int *hashash, unsigned bbpversion, const char *filename, int lineno)
{
	int t;
	char type[33];
	uint16_t width;
	uint16_t var;
	uint16_t properties;
	uint64_t nokey0;
	uint64_t nokey1;
	uint64_t nosorted;
	uint64_t norevsorted;
	uint64_t base;
	uint64_t free;
	uint64_t size;
	uint16_t storage;
	uint64_t minpos, maxpos;
	int n;

	(void) bbpversion;	/* could be used to implement compatibility */

	minpos = maxpos = (uint64_t) oid_nil; /* for GDKLIBRARY_MINMAX_POS case */
	if (bbpversion <= GDKLIBRARY_MINMAX_POS ?
	    sscanf(buf,
		   " %10s %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu64
		   " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
		   " %" SCNu64 " %" SCNu64 " %" SCNu16
		   "%n",
		   type, &width, &var, &properties, &nokey0,
		   &nokey1, &nosorted, &norevsorted, &base,
		   &free, &size, &storage,
		   &n) < 12 :
	    sscanf(buf,
		   " %10s %" SCNu16 " %" SCNu16 " %" SCNu16 " %" SCNu64
		   " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
		   " %" SCNu64 " %" SCNu64 " %" SCNu16 " %" SCNu64 " %" SCNu64
		   "%n",
		   type, &width, &var, &properties, &nokey0,
		   &nokey1, &nosorted, &norevsorted, &base,
		   &free, &size, &storage, &minpos, &maxpos,
		   &n) < 14) {
		TRC_CRITICAL(GDK, "invalid format for BBP.dir on line %d", lineno);
		return -1;
	}

	if (properties & ~0x0F81) {
		TRC_CRITICAL(GDK, "unknown properties are set: incompatible database on line %d of BBP.dir\n", lineno);
		return -1;
	}
	*hashash = var & 2;
	var &= ~2;
#ifdef HAVE_HGE
	if (strcmp(type, "hge") == 0)
		havehge = true;
#endif
	if ((t = ATOMindex(type)) < 0) {
		if ((t = ATOMunknown_find(type)) == 0) {
			TRC_CRITICAL(GDK, "no space for atom %s", type);
			return -1;
		}
	} else if (var != (t == TYPE_void || BATatoms[t].atomPut != NULL)) {
		TRC_CRITICAL(GDK, "inconsistent entry in BBP.dir: tvarsized mismatch for BAT %d on line %d\n", (int) b->batCacheid, lineno);
		return -1;
	} else if (var && t != 0 ?
		   ATOMsize(t) < width ||
		   (width != 1 && width != 2 && width != 4
#if SIZEOF_VAR_T == 8
		    && width != 8
#endif
			   ) :
		   ATOMsize(t) != width) {
		TRC_CRITICAL(GDK, "inconsistent entry in BBP.dir: tsize mismatch for BAT %d on line %d\n", (int) b->batCacheid, lineno);
		return -1;
	}
	b->ttype = t;
	b->twidth = width;
	b->tvarsized = var != 0;
	b->tshift = ATOMelmshift(width);
	assert_shift_width(b->tshift,b->twidth);
	b->tnokey[0] = (BUN) nokey0;
	b->tnokey[1] = (BUN) nokey1;
	b->tsorted = (bit) ((properties & 0x0001) != 0);
	b->trevsorted = (bit) ((properties & 0x0080) != 0);
	b->tkey = (properties & 0x0100) != 0;
	b->tnonil = (properties & 0x0400) != 0;
	b->tnil = (properties & 0x0800) != 0;
	b->tnosorted = (BUN) nosorted;
	b->tnorevsorted = (BUN) norevsorted;
	/* (properties & 0x0200) is the old tdense flag */
	b->tseqbase = (properties & 0x0200) == 0 || base >= (uint64_t) oid_nil ? oid_nil : (oid) base;
	b->theap->free = (size_t) free;
	/* set heap size to match capacity */
	if (b->ttype == TYPE_msk) {
		/* round up capacity to multiple of 32 */
		b->batCapacity = (b->batCapacity + 31) & ~((BUN) 31);
		b->theap->size = b->batCapacity / 8;
	} else {
		b->theap->size = (size_t) b->batCapacity << b->tshift;
	}
	b->theap->base = NULL;
	settailname(b->theap, filename, t, width);
	b->theap->storage = STORE_INVALID;
	b->theap->newstorage = STORE_INVALID;
	b->theap->farmid = BBPselectfarm(PERSISTENT, b->ttype, offheap);
	b->theap->dirty = false;
	b->theap->parentid = b->batCacheid;
	if (minpos < b->batCount)
		BATsetprop_nolock(b, GDK_MIN_POS, TYPE_oid, &(oid){(oid)minpos});
	if (maxpos < b->batCount)
		BATsetprop_nolock(b, GDK_MAX_POS, TYPE_oid, &(oid){(oid)maxpos});
	return n;
}

static int
vheapinit(BAT *b, const char *buf, int hashash, const char *filename, int lineno)
{
	int n = 0;
	uint64_t free, size;
	uint16_t storage;

	if (b->tvarsized && b->ttype != TYPE_void) {
		if (sscanf(buf,
			   " %" SCNu64 " %" SCNu64 " %" SCNu16
			   "%n",
			   &free, &size, &storage, &n) < 3) {
			TRC_CRITICAL(GDK, "invalid format for BBP.dir on line %d", lineno);
			return -1;
		}
		if (b->batCount == 0)
			free = 0;
		if (b->ttype >= 0 &&
		    ATOMstorage(b->ttype) == TYPE_str &&
		    free < GDK_STRHASHTABLE * sizeof(stridx_t) + BATTINY * GDK_VARALIGN)
			size = GDK_STRHASHTABLE * sizeof(stridx_t) + BATTINY * GDK_VARALIGN;
		else if (free < 512)
			size = 512;
		else
			size = free;
		*b->tvheap = (Heap) {
			.free = (size_t) free,
			.size = (size_t) size,
			.base = NULL,
			.storage = STORE_INVALID,
			.hashash = hashash != 0,
			.cleanhash = true,
			.newstorage = STORE_INVALID,
			.dirty = false,
			.parentid = b->batCacheid,
			.farmid = BBPselectfarm(PERSISTENT, b->ttype, varheap),
		};
		strconcat_len(b->tvheap->filename, sizeof(b->tvheap->filename),
			      filename, ".theap", NULL);
	} else {
		b->tvheap = NULL;
	}
	return n;
}

/* read a single line from the BBP.dir file (file pointer fp) and fill
 * in the structure pointed to by bn and extra information through the
 * other pointers; this function does not allocate any memory; return 0
 * on end of file, 1 on success, and -1 on failure */
static int
BBPreadBBPline(FILE *fp, unsigned bbpversion, int *lineno, BAT *bn,
	       int *hashash,
	       char *batname, char *filename, char **options)
{
	char buf[4096];
	uint64_t batid;
	uint16_t status;
	unsigned int properties;
	int nread, n;
	char *s;
	uint64_t count, capacity = 0, base = 0;

	if (fgets(buf, sizeof(buf), fp) == NULL) {
		if (ferror(fp)) {
			TRC_CRITICAL(GDK, "error reading BBP.dir on line %d\n", *lineno);
			return -1;
		}
		return 0;	/* end of file */
	}
	(*lineno)++;
	if ((s = strchr(buf, '\r')) != NULL) {
		/* convert \r\n into just \n */
		if (s[1] != '\n') {
			TRC_CRITICAL(GDK, "invalid format for BBP.dir on line %d", *lineno);
			return -1;
		}
		*s++ = '\n';
		*s = 0;
	}

	if (sscanf(buf,
		   "%" SCNu64 " %" SCNu16 " %128s %19s %u %" SCNu64
		   " %" SCNu64 " %" SCNu64
		   "%n",
		   &batid, &status, batname, filename,
		   &properties,
		   &count, &capacity, &base,
		   &nread) < 8) {
		TRC_CRITICAL(GDK, "invalid format for BBP.dir on line %d", *lineno);
		return -1;
	}

	if (batid >= N_BBPINIT * BBPINIT) {
		TRC_CRITICAL(GDK, "bat ID (%" PRIu64 ") too large to accomodate (max %d), on line %d.", batid, N_BBPINIT * BBPINIT - 1, *lineno);
		return -1;
	}

	/* convert both / and \ path separators to our own DIR_SEP */
#if DIR_SEP != '/'
	s = filename;
	while ((s = strchr(s, '/')) != NULL)
		*s++ = DIR_SEP;
#endif
#if DIR_SEP != '\\'
	s = filename;
	while ((s = strchr(s, '\\')) != NULL)
		*s++ = DIR_SEP;
#endif

	bn->batCacheid = (bat) batid;
	BATinit_idents(bn);
	bn->batTransient = false;
	bn->batCopiedtodisk = true;
	switch ((properties & 0x06) >> 1) {
	case 0:
		bn->batRestricted = BAT_WRITE;
		break;
	case 1:
		bn->batRestricted = BAT_READ;
		break;
	case 2:
		bn->batRestricted = BAT_APPEND;
		break;
	default:
		TRC_CRITICAL(GDK, "incorrect batRestricted value");
		return -1;
	}
	bn->batCount = (BUN) count;
	bn->batInserted = bn->batCount;
	/* set capacity to at least count */
	bn->batCapacity = (BUN) count <= BATTINY ? BATTINY : (BUN) count;

	if (base > (uint64_t) GDK_oid_max) {
		TRC_CRITICAL(GDK, "head seqbase out of range (ID = %" PRIu64 ", seq = %" PRIu64 ") on line %d.", batid, base, *lineno);
		return -1;
	}
	bn->hseqbase = (oid) base;
	n = heapinit(bn, buf + nread,
		     hashash,
		     bbpversion, filename, *lineno);
	if (n < 0) {
		return -1;
	}
	nread += n;
	n = vheapinit(bn, buf + nread, *hashash, filename, *lineno);
	if (n < 0) {
		return -1;
	}
	nread += n;

	if (buf[nread] != '\n' && buf[nread] != ' ') {
		TRC_CRITICAL(GDK, "invalid format for BBP.dir on line %d", *lineno);
		return -1;
	}
	*options = (buf[nread] == ' ') ? buf + nread + 1 : NULL;
	return 1;
}

static gdk_return
BBPreadEntries(FILE *fp, unsigned bbpversion, int lineno)
{
	/* read the BBP.dir and insert the BATs into the BBP */
	for (;;) {
		BAT b;
		Heap h;
		Heap vh;
		vh = h = (Heap) {
			.free = 0,
		};
		b = (BAT) {
			.theap = &h,
			.tvheap = &vh,
		};
		char *options;
		char headname[129];
		char filename[sizeof(BBP_physical(0))];
		char logical[1024];
		int Thashash;

		switch (BBPreadBBPline(fp, bbpversion, &lineno, &b,
				       &Thashash,
				       headname, filename, &options)) {
		case 0:
			/* end of file */
			return GDK_SUCCEED;
		case 1:
			/* successfully read an entry */
			break;
		default:
			/* error */
			goto bailout;
		}

		if (b.batCacheid >= N_BBPINIT * BBPINIT) {
			TRC_CRITICAL(GDK, "bat ID (%d) too large to accommodate (max %d), on line %d.", b.batCacheid, N_BBPINIT * BBPINIT - 1, lineno);
			goto bailout;
		}

		if (b.batCacheid >= (bat) ATOMIC_GET(&BBPsize)) {
			if ((bat) ATOMIC_GET(&BBPsize) + 1 >= BBPlimit &&
			    BBPextend(0, false, b.batCacheid + 1) != GDK_SUCCEED)
				goto bailout;
			ATOMIC_SET(&BBPsize, b.batCacheid + 1);
		}
		if (BBP_desc(b.batCacheid) != NULL) {
			TRC_CRITICAL(GDK, "duplicate entry in BBP.dir (ID = "
				     "%d) on line %d.", b.batCacheid, lineno);
			goto bailout;
		}

		BAT *bn;
		Heap *hn;
		if ((bn = GDKzalloc(sizeof(BAT))) == NULL ||
		    (hn = GDKzalloc(sizeof(Heap))) == NULL) {
			GDKfree(bn);
			TRC_CRITICAL(GDK, "cannot allocate memory for BAT.");
			goto bailout;
		}
		*bn = b;
		*hn = h;
		bn->theap = hn;
		if (options &&
		    (options = GDKstrdup(options)) == NULL) {
			GDKfree(hn);
			GDKfree(bn);
			PROPdestroy_nolock(&b);
			TRC_CRITICAL(GDK, "GDKstrdup failed\n");
			goto bailout;
		}
		if (b.tvheap) {
			Heap *vhn;
			assert(b.tvheap == &vh);
			if ((vhn = GDKmalloc(sizeof(Heap))) == NULL) {
				GDKfree(hn);
				GDKfree(bn);
				GDKfree(options);
				TRC_CRITICAL(GDK, "cannot allocate memory for BAT.");
				goto bailout;
			}
			*vhn = vh;
			bn->tvheap = vhn;
			ATOMIC_INIT(&bn->tvheap->refs, 1);
		}

		char name[MT_NAME_LEN];
		snprintf(name, sizeof(name), "heaplock%d", bn->batCacheid); /* fits */
		MT_lock_init(&bn->theaplock, name);
		snprintf(name, sizeof(name), "BATlock%d", bn->batCacheid); /* fits */
		MT_lock_init(&bn->batIdxLock, name);
		snprintf(name, sizeof(name), "hashlock%d", bn->batCacheid); /* fits */
		MT_rwlock_init(&bn->thashlock, name);
		ATOMIC_INIT(&bn->theap->refs, 1);

		if (snprintf(BBP_bak(b.batCacheid), sizeof(BBP_bak(b.batCacheid)), "tmp_%o", (unsigned) b.batCacheid) >= (int) sizeof(BBP_bak(b.batCacheid))) {
			BATdestroy(bn);
			TRC_CRITICAL(GDK, "BBP logical filename directory is too large, on line %d\n", lineno);
			goto bailout;
		}
		char *s;
		if ((s = strchr(headname, '~')) != NULL && s == headname) {
			/* sizeof(logical) > sizeof(BBP_bak(b.batCacheid)), so
			 * this fits */
			strcpy(logical, BBP_bak(b.batCacheid));
		} else {
			if (s)
				*s = 0;
			strcpy_len(logical, headname, sizeof(logical));
		}
		if (strcmp(logical, BBP_bak(b.batCacheid)) == 0) {
			BBP_logical(b.batCacheid) = BBP_bak(b.batCacheid);
		} else {
			BBP_logical(b.batCacheid) = GDKstrdup(logical);
			if (BBP_logical(b.batCacheid) == NULL) {
				BATdestroy(bn);
				TRC_CRITICAL(GDK, "GDKstrdup failed\n");
				goto bailout;
			}
		}
		strcpy_len(BBP_physical(b.batCacheid), filename, sizeof(BBP_physical(b.batCacheid)));
#ifdef __COVERITY__
		/* help coverity */
		BBP_physical(b.batCacheid)[sizeof(BBP_physical(b.batCacheid)) - 1] = 0;
#endif
		BBP_options(b.batCacheid) = options;
		BBP_refs(b.batCacheid) = 0;
		BBP_lrefs(b.batCacheid) = 1;	/* any BAT we encounter here is persistent, so has a logical reference */
		BBP_desc(b.batCacheid) = bn;
		BBP_pid(b.batCacheid) = 0;
		BBP_status_set(b.batCacheid, BBPEXISTING);	/* do we need other status bits? */
	}

  bailout:
	return GDK_FAIL;
}

/* check that the necessary files for all BATs exist and are large
 * enough */
static gdk_return
BBPcheckbats(unsigned bbpversion)
{
	(void) bbpversion;
	for (bat bid = 1, size = (bat) ATOMIC_GET(&BBPsize); bid < size; bid++) {
		struct stat statb;
		BAT *b;
		char *path;

		if ((b = BBP_desc(bid)) == NULL) {
			/* not a valid BAT */
			continue;
		}
		if (b->ttype == TYPE_void) {
			/* no files needed */
			continue;
		}
		if (b->theap->free > 0) {
			path = GDKfilepath(0, BATDIR, b->theap->filename, NULL);
			if (path == NULL)
				return GDK_FAIL;
#if 1
			/* first check string offset heap with width,
			 * then without */
			if (MT_stat(path, &statb) < 0) {
#ifdef GDKLIBRARY_TAILN
				if (b->ttype == TYPE_str &&
				    b->twidth < SIZEOF_VAR_T) {
					size_t taillen = strlen(path) - 1;
					char tailsave = path[taillen];
					path[taillen] = 0;
					if (MT_stat(path, &statb) < 0) {
						GDKsyserror("cannot stat file %s%c or %s (expected size %zu)\n",
							    path, tailsave, path, b->theap->free);
						GDKfree(path);
						return GDK_FAIL;
					}
				} else
#endif
				{
					GDKsyserror("cannot stat file %s (expected size %zu)\n",
						    path, b->theap->free);
					GDKfree(path);
					return GDK_FAIL;
				}
			}
#else
			/* first check string offset heap without width,
			 * then with */
#ifdef GDKLIBRARY_TAILN
			/* if bbpversion > GDKLIBRARY_TAILN, the offset heap can
			 * exist with either name .tail1 (etc) or .tail, if <=
			 * GDKLIBRARY_TAILN, only with .tail */
			char tailsave = 0;
			size_t taillen = 0;
			if (b->ttype == TYPE_str &&
			    b->twidth < SIZEOF_VAR_T) {
				/* old version: .tail, not .tail1, .tail2, .tail4 */
				taillen = strlen(path) - 1;
				tailsave = path[taillen];
				path[taillen] = 0;
			}
#endif
			if (MT_stat(path, &statb) < 0
#ifdef GDKLIBRARY_TAILN
			    && bbpversion > GDKLIBRARY_TAILN
			    && b->ttype == TYPE_str
			    && b->twidth < SIZEOF_VAR_T
			    && (path[taillen] = tailsave) != 0
			    && MT_stat(path, &statb) < 0
#endif
				) {

				GDKsyserror("cannot stat file %s (expected size %zu)\n",
					    path, b->theap->free);
				GDKfree(path);
				return GDK_FAIL;
			}
#endif
			if ((size_t) statb.st_size < b->theap->free) {
				GDKerror("file %s too small (expected %zu, actual %zu)\n", path, b->theap->free, (size_t) statb.st_size);
				GDKfree(path);
				return GDK_FAIL;
			}
			GDKfree(path);
		}
		if (b->tvheap != NULL && b->tvheap->free > 0) {
			path = GDKfilepath(0, BATDIR, BBP_physical(b->batCacheid), "theap");
			if (path == NULL)
				return GDK_FAIL;
			if (MT_stat(path, &statb) < 0) {
				GDKsyserror("cannot stat file %s\n",
					    path);
				GDKfree(path);
				return GDK_FAIL;
			}
			if ((size_t) statb.st_size < b->tvheap->free) {
				GDKerror("file %s too small (expected %zu, actual %zu)\n", path, b->tvheap->free, (size_t) statb.st_size);
				GDKfree(path);
				return GDK_FAIL;
			}
			GDKfree(path);
		}
	}
	return GDK_SUCCEED;
}

#ifdef HAVE_HGE
#define SIZEOF_MAX_INT SIZEOF_HGE
#else
#define SIZEOF_MAX_INT SIZEOF_LNG
#endif

static unsigned
BBPheader(FILE *fp, int *lineno, bat *bbpsize, lng *logno, lng *transid)
{
	char buf[BUFSIZ];
	int sz, ptrsize, oidsize, intsize;
	unsigned bbpversion;

	if (fgets(buf, sizeof(buf), fp) == NULL) {
		TRC_CRITICAL(GDK, "BBP.dir is empty");
		return 0;
	}
	++*lineno;
	if (sscanf(buf, "BBP.dir, GDKversion %u\n", &bbpversion) != 1) {
		GDKerror("old BBP without version number; "
			 "dump the database using a compatible version, "
			 "then restore into new database using this version.\n");
		return 0;
	}
	if (bbpversion != GDKLIBRARY &&
	    bbpversion != GDKLIBRARY_TAILN &&
	    bbpversion != GDKLIBRARY_MINMAX_POS) {
		TRC_CRITICAL(GDK, "incompatible BBP version: expected 0%o, got 0%o. "
			     "This database was probably created by a %s version of MonetDB.",
			     GDKLIBRARY, bbpversion,
			     bbpversion > GDKLIBRARY ? "newer" : "too old");
		return 0;
	}
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		TRC_CRITICAL(GDK, "short BBP");
		return 0;
	}
	++*lineno;
	if (sscanf(buf, "%d %d %d", &ptrsize, &oidsize, &intsize) != 3) {
		TRC_CRITICAL(GDK, "BBP.dir has incompatible format: pointer, OID, and max. integer sizes are missing on line %d", *lineno);
		return 0;
	}
	if (ptrsize != SIZEOF_SIZE_T || oidsize != SIZEOF_OID) {
		TRC_CRITICAL(GDK, "database created with incompatible server: "
			     "expected pointer size %d, got %d, expected OID size %d, got %d.",
			     SIZEOF_SIZE_T, ptrsize, SIZEOF_OID, oidsize);
		return 0;
	}
	if (intsize > SIZEOF_MAX_INT) {
		TRC_CRITICAL(GDK, "database created with incompatible server: "
			     "expected max. integer size %d, got %d.",
			     SIZEOF_MAX_INT, intsize);
		return 0;
	}
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		TRC_CRITICAL(GDK, "short BBP");
		return 0;
	}
	++*lineno;
	if (sscanf(buf, "BBPsize=%d", &sz) != 1) {
		TRC_CRITICAL(GDK, "no BBPsize value found\n");
		return 0;
	}
	if (sz > *bbpsize)
		*bbpsize = sz;
	if (bbpversion > GDKLIBRARY_MINMAX_POS) {
		if (fgets(buf, sizeof(buf), fp) == NULL) {
			TRC_CRITICAL(GDK, "short BBP");
			return 0;
		}
		if (sscanf(buf, "BBPinfo=" LLSCN " " LLSCN, logno, transid) != 2) {
			TRC_CRITICAL(GDK, "no info value found\n");
			return 0;
		}
	} else {
		*logno = *transid = 0;
	}
	return bbpversion;
}

bool
GDKinmemory(int farmid)
{
	if (farmid == NOFARM)
		farmid = 0;
	assert(farmid >= 0 && farmid < MAXFARMS);
	return BBPfarms[farmid].dirname == NULL;
}

/* all errors are fatal */
gdk_return
BBPaddfarm(const char *dirname, uint32_t rolemask, bool logerror)
{
	struct stat st;
	int i;

	if (dirname == NULL) {
		assert(BBPfarms[0].dirname == NULL);
		assert(rolemask & 1);
		assert(BBPfarms[0].roles == 0);
		BBPfarms[0].roles = rolemask;
		return GDK_SUCCEED;
	}
	if (strchr(dirname, '\n') != NULL) {
		if (logerror)
			GDKerror("no newline allowed in directory name\n");
		return GDK_FAIL;
	}
	if (rolemask == 0 || (rolemask & 1 && BBPfarms[0].dirname != NULL)) {
		if (logerror)
			GDKerror("bad rolemask\n");
		return GDK_FAIL;
	}
	if (strcmp(dirname, "in-memory") == 0 ||
	    /* backward compatibility: */ strcmp(dirname, ":memory:") == 0) {
		dirname = NULL;
	} else if (MT_mkdir(dirname) < 0) {
		if (errno == EEXIST) {
			if (MT_stat(dirname, &st) == -1 || !S_ISDIR(st.st_mode)) {
				if (logerror)
					GDKerror("%s: not a directory\n", dirname);
				return GDK_FAIL;
			}
		} else {
			if (logerror)
				GDKsyserror("%s: cannot create directory\n", dirname);
			return GDK_FAIL;
		}
	}
	for (i = 0; i < MAXFARMS; i++) {
		if (BBPfarms[i].roles == 0) {
			if (dirname) {
				BBPfarms[i].dirname = GDKstrdup(dirname);
				if (BBPfarms[i].dirname == NULL)
					return GDK_FAIL;
			}
			BBPfarms[i].roles = rolemask;
			if ((rolemask & 1) == 0 && dirname != NULL) {
				char *bbpdir;
				int j;

				for (j = 0; j < i; j++)
					if (BBPfarms[j].dirname != NULL &&
					    strcmp(BBPfarms[i].dirname,
						   BBPfarms[j].dirname) == 0)
						return GDK_SUCCEED;
				/* if an extra farm, make sure we
				 * don't find a BBP.dir there that
				 * might belong to an existing
				 * database */
				bbpdir = GDKfilepath(i, BATDIR, "BBP", "dir");
				if (bbpdir == NULL) {
					return GDK_FAIL;
				}
				if (MT_stat(bbpdir, &st) != -1 || errno != ENOENT) {
					GDKfree(bbpdir);
					if (logerror)
						GDKerror("%s is a database\n", dirname);
					return GDK_FAIL;
				}
				GDKfree(bbpdir);
				bbpdir = GDKfilepath(i, BAKDIR, "BBP", "dir");
				if (bbpdir == NULL) {
					return GDK_FAIL;
				}
				if (MT_stat(bbpdir, &st) != -1 || errno != ENOENT) {
					GDKfree(bbpdir);
					if (logerror)
						GDKerror("%s is a database\n", dirname);
					return GDK_FAIL;
				}
				GDKfree(bbpdir);
			}
			return GDK_SUCCEED;
		}
	}
	if (logerror)
		GDKerror("too many farms\n");
	return GDK_FAIL;
}

#ifdef GDKLIBRARY_TAILN
static gdk_return
movestrbats(void)
{
	for (bat bid = 1, nbat = (bat) ATOMIC_GET(&BBPsize); bid < nbat; bid++) {
		BAT *b = BBP_desc(bid);
		if (b == NULL) {
			/* not a valid BAT */
			continue;
		}
		if (b->ttype != TYPE_str || b->twidth == SIZEOF_VAR_T || b->batCount == 0)
			continue;
		char *oldpath = GDKfilepath(0, BATDIR, BBP_physical(b->batCacheid), "tail");
		char *newpath = GDKfilepath(0, BATDIR, b->theap->filename, NULL);
		int ret = -1;
		if (oldpath != NULL && newpath != NULL) {
			struct stat oldst, newst;
			bool oldexist = MT_stat(oldpath, &oldst) == 0;
			bool newexist = MT_stat(newpath, &newst) == 0;
			if (newexist) {
				if (oldexist) {
					if (oldst.st_mtime > newst.st_mtime) {
						GDKerror("both %s and %s exist with %s unexpectedly newer: manual intervention required\n", oldpath, newpath, oldpath);
						ret = -1;
					} else {
						TRC_WARNING(GDK, "both %s and %s exist, removing %s\n", oldpath, newpath, oldpath);
						ret = MT_remove(oldpath);
					}
				} else {
					/* already good */
					ret = 0;
				}
			} else if (oldexist) {
				TRC_DEBUG(IO_, "rename %s to %s\n", oldpath, newpath);
				ret = MT_rename(oldpath, newpath);
			} else {
				/* neither file exists: may be ok, but
				 * will be checked later */
				ret = 0;
			}
		}
		GDKfree(oldpath);
		GDKfree(newpath);
		if (ret == -1)
			return GDK_FAIL;
	}
	return GDK_SUCCEED;
}
#endif

static void
BBPtrim(bool aggressive)
{
	int n = 0;
	unsigned flag = BBPUNLOADING | BBPSYNCING | BBPSAVING;
	if (!aggressive)
		flag |= BBPHOT;
	for (bat bid = 1, nbat = (bat) ATOMIC_GET(&BBPsize); bid < nbat; bid++) {
		/* don't do this during a (sub)commit */
		MT_lock_set(&GDKtmLock);
		MT_lock_set(&GDKswapLock(bid));
		BAT *b = NULL;
		bool swap = false;
		if (!(BBP_status(bid) & flag) &&
		    BBP_refs(bid) == 0 &&
		    BBP_lrefs(bid) != 0 &&
		    (b = BBP_cache(bid)) != NULL) {
			MT_lock_set(&b->theaplock);
			if (b->batSharecnt == 0 &&
			    !isVIEW(b) &&
			    (!BATdirty(b) || (aggressive && b->theap->storage == STORE_MMAP && (b->tvheap == NULL || b->tvheap->storage == STORE_MMAP))) /*&&
			    (BBP_status(bid) & BBPPERSISTENT ||
			     (b->batRole == PERSISTENT && BBP_lrefs(bid) == 1)) */) {
				BBP_status_on(bid, BBPUNLOADING);
				swap = true;
			}
			MT_lock_unset(&b->theaplock);
		}
		MT_lock_unset(&GDKswapLock(bid));
		if (swap) {
			TRC_DEBUG(BAT_, "unload and free bat %d\n", bid);
			if (BBPfree(b) != GDK_SUCCEED)
				GDKerror("unload failed for bat %d", bid);
			n++;
		}
		MT_lock_unset(&GDKtmLock);
	}
	TRC_DEBUG(BAT_, "unloaded %d bats%s\n", n, aggressive ? " (also hot)" : "");
}

static void
BBPmanager(void *dummy)
{
	(void) dummy;

	for (;;) {
		int n = 0;
		for (bat bid = 1, nbat = (bat) ATOMIC_GET(&BBPsize); bid < nbat; bid++) {
			MT_lock_set(&GDKswapLock(bid));
			if (BBP_refs(bid) == 0 && BBP_lrefs(bid) != 0) {
				n += (BBP_status(bid) & BBPHOT) != 0;
				BBP_status_off(bid, BBPHOT);
			}
			MT_lock_unset(&GDKswapLock(bid));
		}
		TRC_DEBUG(BAT_, "cleared HOT bit from %d bats\n", n);
		size_t cur = GDKvm_cursize();
		for (int i = 0, n = cur > GDK_vm_maxsize / 2 ? 1 : cur > GDK_vm_maxsize / 4 ? 10 : 100; i < n; i++) {
			MT_sleep_ms(100);
			if (GDKexiting())
				return;
		}
		BBPtrim(false);
		if (GDKexiting())
			return;
	}
}

static MT_Id manager;

gdk_return
BBPinit(bool first)
{
	FILE *fp = NULL;
	struct stat st;
	unsigned bbpversion = 0;
	int i;
	int lineno = 0;
	int dbg = GDKdebug;

	GDKdebug &= ~TAILCHKMASK;

	/* the maximum number of BATs allowed in the system and the
	 * size of the "physical" array are linked in a complicated
	 * manner.  The expression below shows the relationship */
	static_assert((uint64_t) N_BBPINIT * BBPINIT < (UINT64_C(1) << (3 * ((sizeof(BBP[0][0].physical) + 2) * 2 / 5))), "\"physical\" array in BBPrec is too small");
	/* similarly, the maximum number of BATs allowed also has a
	 * (somewhat simpler) relation with the size of the "bak"
	 * array */
	static_assert((uint64_t) N_BBPINIT * BBPINIT < (UINT64_C(1) << (3 * (sizeof(BBP[0][0].bak) - 5))), "\"bak\" array in BBPrec is too small");

	if (first) {
		for (i = 0; i <= BBP_THREADMASK; i++) {
			char name[MT_NAME_LEN];
			snprintf(name, sizeof(name), "GDKcacheLock%d", i);
			MT_lock_init(&GDKbbpLock[i].cache, name);
			GDKbbpLock[i].free = 0;
		}
	}
	if (!GDKinmemory(0)) {
		str bbpdirstr, backupbbpdirstr;

		MT_lock_set(&GDKtmLock);

		if (!(bbpdirstr = GDKfilepath(0, BATDIR, "BBP", "dir"))) {
			TRC_CRITICAL(GDK, "GDKmalloc failed\n");
			MT_lock_unset(&GDKtmLock);
			GDKdebug = dbg;
			return GDK_FAIL;
		}

		if (!(backupbbpdirstr = GDKfilepath(0, BAKDIR, "BBP", "dir"))) {
			GDKfree(bbpdirstr);
			TRC_CRITICAL(GDK, "GDKmalloc failed\n");
			MT_lock_unset(&GDKtmLock);
			GDKdebug = dbg;
			return GDK_FAIL;
		}

		if (GDKremovedir(0, TEMPDIR) != GDK_SUCCEED) {
			GDKfree(bbpdirstr);
			GDKfree(backupbbpdirstr);
			TRC_CRITICAL(GDK, "cannot remove directory %s\n", TEMPDIR);
			MT_lock_unset(&GDKtmLock);
			GDKdebug = dbg;
			return GDK_FAIL;
		}

		if (GDKremovedir(0, DELDIR) != GDK_SUCCEED) {
			GDKfree(bbpdirstr);
			GDKfree(backupbbpdirstr);
			TRC_CRITICAL(GDK, "cannot remove directory %s\n", DELDIR);
			MT_lock_unset(&GDKtmLock);
			GDKdebug = dbg;
			return GDK_FAIL;
		}

		/* first move everything from SUBDIR to BAKDIR (its parent) */
		if (BBPrecover_subdir() != GDK_SUCCEED) {
			GDKfree(bbpdirstr);
			GDKfree(backupbbpdirstr);
			TRC_CRITICAL(GDK, "cannot properly recover_subdir process %s.", SUBDIR);
			MT_lock_unset(&GDKtmLock);
			GDKdebug = dbg;
			return GDK_FAIL;
		}

		/* try to obtain a BBP.dir from bakdir */
		if (MT_stat(backupbbpdirstr, &st) == 0) {
			/* backup exists; *must* use it */
			if (recover_dir(0, MT_stat(bbpdirstr, &st) == 0) != GDK_SUCCEED) {
				GDKfree(bbpdirstr);
				GDKfree(backupbbpdirstr);
				MT_lock_unset(&GDKtmLock);
				goto bailout;
			}
			if ((fp = GDKfilelocate(0, "BBP", "r", "dir")) == NULL) {
				GDKfree(bbpdirstr);
				GDKfree(backupbbpdirstr);
				TRC_CRITICAL(GDK, "cannot open recovered BBP.dir.");
				MT_lock_unset(&GDKtmLock);
				GDKdebug = dbg;
				return GDK_FAIL;
			}
		} else if ((fp = GDKfilelocate(0, "BBP", "r", "dir")) == NULL) {
			/* there was no BBP.dir either. Panic! try to use a
			 * BBP.bak */
			if (MT_stat(backupbbpdirstr, &st) < 0) {
				/* no BBP.bak (nor BBP.dir or BACKUP/BBP.dir):
				 * create a new one */
				TRC_DEBUG(IO_, "initializing BBP.\n");
				if (BBPdir_init() != GDK_SUCCEED) {
					GDKfree(bbpdirstr);
					GDKfree(backupbbpdirstr);
					MT_lock_unset(&GDKtmLock);
					goto bailout;
				}
			} else if (GDKmove(0, BATDIR, "BBP", "bak", BATDIR, "BBP", "dir", true) == GDK_SUCCEED)
				TRC_DEBUG(IO_, "reverting to dir saved in BBP.bak.\n");

			if ((fp = GDKfilelocate(0, "BBP", "r", "dir")) == NULL) {
				GDKsyserror("cannot open BBP.dir");
				GDKfree(bbpdirstr);
				GDKfree(backupbbpdirstr);
				MT_lock_unset(&GDKtmLock);
				goto bailout;
			}
		}
		assert(fp != NULL);
		GDKfree(bbpdirstr);
		GDKfree(backupbbpdirstr);
		MT_lock_unset(&GDKtmLock);
	}

	/* scan the BBP.dir to obtain current size */
	BBPlimit = 0;
	memset(BBP, 0, sizeof(BBP));

	bat bbpsize;
	bbpsize = 1;
	if (GDKinmemory(0)) {
		bbpversion = GDKLIBRARY;
	} else {
		lng logno, transid;
		bbpversion = BBPheader(fp, &lineno, &bbpsize, &logno, &transid);
		if (bbpversion == 0) {
			GDKdebug = dbg;
			return GDK_FAIL;
		}
		assert(bbpversion > GDKLIBRARY_MINMAX_POS || logno == 0);
		assert(bbpversion > GDKLIBRARY_MINMAX_POS || transid == 0);
		ATOMIC_SET(&BBPlogno, logno);
		ATOMIC_SET(&BBPtransid, transid);
	}

	/* allocate BBP records */
	if (BBPextend(0, false, bbpsize) != GDK_SUCCEED) {
		GDKdebug = dbg;
		return GDK_FAIL;
	}
	ATOMIC_SET(&BBPsize, bbpsize);

	if (!GDKinmemory(0)) {
		if (BBPreadEntries(fp, bbpversion, lineno) != GDK_SUCCEED) {
			GDKdebug = dbg;
			return GDK_FAIL;
		}
		fclose(fp);
	}

	MT_lock_set(&BBPnameLock);
	if (BBPinithash(0, (bat) ATOMIC_GET(&BBPsize)) != GDK_SUCCEED) {
		TRC_CRITICAL(GDK, "BBPinithash failed");
		MT_lock_unset(&BBPnameLock);
		GDKdebug = dbg;
		return GDK_FAIL;
	}
	MT_lock_unset(&BBPnameLock);

	/* will call BBPrecover if needed */
	if (!GDKinmemory(0)) {
		MT_lock_set(&GDKtmLock);
		gdk_return rc = BBPprepare(false);
		MT_lock_unset(&GDKtmLock);
		if (rc != GDK_SUCCEED) {
			TRC_CRITICAL(GDK, "cannot properly prepare process %s.", BAKDIR);
			GDKdebug = dbg;
			return rc;
		}
	}

	if (BBPcheckbats(bbpversion) != GDK_SUCCEED) {
		GDKdebug = dbg;
		return GDK_FAIL;
	}

#ifdef GDKLIBRARY_TAILN
	char *needstrbatmove;
	if (GDKinmemory(0)) {
		needstrbatmove = NULL;
	} else {
		needstrbatmove = GDKfilepath(0, BATDIR, "needstrbatmove", NULL);
		if (bbpversion <= GDKLIBRARY_TAILN) {
			/* create signal file that we need to rename string
			 * offset heaps */
			int fd = MT_open(needstrbatmove, O_WRONLY | O_CREAT);
			if (fd < 0) {
				TRC_CRITICAL(GDK, "cannot create signal file needstrbatmove.\n");
				GDKfree(needstrbatmove);
				GDKdebug = dbg;
				return GDK_FAIL;
			}
			close(fd);
		} else {
			/* check signal file whether we need to rename string
			 * offset heaps */
			int fd = MT_open(needstrbatmove, O_RDONLY);
			if (fd >= 0) {
				/* yes, we do */
				close(fd);
			} else if (errno == ENOENT) {
				/* no, we don't: set var to NULL */
				GDKfree(needstrbatmove);
				needstrbatmove = NULL;
			} else {
				GDKsyserror("unexpected error opening %s\n", needstrbatmove);
				GDKfree(needstrbatmove);
				GDKdebug = dbg;
				return GDK_FAIL;
			}
		}
	}
#endif

	if (bbpversion < GDKLIBRARY && TMcommit() != GDK_SUCCEED) {
		TRC_CRITICAL(GDK, "TMcommit failed\n");
		GDKdebug = dbg;
		return GDK_FAIL;
	}

#ifdef GDKLIBRARY_TAILN
	/* we rename the offset heaps after the above commit: in this
	 * version we accept both the old and new names, but we want to
	 * convert so that future versions only have the new name */
	if (needstrbatmove) {
		/* note, if renaming fails, nothing is lost: a next
		 * invocation will just try again; an older version of
		 * mserver will not work because of the TMcommit
		 * above */
		if (movestrbats() != GDK_SUCCEED) {
			GDKfree(needstrbatmove);
			GDKdebug = dbg;
			return GDK_FAIL;
		}
		MT_remove(needstrbatmove);
		GDKfree(needstrbatmove);
		needstrbatmove = NULL;
	}
#endif
	GDKdebug = dbg;

	/* cleanup any leftovers (must be done after BBPrecover) */
	for (i = 0; i < MAXFARMS && BBPfarms[i].dirname != NULL; i++) {
		int j;
		for (j = 0; j < i; j++) {
			/* don't clean a directory twice */
			if (BBPfarms[j].dirname &&
			    strcmp(BBPfarms[i].dirname,
				   BBPfarms[j].dirname) == 0)
				break;
		}
		if (j == i) {
			char *d = GDKfilepath(i, NULL, BATDIR, NULL);
			if (d == NULL) {
				return GDK_FAIL;
			}
			BBPdiskscan(d, strlen(d) - strlen(BATDIR));
			GDKfree(d);
		}
	}

	manager = THRcreate(BBPmanager, NULL, MT_THR_DETACHED, "BBPmanager");
	return GDK_SUCCEED;

  bailout:
	/* now it is time for real panic */
	TRC_CRITICAL(GDK, "could not write %s%cBBP.dir.", BATDIR, DIR_SEP);
	return GDK_FAIL;
}

/*
 * During the exit phase all non-persistent BATs are removed.  Upon
 * exit the status of the BBP tables is saved on disk.  This function
 * is called once and during the shutdown of the server. Since
 * shutdown may be issued from any thread (dangerous) it may lead to
 * interference in a parallel session.
 */

static int backup_files = 0, backup_dir = 0, backup_subdir = 0;

void
BBPexit(void)
{
	bat i;
	bool skipped;

	BBPlock();	/* stop all threads ever touching more descriptors */

	/* free all memory (just for leak-checking in Purify) */
	do {
		skipped = false;
		for (i = 0; i < (bat) ATOMIC_GET(&BBPsize); i++) {
			if (BBPvalid(i)) {
				BAT *b = BBP_desc(i);

				if (b) {
					if (b->batSharecnt > 0) {
						skipped = true;
						continue;
					}
					if (isVIEW(b)) {
						/* "manually"
						 * decrement parent
						 * references, since
						 * VIEWdestroy doesn't
						 * (and can't here due
						 * to locks) do it */
						bat tp = VIEWtparent(b);
						bat vtp = VIEWvtparent(b);
						if (tp) {
							BBP_desc(tp)->batSharecnt--;
							--BBP_lrefs(tp);
						}
						if (vtp) {
							BBP_desc(vtp)->batSharecnt--;
							--BBP_lrefs(vtp);
						}
						VIEWdestroy(b);
					} else {
						PROPdestroy_nolock(b);
						BATfree(b);
					}
				}
				BBP_pid(i) = 0;
				BBPuncacheit(i, true);
				if (BBP_logical(i) != BBP_bak(i))
					GDKfree(BBP_logical(i));
				BBP_logical(i) = NULL;
			}
		}
	} while (skipped);
	GDKfree(BBP_hash);
	BBP_hash = NULL;
	// these need to be NULL, otherwise no new ones get created
	backup_files = 0;
	backup_dir = 0;
	backup_subdir = 0;

}

/*
 * The routine BBPdir creates the BAT pool dictionary file.  It
 * includes some information about the current state of affair in the
 * pool.  The location in the buffer pool is saved for later use as
 * well.  This is merely done for ease of debugging and of no
 * importance to front-ends.  The tail of non-used entries is
 * reclaimed as well.
 */
static inline int
heap_entry(FILE *fp, BATiter *bi, BUN size, oid minpos, oid maxpos)
{
	BAT *b = bi->b;
	size_t free = bi->hfree;
	if (size < BUN_NONE) {
		if ((bi->type >= 0 && ATOMstorage(bi->type) == TYPE_msk))
			free = ((size + 31) / 32) * 4;
		else if (bi->width > 0)
			free = size << bi->shift;
		else
			free = 0;
	}

	return fprintf(fp, " %s %d %d %d " BUNFMT " " BUNFMT " " BUNFMT " "
		       BUNFMT " " OIDFMT " %zu %zu %d " OIDFMT " " OIDFMT,
		       bi->type >= 0 ? BATatoms[bi->type].name : ATOMunknown_name(bi->type),
		       bi->width,
		       b->tvarsized | (bi->vh ? bi->vh->hashash << 1 : 0),
		       (unsigned short) b->tsorted |
			   ((unsigned short) b->trevsorted << 7) |
			   (((unsigned short) b->tkey & 0x01) << 8) |
		           ((unsigned short) BATtdense(b) << 9) |
			   ((unsigned short) b->tnonil << 10) |
			   ((unsigned short) b->tnil << 11),
		       b->tnokey[0] >= size || b->tnokey[1] >= size ? 0 : b->tnokey[0],
		       b->tnokey[0] >= size || b->tnokey[1] >= size ? 0 : b->tnokey[1],
		       b->tnosorted >= size ? 0 : b->tnosorted,
		       b->tnorevsorted >= size ? 0 : b->tnorevsorted,
		       b->tseqbase,
		       free,
		       bi->h->size,
		       0,
		       (BUN) minpos < size ? minpos : oid_nil,
		       (BUN) maxpos < size ? maxpos : oid_nil);
}

static inline int
vheap_entry(FILE *fp, BATiter *bi, BUN size)
{
	(void) size;
	if (bi->vh == NULL)
		return 0;
	return fprintf(fp, " %zu %zu %d", size == 0 ? 0 : bi->vhfree, bi->vh->size, 0);
}

static gdk_return
new_bbpentry(FILE *fp, bat i, BUN size, BATiter *bi, oid minpos, oid maxpos)
{
#ifndef NDEBUG
	assert(i > 0);
	assert(i < (bat) ATOMIC_GET(&BBPsize));
	assert(bi->b);
	assert(bi->b->batCacheid == i);
	assert(bi->b->batRole == PERSISTENT);
	assert(0 <= bi->h->farmid && bi->h->farmid < MAXFARMS);
	assert(BBPfarms[bi->h->farmid].roles & (1U << PERSISTENT));
	if (bi->vh) {
		assert(0 <= bi->vh->farmid && bi->vh->farmid < MAXFARMS);
		assert(BBPfarms[bi->vh->farmid].roles & (1U << PERSISTENT));
	}
#endif

	if (size > bi->count)
		size = bi->count;
	if (fprintf(fp, "%d %u %s %s %d " BUNFMT " " BUNFMT " " OIDFMT,
		    /* BAT info */
		    (int) i,
		    BBP_status(i) & BBPPERSISTENT,
		    BBP_logical(i),
		    BBP_physical(i),
		    bi->b->batRestricted << 1,
		    size,
		    bi->b->batCapacity,
		    bi->b->hseqbase) < 0 ||
	    heap_entry(fp, bi, size, minpos, maxpos) < 0 ||
	    vheap_entry(fp, bi, size) < 0 ||
	    (BBP_options(i) && fprintf(fp, " %s", BBP_options(i)) < 0) ||
	    fprintf(fp, "\n") < 0) {
		GDKsyserror("new_bbpentry: Writing BBP.dir entry failed\n");
		return GDK_FAIL;
	}

	return GDK_SUCCEED;
}

static gdk_return
BBPdir_header(FILE *f, int n, lng logno, lng transid)
{
	if (fprintf(f, "BBP.dir, GDKversion %u\n%d %d %d\nBBPsize=%d\nBBPinfo=" LLFMT " " LLFMT "\n",
		    GDKLIBRARY, SIZEOF_SIZE_T, SIZEOF_OID,
#ifdef HAVE_HGE
		    havehge ? SIZEOF_HGE :
#endif
		    SIZEOF_LNG, n, logno, transid) < 0 ||
	    ferror(f)) {
		GDKsyserror("Writing BBP.dir header failed\n");
		return GDK_FAIL;
	}
	return GDK_SUCCEED;
}

static gdk_return
BBPdir_first(bool subcommit, lng logno, lng transid,
	     FILE **obbpfp, FILE **nbbpfp)
{
	FILE *obbpf = NULL, *nbbpf = NULL;
	int n = 0;
	lng ologno, otransid;

	if (obbpfp)
		*obbpfp = NULL;
	*nbbpfp = NULL;

	if ((nbbpf = GDKfilelocate(0, "BBP", "w", "dir")) == NULL) {
		return GDK_FAIL;
	}

	if (subcommit) {
		char buf[512];

		assert(obbpfp != NULL);
		/* we need to copy the backup BBP.dir to the new, but
		 * replacing the entries for the subcommitted bats */
		if ((obbpf = GDKfileopen(0, SUBDIR, "BBP", "dir", "r")) == NULL &&
		    (obbpf = GDKfileopen(0, BAKDIR, "BBP", "dir", "r")) == NULL) {
			GDKsyserror("subcommit attempted without backup BBP.dir.");
			goto bailout;
		}
		/* read first three lines */
		if (fgets(buf, sizeof(buf), obbpf) == NULL || /* BBP.dir, GDKversion %d */
		    fgets(buf, sizeof(buf), obbpf) == NULL || /* SIZEOF_SIZE_T SIZEOF_OID SIZEOF_MAX_INT */
		    fgets(buf, sizeof(buf), obbpf) == NULL) { /* BBPsize=%d */
			GDKerror("subcommit attempted with invalid backup BBP.dir.");
			goto bailout;
		}
		/* third line contains BBPsize */
		if (sscanf(buf, "BBPsize=%d", &n) != 1) {
			GDKerror("cannot read BBPsize in backup BBP.dir.");
			goto bailout;
		}
		/* fourth line contains BBPinfo */
		if (fgets(buf, sizeof(buf), obbpf) == NULL ||
		    sscanf(buf, "BBPinfo=" LLSCN " " LLSCN, &ologno, &otransid) != 2) {
			GDKerror("cannot read BBPinfo in backup BBP.dir.");
			goto bailout;
		}
	}

	if (n < (bat) ATOMIC_GET(&BBPsize))
		n = (bat) ATOMIC_GET(&BBPsize);

	TRC_DEBUG(IO_, "writing BBP.dir (%d bats).\n", n);

	if (BBPdir_header(nbbpf, n, logno, transid) != GDK_SUCCEED) {
		goto bailout;
	}

	if (obbpfp)
		*obbpfp = obbpf;
	*nbbpfp = nbbpf;

	return GDK_SUCCEED;

  bailout:
	if (obbpf != NULL)
		fclose(obbpf);
	if (nbbpf != NULL)
		fclose(nbbpf);
	return GDK_FAIL;
}

static bat
BBPdir_step(bat bid, BUN size, int n, char *buf, size_t bufsize,
	    FILE **obbpfp, FILE *nbbpf, BATiter *bi,
	    oid minpos, oid maxpos)
{
	if (n < -1)		/* safety catch */
		return n;
	while (n >= 0 && n < bid) {
		if (n > 0) {
			if (fputs(buf, nbbpf) == EOF) {
				GDKerror("Writing BBP.dir file failed.\n");
				goto bailout;
			}
		}
		if (fgets(buf, (int) bufsize, *obbpfp) == NULL) {
			if (ferror(*obbpfp)) {
				GDKerror("error reading backup BBP.dir.");
				goto bailout;
			}
			n = -1;
			if (fclose(*obbpfp) == EOF) {
				GDKsyserror("Closing backup BBP.dir file failed.\n");
				GDKclrerr(); /* ignore error */
			}
			*obbpfp = NULL;
		} else {
			if (sscanf(buf, "%d", &n) != 1 || n <= 0) {
				GDKerror("subcommit attempted with invalid backup BBP.dir.");
				goto bailout;
			}
		}
	}
	if (BBP_status(bid) & BBPPERSISTENT) {
		if (new_bbpentry(nbbpf, bid, size, bi, minpos, maxpos) != GDK_SUCCEED)
			goto bailout;
	}
	return n == -1 ? -1 : n == bid ? 0 : n;

  bailout:
	if (*obbpfp)
		fclose(*obbpfp);
	fclose(nbbpf);
	return -2;
}

static gdk_return
BBPdir_last(int n, char *buf, size_t bufsize, FILE *obbpf, FILE *nbbpf)
{
	if (n > 0 && fputs(buf, nbbpf) == EOF) {
		GDKerror("Writing BBP.dir file failed.\n");
		goto bailout;
	}
	while (obbpf) {
		if (fgets(buf, (int) bufsize, obbpf) == NULL) {
			if (ferror(obbpf)) {
				GDKerror("error reading backup BBP.dir.");
				goto bailout;
			}
			if (fclose(obbpf) == EOF) {
				GDKsyserror("Closing backup BBP.dir file failed.\n");
				GDKclrerr(); /* ignore error */
			}
			obbpf = NULL;
		} else {
			if (fputs(buf, nbbpf) == EOF) {
				GDKerror("Writing BBP.dir file failed.\n");
				goto bailout;
			}
		}
	}
	if (fflush(nbbpf) == EOF ||
	    (!(GDKdebug & NOSYNCMASK)
#if defined(NATIVE_WIN32)
	     && _commit(_fileno(nbbpf)) < 0
#elif defined(HAVE_FDATASYNC)
	     && fdatasync(fileno(nbbpf)) < 0
#elif defined(HAVE_FSYNC)
	     && fsync(fileno(nbbpf)) < 0
#endif
		    )) {
		GDKsyserror("Syncing BBP.dir file failed\n");
		goto bailout;
	}
	if (fclose(nbbpf) == EOF) {
		GDKsyserror("Closing BBP.dir file failed\n");
		nbbpf = NULL;	/* can't close again */
		goto bailout;
	}

	TRC_DEBUG(IO_, "end\n");

	return GDK_SUCCEED;

  bailout:
	if (obbpf != NULL)
		fclose(obbpf);
	if (nbbpf != NULL)
		fclose(nbbpf);
	return GDK_FAIL;
}

gdk_return
BBPdir_init(void)
{
	FILE *fp;
	gdk_return rc;

	rc = BBPdir_first(false, 0, 0, NULL, &fp);
	if (rc == GDK_SUCCEED)
		rc = BBPdir_last(-1, NULL, 0, NULL, fp);
	return rc;
}

/* function used for debugging */
void
BBPdump(void)
{
	size_t mem = 0, vm = 0;
	size_t cmem = 0, cvm = 0;
	int n = 0, nc = 0;

	for (bat i = 0; i < (bat) ATOMIC_GET(&BBPsize); i++) {
		if (BBP_refs(i) == 0 && BBP_lrefs(i) == 0)
			continue;
		BAT *b = BBP_desc(i);
		unsigned status = BBP_status(i);
		fprintf(stderr,
			"# %d: " ALGOOPTBATFMT " "
			"refs=%d lrefs=%d "
			"status=%u%s",
			i,
			ALGOOPTBATPAR(b),
			BBP_refs(i),
			BBP_lrefs(i),
			status,
			BBP_cache(i) ? "" : " not cached");
		if (b == NULL) {
			fprintf(stderr, ", no descriptor\n");
			continue;
		}
		if (b->batSharecnt > 0)
			fprintf(stderr, " shares=%d", b->batSharecnt);
		if (b->theap) {
			if (b->theap->parentid != b->batCacheid) {
				fprintf(stderr, " Theap -> %d", b->theap->parentid);
			} else {
				fprintf(stderr,
					" Theap=[%zu,%zu,f=%d]%s%s",
					b->theap->free,
					b->theap->size,
					b->theap->farmid,
					b->theap->base == NULL ? "X" : b->theap->storage == STORE_MMAP ? "M" : "",
					status & BBPSWAPPED ? "(Swapped)" : b->theap->dirty ? "(Dirty)" : "");
				if (BBP_logical(i) && BBP_logical(i)[0] == '.') {
					cmem += HEAPmemsize(b->theap);
					cvm += HEAPvmsize(b->theap);
					nc++;
				} else {
					mem += HEAPmemsize(b->theap);
					vm += HEAPvmsize(b->theap);
					n++;
				}
			}
		}
		if (b->tvheap) {
			if (b->tvheap->parentid != b->batCacheid) {
				fprintf(stderr,
					" Tvheap -> %d",
					b->tvheap->parentid);
			} else {
				fprintf(stderr,
					" Tvheap=[%zu,%zu,f=%d]%s%s",
					b->tvheap->free,
					b->tvheap->size,
					b->tvheap->farmid,
					b->tvheap->base == NULL ? "X" : b->tvheap->storage == STORE_MMAP ? "M" : "",
					b->tvheap->dirty ? "(Dirty)" : "");
				if (BBP_logical(i) && BBP_logical(i)[0] == '.') {
					cmem += HEAPmemsize(b->tvheap);
					cvm += HEAPvmsize(b->tvheap);
				} else {
					mem += HEAPmemsize(b->tvheap);
					vm += HEAPvmsize(b->tvheap);
				}
			}
		}
		if (MT_rwlock_rdtry(&b->thashlock)) {
			if (b->thash && b->thash != (Hash *) 1) {
				size_t m = HEAPmemsize(&b->thash->heaplink) + HEAPmemsize(&b->thash->heapbckt);
				size_t v = HEAPvmsize(&b->thash->heaplink) + HEAPvmsize(&b->thash->heapbckt);
				fprintf(stderr, " Thash=[%zu,%zu,f=%d/%d]", m, v,
					b->thash->heaplink.farmid,
					b->thash->heapbckt.farmid);
				if (BBP_logical(i) && BBP_logical(i)[0] == '.') {
					cmem += m;
					cvm += v;
				} else {
					mem += m;
					vm += v;
				}
			}
			MT_rwlock_rdunlock(&b->thashlock);
		}
		fprintf(stderr, " role: %s\n",
			b->batRole == PERSISTENT ? "persistent" : "transient");
	}
	fprintf(stderr,
		"# %d bats: mem=%zu, vm=%zu %d cached bats: mem=%zu, vm=%zu\n",
		n, mem, vm, nc, cmem, cvm);
	fflush(stderr);
}

/*
 * @+ BBP Readonly Interface
 *
 * These interface functions do not change the BBP tables. If they
 * only access one specific BAT, the caller must have ensured that no
 * other thread is modifying that BAT, therefore such functions do not
 * need locking.
 *
 * BBP index lookup by BAT name:
 */
static inline bat
BBP_find(const char *nme, bool lock)
{
	bat i = BBPnamecheck(nme);

	if (i != 0) {
		/* for tmp_X BATs, we already know X */
		const char *s;

		if (i >= (bat) ATOMIC_GET(&BBPsize) || (s = BBP_logical(i)) == NULL || strcmp(s, nme)) {
			i = 0;
		}
	} else if (*nme != '.') {
		/* must lock since hash-lookup traverses other BATs */
		if (lock)
			MT_lock_set(&BBPnameLock);
		for (i = BBP_hash[strHash(nme) & BBP_mask]; i; i = BBP_next(i)) {
			if (strcmp(BBP_logical(i), nme) == 0)
				break;
		}
		if (lock)
			MT_lock_unset(&BBPnameLock);
	}
	return i;
}

bat
BBPindex(const char *nme)
{
	return BBP_find(nme, true);
}

/*
 * @+ BBP Update Interface
 * Operations to insert, delete, clear, and modify BBP entries.
 * Our policy for the BBP is to provide unlocked BBP access for
 * speed, but still write operations have to be locked.
 * #ifdef DEBUG_THREADLOCAL_BATS
 * Create the shadow version (reversed) of a bat.
 *
 * An existing BAT is inserted into the BBP
 */
static inline str
BBPsubdir_recursive(str s, bat i)
{
	i >>= 6;
	if (i >= 0100) {
		s = BBPsubdir_recursive(s, i);
		*s++ = DIR_SEP;
	}
	i &= 077;
	*s++ = '0' + (i >> 3);
	*s++ = '0' + (i & 7);
	return s;
}

static inline void
BBPgetsubdir(str s, bat i)
{
	if (i >= 0100) {
		s = BBPsubdir_recursive(s, i);
	}
	*s = 0;
}

/* There are BBP_THREADMASK+1 (64) free lists, and ours (idx) is
 * empty.  Here we find a longish free list (at least 20 entries), and
 * if we can find one, we take one entry from that list.  If no long
 * enough list can be found, we create a new entry by either just
 * increasing BBPsize (up to BBPlimit) or extending the BBP (which
 * increases BBPlimit).  Every time this function is called we start
 * searching in a following free list (variable "last").
 *
 * Note that this is the only place in normal, multi-threaded operation
 * where BBPsize is assigned a value (never decreasing), that the
 * assignment happens after any necessary memory was allocated and
 * initialized, and that this happens when the BBPnameLock is held. */
static gdk_return
maybeextend(int idx)
{
#if BBP_THREADMASK > 0
	int t, m;
	int n, l;
	bat i;
	static int last = 0;

	l = 0;			/* length of longest list */
	m = 0;			/* index of longest list */
	/* find a longish free list */
	for (t = 0; t <= BBP_THREADMASK && l <= 20; t++) {
		n = 0;
		for (i = BBP_free((t + last) & BBP_THREADMASK);
		     i != 0 && n <= 20;
		     i = BBP_next(i))
			n++;
		if (n > l) {
			m = (t + last) & BBP_THREADMASK;
			l = n;
		}
	}
	if (l > 20) {
		/* list is long enough, get an entry from there */
		i = BBP_free(m);
		BBP_free(m) = BBP_next(i);
		BBP_next(i) = 0;
		BBP_free(idx) = i;
	} else {
#endif
		/* let the longest list alone, get a fresh entry */
		bat size = (bat) ATOMIC_GET(&BBPsize);
		if (size >= BBPlimit &&
		    BBPextend(idx, true, size + 1) != GDK_SUCCEED) {
			/* couldn't extend; if there is any
			 * free entry, take it from the
			 * longest list after all */
#if BBP_THREADMASK > 0
			if (l > 0) {
				i = BBP_free(m);
				BBP_free(m) = BBP_next(i);
				BBP_next(i) = 0;
				BBP_free(idx) = i;
				GDKclrerr();
			} else
#endif
			{
				/* nothing available */
				return GDK_FAIL;
			}
		} else {
			ATOMIC_SET(&BBPsize, size + 1);
			BBP_free(idx) = size;
		}
#if BBP_THREADMASK > 0
	}
	last = (last + 1) & BBP_THREADMASK;
#endif
	return GDK_SUCCEED;
}

/* return new BAT id (> 0); return 0 on failure */
bat
BBPinsert(BAT *bn)
{
	MT_Id pid = MT_getpid();
	bool lock = locked_by == 0 || locked_by != pid;
	char dirname[24];
	bat i;
	int idx = threadmask(pid), len = 0;

	/* critical section: get a new BBP entry */
	if (lock) {
		MT_lock_set(&GDKcacheLock(idx));
	}

	/* find an empty slot */
	if (BBP_free(idx) <= 0) {
		/* we need to extend the BBP */
		gdk_return r = GDK_SUCCEED;
#if BBP_THREADMASK > 0
		if (lock) {
			/* we must take all locks in a consistent
			 * order so first unset the one we've already
			 * got */
			MT_lock_unset(&GDKcacheLock(idx));
			for (i = 0; i <= BBP_THREADMASK; i++)
				MT_lock_set(&GDKcacheLock(i));
		}
#endif
		MT_lock_set(&BBPnameLock);
		/* check again in case some other thread extended
		 * while we were waiting */
		if (BBP_free(idx) <= 0) {
			r = maybeextend(idx);
		}
		MT_lock_unset(&BBPnameLock);
#if BBP_THREADMASK > 0
		if (lock)
			for (i = BBP_THREADMASK; i >= 0; i--)
				if (i != idx)
					MT_lock_unset(&GDKcacheLock(i));
#endif
		if (r != GDK_SUCCEED) {
			if (lock) {
				MT_lock_unset(&GDKcacheLock(idx));
			}
			return 0;
		}
	}
	i = BBP_free(idx);
	assert(i > 0);
	BBP_free(idx) = BBP_next(i);

	if (lock) {
		MT_lock_unset(&GDKcacheLock(idx));
	}
	/* rest of the work outside the lock */

	/* fill in basic BBP fields for the new bat */

	bn->batCacheid = i;
	bn->creator_tid = MT_getpid();

	MT_lock_set(&GDKswapLock(i));
	BBP_status_set(i, BBPDELETING|BBPHOT);
	BBP_cache(i) = NULL;
	BBP_desc(i) = NULL;
	BBP_refs(i) = 1;	/* new bats have 1 pin */
	BBP_lrefs(i) = 0;	/* ie. no logical refs */
	BBP_pid(i) = MT_getpid();
	MT_lock_unset(&GDKswapLock(i));

#ifdef HAVE_HGE
	if (bn->ttype == TYPE_hge)
		havehge = true;
#endif

	if (*BBP_bak(i) == 0)
		len = snprintf(BBP_bak(i), sizeof(BBP_bak(i)), "tmp_%o", (unsigned) i);
	if (len == -1 || len >= FILENAME_MAX) {
		GDKerror("impossible error\n");
		return 0;
	}
	BBP_logical(i) = BBP_bak(i);

	/* Keep the physical location around forever */
	if (!GDKinmemory(0) && *BBP_physical(i) == 0) {
		BBPgetsubdir(dirname, i);

		if (*dirname)	/* i.e., i >= 0100 */
			len = snprintf(BBP_physical(i), sizeof(BBP_physical(i)),
				       "%s%c%o", dirname, DIR_SEP, (unsigned) i);
		else
			len = snprintf(BBP_physical(i), sizeof(BBP_physical(i)),
				       "%o", (unsigned) i);
		if (len == -1 || len >= FILENAME_MAX)
			return 0;

		TRC_DEBUG(BAT_, "%d = new %s(%s)\n", (int) i, BBP_logical(i), ATOMname(bn->ttype));
	}

	return i;
}

gdk_return
BBPcacheit(BAT *bn, bool lock)
{
	bat i = bn->batCacheid;
	unsigned mode;

	if (lock)
		lock = locked_by == 0 || locked_by != MT_getpid();

	if (i) {
		assert(i > 0);
	} else {
		i = BBPinsert(bn);	/* bat was not previously entered */
		if (i == 0)
			return GDK_FAIL;
		if (bn->theap)
			bn->theap->parentid = i;
		if (bn->tvheap)
			bn->tvheap->parentid = i;
	}

	if (lock)
		MT_lock_set(&GDKswapLock(i));
	mode = (BBP_status(i) | BBPLOADED) & ~(BBPLOADING | BBPDELETING | BBPSWAPPED);
	BBP_desc(i) = bn;

	/* cache it! */
	BBP_cache(i) = bn;

	BBP_status_set(i, mode);

	if (lock)
		MT_lock_unset(&GDKswapLock(i));
	return GDK_SUCCEED;
}

/*
 * BBPuncacheit changes the BBP status to swapped out.  Currently only
 * used in BBPfree (bat swapped out) and BBPclear (bat destroyed
 * forever).
 */

static void
BBPuncacheit(bat i, bool unloaddesc)
{
	if (i < 0)
		i = -i;
	if (BBPcheck(i)) {
		BAT *b = BBP_desc(i);

		assert(unloaddesc || BBP_refs(i) == 0);

		if (b) {
			if (BBP_cache(i)) {
				TRC_DEBUG(BAT_, "uncache %d (%s)\n", (int) i, BBP_logical(i));

				/* clearing bits can be done without the lock */
				BBP_status_off(i, BBPLOADED);

				BBP_cache(i) = NULL;
			}
			if (unloaddesc) {
				BBP_desc(i) = NULL;
				BATdestroy(b);
			}
		}
	}
}

/*
 * @- BBPclear
 * BBPclear removes a BAT from the BBP directory forever.
 */
static inline void
bbpclear(bat i, int idx, bool lock)
{
	TRC_DEBUG(BAT_, "clear %d (%s)\n", (int) i, BBP_logical(i));
	BBPuncacheit(i, true);
	TRC_DEBUG(BAT_, "set to unloading %d\n", i);
	if (lock) {
		MT_lock_set(&GDKcacheLock(idx));
		MT_lock_set(&GDKswapLock(i));
	}

	BBP_status_set(i, BBPUNLOADING);
	BBP_refs(i) = 0;
	BBP_lrefs(i) = 0;
	if (lock)
		MT_lock_unset(&GDKswapLock(i));
	if (!BBPtmpcheck(BBP_logical(i))) {
		MT_lock_set(&BBPnameLock);
		BBP_delete(i);
		MT_lock_unset(&BBPnameLock);
	}
	if (BBP_logical(i) != BBP_bak(i))
		GDKfree(BBP_logical(i));
	BBP_status_set(i, 0);
	BBP_logical(i) = NULL;
	BBP_next(i) = BBP_free(idx);
	BBP_free(idx) = i;
	BBP_pid(i) = ~(MT_Id)0; /* not zero, not a valid thread id */
	if (lock)
		MT_lock_unset(&GDKcacheLock(idx));
}

void
BBPclear(bat i, bool lock)
{
	MT_Id pid = MT_getpid();

	lock &= locked_by == 0 || locked_by != pid;
	if (BBPcheck(i)) {
		bbpclear(i, threadmask(pid), lock);
	}
}

/*
 * @- BBP rename
 *
 * Each BAT has a logical name that is globally unique.
 * The batId is the same as the logical BAT name.
 *
 * The default logical name of a BAT is tmp_X, where X is the
 * batCacheid.  Apart from being globally unique, new logical bat
 * names cannot be of the form tmp_X, unless X is the batCacheid.
 *
 * Physical names consist of a directory name followed by a logical
 * name suffix.  The directory name is derived from the batCacheid,
 * and is currently organized in a hierarchy that puts max 64 bats in
 * each directory (see BBPgetsubdir).
 *
 * Concerning the physical suffix: it is almost always bat_X. This
 * saves us a whole lot of trouble, as bat_X is always unique and no
 * conflicts can occur.  Other suffixes are only supported in order
 * just for backward compatibility with old repositories (you won't
 * see them anymore in new repositories).
 */
int
BBPrename(bat bid, const char *nme)
{
	BAT *b = BBPdescriptor(bid);
	char dirname[24];
	bat tmpid = 0, i;

	if (b == NULL)
		return 0;

	if (nme == NULL) {
		if (BBP_bak(bid)[0] == 0 &&
		    snprintf(BBP_bak(bid), sizeof(BBP_bak(bid)), "tmp_%o", (unsigned) bid) >= (int) sizeof(BBP_bak(bid))) {
			/* cannot happen */
			TRC_CRITICAL(GDK, "BBP default filename too long\n");
			return BBPRENAME_LONG;
		}
		nme = BBP_bak(bid);
	}

	/* If name stays same, do nothing */
	if (BBP_logical(bid) && strcmp(BBP_logical(bid), nme) == 0)
		return 0;

	BBPgetsubdir(dirname, bid);

	if ((tmpid = BBPnamecheck(nme)) && tmpid != bid) {
		GDKerror("illegal temporary name: '%s'\n", nme);
		return BBPRENAME_ILLEGAL;
	}
	if (strlen(dirname) + strLen(nme) + 1 >= IDLENGTH) {
		GDKerror("illegal temporary name: '%s'\n", nme);
		return BBPRENAME_LONG;
	}

	MT_lock_set(&BBPnameLock);
	i = BBP_find(nme, false);
	if (i != 0) {
		MT_lock_unset(&BBPnameLock);
		GDKerror("name is in use: '%s'.\n", nme);
		return BBPRENAME_ALREADY;
	}

	char *nnme;
	if (nme == BBP_bak(bid) || strcmp(nme, BBP_bak(bid)) == 0) {
		nnme = BBP_bak(bid);
	} else {
		nnme = GDKstrdup(nme);
		if (nnme == NULL) {
			MT_lock_unset(&BBPnameLock);
			return BBPRENAME_MEMORY;
		}
	}

	/* carry through the name change */
	if (BBP_logical(bid) && !BBPtmpcheck(BBP_logical(bid))) {
		BBP_delete(bid);
	}
	if (BBP_logical(bid) != BBP_bak(bid))
		GDKfree(BBP_logical(bid));
	BBP_logical(bid) = nnme;
	if (tmpid == 0) {
		BBP_insert(bid);
	}
	if (!b->batTransient) {
		bool lock = locked_by == 0 || locked_by != MT_getpid();

		if (lock)
			MT_lock_set(&GDKswapLock(i));
		BBP_status_on(bid, BBPRENAMED);
		if (lock)
			MT_lock_unset(&GDKswapLock(i));
	}
	MT_lock_unset(&BBPnameLock);
	return 0;
}

/*
 * @+ BBP swapping Policy
 * The BAT can be moved back to disk using the routine BBPfree.  It
 * frees the storage for other BATs. After this call BAT* references
 * maintained for the BAT are wrong.  We should keep track of dirty
 * unloaded BATs. They may have to be committed later on, which may
 * include reading them in again.
 *
 * BBPswappable: may this bat be unloaded?  Only real bats without
 * memory references can be unloaded.
 */
static inline void
BBPspin(bat i, const char *s, unsigned event)
{
	if (BBPcheck(i) && (BBP_status(i) & event)) {
		lng spin = LL_CONSTANT(0);

		do {
			MT_sleep_ms(KITTENNAP);
			spin++;
		} while (BBP_status(i) & event);
		TRC_DEBUG(BAT_, "%d,%s,%u: " LLFMT " loops\n", (int) i, s, event, spin);
	}
}

void
BBPcold(bat i)
{
	if (!is_bat_nil(i)) {
		BAT *b = BBP_cache(i);
		if (b == NULL)
			b = BBP_desc(i);
		if (b == NULL || b->batRole == PERSISTENT)
			BBP_status_off(i, BBPHOT);
	}
}

/* This function can fail if the input parameter (i) is incorrect
 * (unlikely), of if the bat is a view, this is a physical (not
 * logical) incref (i.e. called through BBPfix(), and it is the first
 * reference (refs was 0 and should become 1).  It can fail in this
 * case if the parent bat cannot be loaded.
 * This means the return value of BBPfix should be checked in these
 * circumstances, but not necessarily in others. */
static inline int
incref(bat i, bool logical, bool lock)
{
	int refs;
	bat tp = i, tvp = i;
	BAT *b, *pb = NULL, *pvb = NULL;
	bool load = false;

	if (!BBPcheck(i))
		return 0;

	/* Before we get the lock and before we do all sorts of
	 * things, make sure we can load the parent bats if there are
	 * any.  If we can't load them, we can still easily fail.  If
	 * this is indeed a view, but not the first physical
	 * reference, getting the parent BAT descriptor is
	 * superfluous, but not too expensive, so we do it anyway. */
	if (!logical && (b = BBP_desc(i)) != NULL) {
		MT_lock_set(&b->theaplock);
		tp = b->theap ? b->theap->parentid : i;
		tvp = b->tvheap ? b->tvheap->parentid : i;
		MT_lock_unset(&b->theaplock);
		if (tp != i) {
			pb = BATdescriptor(tp);
			if (pb == NULL)
				return 0;
		}
		if (tvp != i) {
			pvb = BATdescriptor(tvp);
			if (pvb == NULL) {
				if (pb)
					BBPunfix(pb->batCacheid);
				return 0;
			}
		}
	}

	if (lock) {
		for (;;) {
			MT_lock_set(&GDKswapLock(i));
			if (!(BBP_status(i) & (BBPUNSTABLE|BBPLOADING)))
				break;
			/* the BATs is "unstable", try again */
			MT_lock_unset(&GDKswapLock(i));
			BBPspin(i, __func__, BBPUNSTABLE|BBPLOADING);
		}
	}
	/* we have the lock */

	b = BBP_desc(i);
	if (b == NULL) {
		/* should not have happened */
		if (lock)
			MT_lock_unset(&GDKswapLock(i));
		return 0;
	}

	assert(BBP_refs(i) + BBP_lrefs(i) ||
	       BBP_status(i) & (BBPDELETED | BBPSWAPPED));
	if (logical) {
		/* parent BATs are not relevant for logical refs */
		refs = ++BBP_lrefs(i);
		BBP_pid(i) = 0;
	} else {
		assert(tp >= 0);
		refs = ++BBP_refs(i);
		unsigned flag = BBPHOT;
		if (refs == 1 && (tp != i || tvp != i)) {
			/* If this is a view, we must load the parent
			 * BATs, but we must do that outside of the
			 * lock.  Set the BBPLOADING flag so that
			 * other threads will wait until we're
			 * done. */
			flag |= BBPLOADING;
			load = true;
		}
		BBP_status_on(i, flag);
	}
	if (lock)
		MT_lock_unset(&GDKswapLock(i));

	if (load) {
		/* load the parent BATs */
		assert(!logical);
		if (tp != i) {
			assert(pb != NULL);
			/* load being set implies there is no other
			 * thread that has access to this bat, but the
			 * parent is a different matter */
			MT_lock_set(&pb->theaplock);
			if (b->theap != pb->theap) {
				HEAPincref(pb->theap);
				HEAPdecref(b->theap, false);
				b->theap = pb->theap;
			}
			MT_lock_unset(&pb->theaplock);
		}
		/* done loading, release descriptor */
		BBP_status_off(i, BBPLOADING);
	} else if (!logical) {
		/* this wasn't the first physical reference, so undo
		 * the fixes on the parent bats */
		if (pb)
			BBPunfix(pb->batCacheid);
		if (pvb)
			BBPunfix(pvb->batCacheid);
	}
	return refs;
}

/* see comment for incref */
int
BBPfix(bat i)
{
	bool lock = locked_by == 0 || locked_by != MT_getpid();

	return incref(i, false, lock);
}

int
BBPretain(bat i)
{
	bool lock = locked_by == 0 || locked_by != MT_getpid();

	return incref(i, true, lock);
}

void
BBPshare(bat parent)
{
	bool lock = locked_by == 0 || locked_by != MT_getpid();

	assert(parent > 0);
	(void) incref(parent, true, lock);
	if (lock)
		MT_lock_set(&GDKswapLock(parent));
	++BBP_cache(parent)->batSharecnt;
	assert(BBP_refs(parent) > 0);
	if (lock)
		MT_lock_unset(&GDKswapLock(parent));
	(void) incref(parent, false, lock);
}

static inline int
decref(bat i, bool logical, bool releaseShare, bool lock, const char *func)
{
	int refs = 0, lrefs;
	bool swap = false;
	bat tp = 0, tvp = 0;
	int farmid = 0;
	BAT *b;

	if (is_bat_nil(i))
		return -1;
	assert(i > 0);
	if (BBPcheck(i) == 0)
		return -1;

	if (lock)
		MT_lock_set(&GDKswapLock(i));
	if (releaseShare) {
		assert(BBP_lrefs(i) > 0);
		if (BBP_desc(i)->batSharecnt == 0) {
			GDKerror("%s: %s does not have any shares.\n", func, BBP_logical(i));
			assert(0);
		} else {
			--BBP_desc(i)->batSharecnt;
		}
		if (lock)
			MT_lock_unset(&GDKswapLock(i));
		return refs;
	}

	while (BBP_status(i) & BBPUNLOADING) {
		if (lock)
			MT_lock_unset(&GDKswapLock(i));
		BBPspin(i, func, BBPUNLOADING);
		if (lock)
			MT_lock_set(&GDKswapLock(i));
	}

	b = BBP_cache(i);

	/* decrement references by one */
	if (logical) {
		if (BBP_lrefs(i) == 0) {
			GDKerror("%s: %s does not have logical references.\n", func, BBP_logical(i));
			assert(0);
		} else {
			refs = --BBP_lrefs(i);
		}
		/* cannot release last logical ref if still shared */
		assert(BBP_desc(i)->batSharecnt == 0 || refs > 0);
	} else {
		if (BBP_refs(i) == 0) {
			GDKerror("%s: %s does not have pointer fixes.\n", func, BBP_logical(i));
			assert(0);
		} else {
			assert(b == NULL || b->theap == NULL || BBP_refs(b->theap->parentid) > 0);
			assert(b == NULL || b->tvheap == NULL || BBP_refs(b->tvheap->parentid) > 0);
			refs = --BBP_refs(i);
			if (b && refs == 0) {
				tp = VIEWtparent(b);
				tvp = VIEWvtparent(b);
				if (tp || tvp)
					BBP_status_on(i, BBPHOT);
			}
		}
	}
	if (b) {
		MT_lock_set(&b->theaplock);
		if (b->batCount > b->batInserted && !isVIEW(b)) {
			/* if batCount is larger than batInserted and
			 * the dirty bits are off, it may be that a
			 * (sub)commit happened in parallel to an
			 * update; we must undo the turning off of the
			 * dirty bits */
			if (b->theap && b->theap->parentid == i)
				b->theap->dirty = true;
			if (b->tvheap && b->tvheap->parentid == i)
				b->tvheap->dirty = true;
		}
		if (b->theap)
			farmid = b->theap->farmid;
		MT_lock_unset(&b->theaplock);
	}

	/* we destroy transients asap and unload persistent bats only
	 * if they have been made cold or are not dirty */
	unsigned chkflag = BBPSYNCING;
	if (GDKvm_cursize() < GDK_vm_maxsize &&
	     ((b && b->theap ? b->theap->size : 0) + (b && b->tvheap ? b->tvheap->size : 0)) < (GDK_vm_maxsize - GDKvm_cursize()) / 32)
		chkflag |= BBPHOT;
	/* only consider unloading if refs is 0; if, in addition, lrefs
	 * is 0, we can definitely unload, else only if some more
	 * conditions are met */
	if (BBP_refs(i) == 0 &&
	    (BBP_lrefs(i) == 0 ||
	     (b != NULL
	      ? (!BATdirty(b) &&
		 !(BBP_status(i) & chkflag) &&
		 (BBP_status(i) & BBPPERSISTENT) &&
		 !GDKinmemory(farmid) &&
		 b->batSharecnt == 0)
	      : (BBP_status(i) & BBPTMP)))) {
		/* bat will be unloaded now. set the UNLOADING bit
		 * while locked so no other thread thinks it's
		 * available anymore */
		assert((BBP_status(i) & BBPUNLOADING) == 0);
		TRC_DEBUG(BAT_, "%s set to unloading BAT %d (status %u, lrefs %d)\n", func, i, BBP_status(i), BBP_lrefs(i));
		BBP_status_on(i, BBPUNLOADING);
		swap = true;
	} /* else: bat cannot be swapped out */
	lrefs = BBP_lrefs(i);

	/* unlock before re-locking in unload; as saving a dirty
	 * persistent bat may take a long time */
	if (lock)
		MT_lock_unset(&GDKswapLock(i));

	if (swap) {
		if (b != NULL) {
			if (lrefs == 0 && (BBP_status(i) & BBPDELETED) == 0) {
				/* free memory (if loaded) and delete from
				 * disk (if transient but saved) */
				BBPdestroy(b);
			} else {
				TRC_DEBUG(BAT_, "%s unload and free bat %d\n", func, i);
				/* free memory of transient */
				if (BBPfree(b) != GDK_SUCCEED)
					return -1;	/* indicate failure */
			}
		} else if (lrefs == 0 && (BBP_status(i) & BBPDELETED) == 0) {
			if ((b = BBP_desc(i)) != NULL)
				BATdelete(b);
			BBPclear(i, true);
		} else {
			BBP_status_off(i, BBPUNLOADING);
		}
	}
	if (tp)
		decref(tp, false, false, lock, func);
	if (tvp)
		decref(tvp, false, false, lock, func);
	return refs;
}

int
BBPunfix(bat i)
{
	return decref(i, false, false, true, __func__);
}

int
BBPrelease(bat i)
{
	return decref(i, true, false, true, __func__);
}

/*
 * M5 often changes the physical ref into a logical reference.  This
 * state change consist of the sequence BBPretain(b);BBPunfix(b).
 * A faster solution is given below, because it does not trigger the
 * BBP management actions, such as garbage collecting the bats.
 * [first step, initiate code change]
 */
void
BBPkeepref(bat i)
{
	if (BBPcheck(i)) {
		bool lock = locked_by == 0 || locked_by != MT_getpid();
		BAT *b;

		int refs = incref(i, true, lock);
		if ((b = BBPdescriptor(i)) != NULL) {
			if (refs == 1) {
				MT_lock_set(&b->theaplock);
				BATsettrivprop(b);
				MT_lock_unset(&b->theaplock);
			}
			if (GDKdebug & (CHECKMASK | PROPMASK))
				BATassertProps(b);
			if (BATsetaccess(b, BAT_READ) == NULL)
				return; /* already decreffed */
		}

		assert(BBP_refs(i));
		decref(i, false, false, lock, __func__);
	}
}

static inline void
GDKunshare(bat parent)
{
	(void) decref(parent, false, true, true, __func__);
	(void) decref(parent, true, false, true, __func__);
}

void
BBPunshare(bat parent)
{
	GDKunshare(parent);
}

/*
 * BBPreclaim is a user-exported function; the common way to destroy a
 * BAT the hard way.
 *
 * Return values:
 * -1 = bat cannot be unloaded (it has more than your own memory fix)
 *  0 = unloaded successfully
 *  1 = unload failed (due to write-to-disk failure)
 */
int
BBPreclaim(BAT *b)
{
	bat i;
	bool lock = locked_by == 0 || locked_by != MT_getpid();

	if (b == NULL)
		return -1;
	i = b->batCacheid;

	assert(BBP_refs(i) == 1);

	return decref(i, false, false, lock, __func__) <0;
}

/*
 * BBPdescriptor checks whether BAT needs loading and does so if
 * necessary. You must have at least one fix on the BAT before calling
 * this.
 */
static BAT *
getBBPdescriptor(bat i, bool lock)
{
	bool load = false;
	BAT *b = NULL;

	assert(i > 0);
	if (!BBPcheck(i)) {
		GDKerror("BBPcheck failed for bat id %d\n", i);
		return NULL;
	}
	assert(BBP_refs(i));
	if (lock)
		MT_lock_set(&GDKswapLock(i));
	if ((b = BBP_cache(i)) == NULL || BBP_status(i) & BBPWAITING) {

		while (BBP_status(i) & BBPWAITING) {	/* wait for bat to be loaded by other thread */
			if (lock)
				MT_lock_unset(&GDKswapLock(i));
			BBPspin(i, __func__, BBPWAITING);
			if (lock)
				MT_lock_set(&GDKswapLock(i));
		}
		if (BBPvalid(i)) {
			b = BBP_cache(i);
			if (b == NULL) {
				load = true;
				TRC_DEBUG(BAT_, "set to loading BAT %d\n", i);
				BBP_status_on(i, BBPLOADING);
			}
		}
	}
	if (lock)
		MT_lock_unset(&GDKswapLock(i));
	if (load) {
		TRC_DEBUG(IO_, "load %s\n", BBP_logical(i));

		b = BATload_intern(i, lock);

		/* clearing bits can be done without the lock */
		BBP_status_off(i, BBPLOADING);
		CHECKDEBUG if (b != NULL)
			BATassertProps(b);
	}
	return b;
}

BAT *
BBPdescriptor(bat i)
{
	bool lock = locked_by == 0 || locked_by != MT_getpid();

	return getBBPdescriptor(i, lock);
}

/*
 * In BBPsave executes unlocked; it just marks the BBP_status of the
 * BAT to BBPsaving, so others that want to save or unload this BAT
 * must spin lock on the BBP_status field.
 */
gdk_return
BBPsave(BAT *b)
{
	bool lock = locked_by == 0 || locked_by != MT_getpid();
	bat bid = b->batCacheid;
	gdk_return ret = GDK_SUCCEED;

	if (BBP_lrefs(bid) == 0 || isVIEW(b) || !BATdirtydata(b)) {
		/* do nothing */
		MT_rwlock_rdlock(&b->thashlock);
		if (b->thash && b->thash != (Hash *) 1 &&
		    (b->thash->heaplink.dirty || b->thash->heapbckt.dirty))
			BAThashsave(b, (BBP_status(bid) & BBPPERSISTENT) != 0);
		MT_rwlock_rdunlock(&b->thashlock);
		return GDK_SUCCEED;
	}
	if (lock)
		MT_lock_set(&GDKswapLock(bid));

	if (BBP_status(bid) & BBPSAVING) {
		/* wait until save in other thread completes */
		if (lock)
			MT_lock_unset(&GDKswapLock(bid));
		BBPspin(bid, __func__, BBPSAVING);
	} else {
		/* save it */
		unsigned flags = BBPSAVING;

		if (DELTAdirty(b)) {
			flags |= BBPSWAPPED;
		}
		if (b->batTransient) {
			flags |= BBPTMP;
		}
		BBP_status_on(bid, flags);
		if (lock)
			MT_lock_unset(&GDKswapLock(bid));

		TRC_DEBUG(IO_, "save %s\n", BATgetId(b));

		/* do the time-consuming work unlocked */
		if (BBP_status(bid) & BBPEXISTING)
			ret = BBPbackup(b, false);
		if (ret == GDK_SUCCEED) {
			ret = BATsave(b);
		}
		/* clearing bits can be done without the lock */
		BBP_status_off(bid, BBPSAVING);
	}
	return ret;
}

/*
 * TODO merge BBPfree with BATfree? Its function is to prepare a BAT
 * for being unloaded (or even destroyed, if the BAT is not
 * persistent).
 */
static void
BBPdestroy(BAT *b)
{
	bat tp = VIEWtparent(b);
	bat vtp = VIEWvtparent(b);

	if (tp == 0) {
		/* bats that get destroyed must unfix their atoms */
		gdk_return (*tunfix) (const void *) = BATatoms[b->ttype].atomUnfix;
		assert(b->batSharecnt == 0);
		if (tunfix) {
			BUN p, q;
			BATiter bi = bat_iterator_nolock(b);

			BATloop(b, p, q) {
				/* ignore errors */
				(void) (*tunfix)(BUNtail(bi, p));
			}
		}
	}
	if (tp || vtp)
		VIEWunlink(b);
	BATdelete(b);

	BBPclear(b->batCacheid, true);	/* if destroyed; de-register from BBP */

	/* parent released when completely done with child */
	if (tp)
		GDKunshare(tp);
	if (vtp)
		GDKunshare(vtp);
}

static gdk_return
BBPfree(BAT *b)
{
	bat bid = b->batCacheid, tp = VIEWtparent(b), vtp = VIEWvtparent(b);
	gdk_return ret;

	assert(bid > 0);
	assert(BBPswappable(b));

	BBP_unload_inc();
	/* write dirty BATs before being unloaded */
	ret = BBPsave(b);
	if (ret == GDK_SUCCEED) {
		if (isVIEW(b)) {	/* physical view */
			VIEWdestroy(b);
		} else {
			if (BBP_cache(bid))
				BATfree(b);	/* free memory */
		}
		BBPuncacheit(bid, false);
	}
	/* clearing bits can be done without the lock */
	TRC_DEBUG(BAT_, "turn off unloading %d\n", bid);
	BBP_status_off(bid, BBPUNLOADING);
	BBP_unload_dec();

	/* parent released when completely done with child */
	if (ret == GDK_SUCCEED && tp)
		GDKunshare(tp);
	if (ret == GDK_SUCCEED && vtp)
		GDKunshare(vtp);
	return ret;
}

/*
 * BBPquickdesc loads a BAT descriptor without loading the entire BAT,
 * of which the result be used only for a *limited* number of
 * purposes. Specifically, during the global sync/commit, we do not
 * want to load any BATs that are not already loaded, both because
 * this costs performance, and because getting into memory shortage
 * during a commit is extremely dangerous. Loading a BAT tends not to
 * be required, since the commit actions mostly involve moving some
 * pointers in the BAT descriptor.
 */
BAT *
BBPquickdesc(bat bid)
{
	BAT *b;

	if (!BBPcheck(bid)) {
		if (!is_bat_nil(bid)) {
			GDKerror("called with invalid batid.\n");
			assert(0);
		}
		return NULL;
	}
	if ((b = BBP_cache(bid)) != NULL)
		return b;	/* already cached */
	b = BBP_desc(bid);
	if (b && b->ttype < 0) {
		const char *aname = ATOMunknown_name(b->ttype);
		int tt = ATOMindex(aname);
		if (tt < 0) {
			TRC_WARNING(GDK, "atom '%s' unknown in bat '%s'.\n",
				    aname, BBP_physical(bid));
		} else {
			b->ttype = tt;
		}
	}
	return b;
}

/*
 * @+ Global Commit
 */
static BAT *
dirty_bat(bat *i, bool subcommit)
{
	if (BBPvalid(*i)) {
		BAT *b;
		BBPspin(*i, __func__, BBPSAVING);
		b = BBP_cache(*i);
		if (b != NULL) {
			if ((BBP_status(*i) & BBPNEW) &&
			    BATcheckmodes(b, false) != GDK_SUCCEED) /* check mmap modes */
				*i = -*i;	/* error */
			else if ((BBP_status(*i) & BBPPERSISTENT) &&
				 (subcommit || BATdirty(b)))
				return b;	/* the bat is loaded, persistent and dirty */
		} else if (BBP_status(*i) & BBPSWAPPED) {
			b = (BAT *) BBPquickdesc(*i);
			if (b && subcommit)
				return b;	/* only the desc is loaded & dirty */
		}
	}
	return NULL;
}

/*
 * @- backup-bat
 * Backup-bat moves all files of a BAT to a backup directory. Only
 * after this succeeds, it may be saved. If some failure occurs
 * halfway saving, we can thus always roll back.
 */
static gdk_return
file_move(int farmid, const char *srcdir, const char *dstdir, const char *name, const char *ext)
{
	if (GDKmove(farmid, srcdir, name, ext, dstdir, name, ext, false) == GDK_SUCCEED) {
		return GDK_SUCCEED;
	} else {
		char *path;
		struct stat st;

		path = GDKfilepath(farmid, srcdir, name, ext);
		if (path == NULL)
			return GDK_FAIL;
		if (MT_stat(path, &st)) {
			/* source file does not exist; the best
			 * recovery is to give an error but continue
			 * by considering the BAT as not saved; making
			 * sure that this time it does get saved.
			 */
			GDKsyserror("file_move: cannot stat %s\n", path);
			GDKfree(path);
			return GDK_FAIL;	/* fishy, but not fatal */
		}
		GDKfree(path);
	}
	return GDK_FAIL;
}

/* returns true if the file exists */
static bool
file_exists(int farmid, const char *dir, const char *name, const char *ext)
{
	char *path;
	struct stat st;
	int ret = -1;

	path = GDKfilepath(farmid, dir, name, ext);
	if (path) {
		ret = MT_stat(path, &st);
		TRC_DEBUG(IO_, "stat(%s) = %d\n", path, ret);
		GDKfree(path);
	}
	return (ret == 0);
}

static gdk_return
heap_move(Heap *hp, const char *srcdir, const char *dstdir, const char *nme, const char *ext)
{
	/* see doc at BATsetaccess()/gdk_bat.c for an expose on mmap
	 * heap modes */
	if (file_exists(hp->farmid, dstdir, nme, ext)) {
		/* dont overwrite heap with the committed state
		 * already in dstdir */
		return GDK_SUCCEED;
	} else if (hp->newstorage == STORE_PRIV &&
		   !file_exists(hp->farmid, srcdir, nme, ext)) {

		/* In order to prevent half-saved X.new files
		 * surviving a recover we create a dummy file in the
		 * BACKUP(dstdir) whose presence will trigger
		 * BBPrecover to remove them.  Thus, X will prevail
		 * where it otherwise wouldn't have.  If X already has
		 * a saved X.new, that one is backed up as normal.
		 */

		FILE *fp;
		long_str kill_ext;
		char *path;

		strconcat_len(kill_ext, sizeof(kill_ext), ext, ".kill", NULL);
		path = GDKfilepath(hp->farmid, dstdir, nme, kill_ext);
		if (path == NULL)
			return GDK_FAIL;
		fp = MT_fopen(path, "w");
		if (fp == NULL)
			GDKsyserror("heap_move: cannot open file %s\n", path);
		TRC_DEBUG(IO_, "open %s = %d\n", path, fp ? 0 : -1);
		GDKfree(path);

		if (fp != NULL) {
			fclose(fp);
			return GDK_SUCCEED;
		} else {
			return GDK_FAIL;
		}
	}
	return file_move(hp->farmid, srcdir, dstdir, nme, ext);
}

/*
 * @- BBPprepare
 *
 * this routine makes sure there is a BAKDIR/, and initiates one if
 * not.  For subcommits, it does the same with SUBDIR.
 *
 * It is now locked, to get proper file counters, and also to prevent
 * concurrent BBPrecovers, etc.
 *
 * backup_dir == 0 => no backup BBP.dir
 * backup_dir == 1 => BBP.dir saved in BACKUP/
 * backup_dir == 2 => BBP.dir saved in SUBCOMMIT/
 */

static gdk_return
BBPprepare(bool subcommit)
{
	bool start_subcommit;
	int set = 1 + subcommit;
	str bakdirpath, subdirpath;
	gdk_return ret = GDK_SUCCEED;

	bakdirpath = GDKfilepath(0, NULL, BAKDIR, NULL);
	subdirpath = GDKfilepath(0, NULL, SUBDIR, NULL);
	if (bakdirpath == NULL || subdirpath == NULL) {
		GDKfree(bakdirpath);
		GDKfree(subdirpath);
		return GDK_FAIL;
	}

	start_subcommit = (subcommit && backup_subdir == 0);
	if (start_subcommit) {
		/* starting a subcommit. Make sure SUBDIR and DELDIR
		 * are clean */
		ret = BBPrecover_subdir();
	}
	if (backup_files == 0) {
		backup_dir = 0;
		ret = BBPrecover(0);
		if (ret == GDK_SUCCEED) {
			if (MT_mkdir(bakdirpath) < 0 && errno != EEXIST) {
				GDKsyserror("cannot create directory %s\n", bakdirpath);
				ret = GDK_FAIL;
			}
			/* if BAKDIR already exists, don't signal error */
			TRC_DEBUG(IO_, "mkdir %s = %d\n", bakdirpath, (int) ret);
		}
	}
	if (ret == GDK_SUCCEED && start_subcommit) {
		/* make a new SUBDIR (subdir of BAKDIR) */
		if (MT_mkdir(subdirpath) < 0) {
			GDKsyserror("cannot create directory %s\n", subdirpath);
			ret = GDK_FAIL;
		}
		TRC_DEBUG(IO_, "mkdir %s = %d\n", subdirpath, (int) ret);
	}
	if (ret == GDK_SUCCEED && backup_dir != set) {
		/* a valid backup dir *must* at least contain BBP.dir */
		if ((ret = GDKmove(0, backup_dir ? BAKDIR : BATDIR, "BBP", "dir", subcommit ? SUBDIR : BAKDIR, "BBP", "dir", true)) == GDK_SUCCEED) {
			backup_dir = set;
		}
	}
	/* increase counters */
	if (ret == GDK_SUCCEED) {
		backup_subdir += subcommit;
		backup_files++;
	}
	GDKfree(bakdirpath);
	GDKfree(subdirpath);
	return ret;
}

static gdk_return
do_backup(const char *srcdir, const char *nme, const char *ext,
	  Heap *h, bool dirty, bool subcommit)
{
	gdk_return ret = GDK_SUCCEED;
	char extnew[16];
	bool istail = strncmp(ext, "tail", 4) == 0;

	if (h->wasempty) {
		return GDK_SUCCEED;
	}

	/* direct mmap is unprotected (readonly usage, or has WAL
	 * protection); however, if we're backing up for subcommit
	 * and a backup already exists in the main backup directory
	 * (see GDKupgradevarheap), move the file */
	if (subcommit) {
		strcpy_len(extnew, ext, sizeof(extnew));
		char *p = extnew + strlen(extnew) - 1;
		if (*p == 'l') {
			p++;
			p[1] = 0;
		}
		bool exists;
		for (;;) {
			exists = file_exists(h->farmid, BAKDIR, nme, extnew);
			if (exists)
				break;
			if (!istail)
				break;
			if (*p == '1')
				break;
			if (*p == '2')
				*p = '1';
#if SIZEOF_VAR_T == 8
			else if (*p != '4')
				*p = '4';
#endif
			else
				*p = '2';
		}
		if (exists &&
		    file_move(h->farmid, BAKDIR, SUBDIR, nme, extnew) != GDK_SUCCEED)
			return GDK_FAIL;
	}
	if (h->storage != STORE_MMAP) {
		/* STORE_PRIV saves into X.new files. Two cases could
		 * happen. The first is when a valid X.new exists
		 * because of an access change or a previous
		 * commit. This X.new should be backed up as
		 * usual. The second case is when X.new doesn't
		 * exist. In that case we could have half written
		 * X.new files (after a crash). To protect against
		 * these we write X.new.kill files in the backup
		 * directory (see heap_move). */
		gdk_return mvret = GDK_SUCCEED;
		bool exists;

		if (istail) {
			exists = file_exists(h->farmid, BAKDIR, nme, "tail.new") ||
#if SIZEOF_VAR_T == 8
				file_exists(h->farmid, BAKDIR, nme, "tail4.new") ||
#endif
				file_exists(h->farmid, BAKDIR, nme, "tail2.new") ||
				file_exists(h->farmid, BAKDIR, nme, "tail1.new") ||
				file_exists(h->farmid, BAKDIR, nme, "tail") ||
#if SIZEOF_VAR_T == 8
				file_exists(h->farmid, BAKDIR, nme, "tail4") ||
#endif
				file_exists(h->farmid, BAKDIR, nme, "tail2") ||
				file_exists(h->farmid, BAKDIR, nme, "tail1");
		} else {
			exists = file_exists(h->farmid, BAKDIR, nme, "theap.new") ||
				file_exists(h->farmid, BAKDIR, nme, "theap");
		}

		strconcat_len(extnew, sizeof(extnew), ext, ".new", NULL);
		if (dirty && !exists) {
			/* if the heap is dirty and there is no heap
			 * file (with or without .new extension) in
			 * the BAKDIR, move the heap (preferably with
			 * .new extension) to the correct backup
			 * directory */
			if (istail) {
				if (file_exists(h->farmid, srcdir, nme, "tail.new"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail.new");
#if SIZEOF_VAR_T == 8
				else if (file_exists(h->farmid, srcdir, nme, "tail4.new"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail4.new");
#endif
				else if (file_exists(h->farmid, srcdir, nme, "tail2.new"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail2.new");
				else if (file_exists(h->farmid, srcdir, nme, "tail1.new"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail1.new");
				else if (file_exists(h->farmid, srcdir, nme, "tail"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail");
#if SIZEOF_VAR_T == 8
				else if (file_exists(h->farmid, srcdir, nme, "tail4"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail4");
#endif
				else if (file_exists(h->farmid, srcdir, nme, "tail2"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail2");
				else if (file_exists(h->farmid, srcdir, nme, "tail1"))
					mvret = heap_move(h, srcdir,
							  subcommit ? SUBDIR : BAKDIR,
							  nme, "tail1");
			} else if (file_exists(h->farmid, srcdir, nme, extnew))
				mvret = heap_move(h, srcdir,
						  subcommit ? SUBDIR : BAKDIR,
						  nme, extnew);
			else if (file_exists(h->farmid, srcdir, nme, ext))
				mvret = heap_move(h, srcdir,
						  subcommit ? SUBDIR : BAKDIR,
						  nme, ext);
		} else if (subcommit) {
			/* if subcommit, we may need to move an
			 * already made backup from BAKDIR to
			 * SUBDIR */
			if (file_exists(h->farmid, BAKDIR, nme, extnew))
				mvret = file_move(h->farmid, BAKDIR, SUBDIR, nme, extnew);
			else if (file_exists(h->farmid, BAKDIR, nme, ext))
				mvret = file_move(h->farmid, BAKDIR, SUBDIR, nme, ext);
		}
		/* there is a situation where the move may fail,
		 * namely if this heap was not supposed to be existing
		 * before, i.e. after a BATmaterialize on a persistent
		 * bat; as a workaround, do not complain about move
		 * failure if the source file is nonexistent
		 */
		if (mvret != GDK_SUCCEED && file_exists(h->farmid, srcdir, nme, ext)) {
			ret = GDK_FAIL;
		}
		if (subcommit &&
		    (h->storage == STORE_PRIV || h->newstorage == STORE_PRIV)) {
			long_str kill_ext;

			strconcat_len(kill_ext, sizeof(kill_ext),
				      ext, ".new.kill", NULL);
			if (file_exists(h->farmid, BAKDIR, nme, kill_ext) &&
			    file_move(h->farmid, BAKDIR, SUBDIR, nme, kill_ext) != GDK_SUCCEED) {
				ret = GDK_FAIL;
			}
		}
	}
	return ret;
}

static gdk_return
BBPbackup(BAT *b, bool subcommit)
{
	char *srcdir;
	long_str nme;
	const char *s = BBP_physical(b->batCacheid);
	size_t slen;
	bool locked = false;

	if (BBPprepare(subcommit) != GDK_SUCCEED) {
		return GDK_FAIL;
	}
	if (!b->batCopiedtodisk || b->batTransient) {
		return GDK_SUCCEED;
	}
	/* determine location dir and physical suffix */
	if (!(srcdir = GDKfilepath(NOFARM, BATDIR, s, NULL)))
		goto fail;
	s = strrchr(srcdir, DIR_SEP);
	if (!s)
		goto fail;

	slen = strlen(++s);
	if (slen >= sizeof(nme))
		goto fail;
	memcpy(nme, s, slen + 1);
	srcdir[s - srcdir] = 0;

	MT_lock_set(&b->theaplock);
	locked = true;
	if (b->ttype != TYPE_void &&
	    do_backup(srcdir, nme, gettailname(b), b->theap,
		      b->theap->dirty,
		      subcommit) != GDK_SUCCEED)
		goto fail;
	if (b->tvheap &&
	    do_backup(srcdir, nme, "theap", b->tvheap,
		      b->tvheap->dirty,
		      subcommit) != GDK_SUCCEED)
		goto fail;
	MT_lock_unset(&b->theaplock);
	GDKfree(srcdir);
	return GDK_SUCCEED;
  fail:
	if (locked)
		MT_lock_unset(&b->theaplock);
	if(srcdir)
		GDKfree(srcdir);
	return GDK_FAIL;
}

static inline void
BBPcheckHeap(bool subcommit, Heap *h)
{
	struct stat statb;
	char *path;

	if (subcommit) {
		char *s = strrchr(h->filename, DIR_SEP);
		if (s)
			s++;
		else
			s = h->filename;
		path = GDKfilepath(0, BAKDIR, s, NULL);
		if (path == NULL)
			return;
		if (MT_stat(path, &statb) < 0) {
			GDKfree(path);
			path = GDKfilepath(0, BATDIR, h->filename, NULL);
			if (path == NULL)
				return;
			if (MT_stat(path, &statb) < 0) {
				assert(0);
				GDKsyserror("cannot stat file %s (expected size %zu)\n",
					    path, h->free);
				GDKfree(path);
				return;
			}
		}
	} else {
		path = GDKfilepath(0, BATDIR, h->filename, NULL);
		if (path == NULL)
			return;
		if (MT_stat(path, &statb) < 0) {
			assert(0);
			GDKsyserror("cannot stat file %s (expected size %zu)\n",
				    path, h->free);
			GDKfree(path);
			return;
		}
	}
	assert((statb.st_mode & S_IFMT) == S_IFREG);
	assert((size_t) statb.st_size >= h->free);
	if ((size_t) statb.st_size < h->free) {
		GDKerror("file %s too small (expected %zu, actual %zu)\n", path, h->free, (size_t) statb.st_size);
		GDKfree(path);
		return;
	}
	GDKfree(path);
}

static void
BBPcheckBBPdir(bool subcommit)
{
	FILE *fp;
	int lineno = 0;
	bat bbpsize = 0;
	unsigned bbpversion;
	lng logno, transid;

	fp = GDKfileopen(0, BATDIR, "BBP", "dir", "r");
	assert(fp != NULL);
	if (fp == NULL)
		return;
	bbpversion = BBPheader(fp, &lineno, &bbpsize, &logno, &transid);
	if (bbpversion == 0) {
		fclose(fp);
		return;		/* error reading file */
	}
	assert(bbpversion == GDKLIBRARY);

	for (;;) {
		BAT b;
		Heap h;
		Heap vh;
		vh = h = (Heap) {
			.free = 0,
		};
		b = (BAT) {
			.theap = &h,
			.tvheap = &vh,
		};
		char *options;
		char filename[sizeof(BBP_physical(0))];
		char batname[129];
		int hashash;

		switch (BBPreadBBPline(fp, bbpversion, &lineno, &b,
				       &hashash,
				       batname, filename, &options)) {
		case 0:
			/* end of file */
			fclose(fp);
			/* don't leak errors, this is just debug code */
			GDKclrerr();
			return;
		case 1:
			/* successfully read an entry */
			break;
		default:
			/* error */
			fclose(fp);
			return;
		}
		assert(b.batCacheid < (bat) ATOMIC_GET(&BBPsize));
		assert(BBP_desc(b.batCacheid) != NULL);
		assert(b.hseqbase <= GDK_oid_max);
		if (b.ttype == TYPE_void) {
			/* no files needed */
			continue;
		}
		if (b.theap->free > 0)
			BBPcheckHeap(subcommit, b.theap);
		if (b.tvheap != NULL && b.tvheap->free > 0)
			BBPcheckHeap(subcommit, b.tvheap);
	}
}

/*
 * @+ Atomic Write
 * The atomic BBPsync() function first safeguards the old images of
 * all files to be written in BAKDIR. It then saves all files. If that
 * succeeds fully, BAKDIR is renamed to DELDIR. The rename is
 * considered an atomic action. If it succeeds, the DELDIR is removed.
 * If something fails, the pre-sync status can be obtained by moving
 * back all backed up files; this is done by BBPrecover().
 *
 * The BBP.dir is also moved into the BAKDIR.
 */
gdk_return
BBPsync(int cnt, bat *restrict subcommit, BUN *restrict sizes, lng logno, lng transid)
{
	gdk_return ret = GDK_SUCCEED;
	int t0 = 0, t1 = 0;
	str bakdir, deldir;
	const bool lock = locked_by == 0 || locked_by != MT_getpid();
	char buf[3000];
	int n = subcommit ? 0 : -1;
	FILE *obbpf, *nbbpf;

	if(!(bakdir = GDKfilepath(0, NULL, subcommit ? SUBDIR : BAKDIR, NULL)))
		return GDK_FAIL;
	if(!(deldir = GDKfilepath(0, NULL, DELDIR, NULL))) {
		GDKfree(bakdir);
		return GDK_FAIL;
	}

	TRC_DEBUG_IF(PERF) t0 = t1 = GDKms();

	ret = BBPprepare(subcommit != NULL);

	/* PHASE 1: safeguard everything in a backup-dir */
	if (ret == GDK_SUCCEED) {
		int idx = 0;

		while (++idx < cnt) {
			bat i = subcommit ? subcommit[idx] : idx;
			if (lock)
				MT_lock_set(&GDKswapLock(i));
			/* set flag that we're syncing, i.e. that we'll
			 * be between moving heap to backup dir and
			 * saving the new version, in other words, the
			 * heap may not exist in the usual location */
			BBP_status_on(i, BBPSYNCING);
			/* wait until unloading is finished before
			 * attempting to make a backup */
			while (BBP_status(i) & BBPUNLOADING) {
				if (lock)
					MT_lock_unset(&GDKswapLock(i));
				BBPspin(i, __func__, BBPUNLOADING);
				if (lock)
					MT_lock_set(&GDKswapLock(i));
			}
			BAT *b = dirty_bat(&i, subcommit != NULL);
			if (i <= 0) {
				if (lock)
					MT_lock_unset(&GDKswapLock(subcommit ? subcommit[idx] : idx));
				break;
			}
			if (BBP_status(i) & BBPEXISTING) {
				if (b != NULL && b->batInserted > 0) {
					if (BBPbackup(b, subcommit != NULL) != GDK_SUCCEED) {
						if (lock)
							MT_lock_unset(&GDKswapLock(i));
						break;
					}
				}
			} else {
				if (subcommit && (b = BBP_desc(i)) && BBP_status(i) & BBPDELETED) {
					char o[10];
					char *f;
					snprintf(o, sizeof(o), "%o", (unsigned) b->batCacheid);
					f = GDKfilepath(b->theap->farmid, BAKDIR, o, gettailname(b));
					if (f == NULL) {
						if (lock)
							MT_lock_unset(&GDKswapLock(i));
						ret = GDK_FAIL;
						goto bailout;
					}
					if (MT_access(f, F_OK) == 0)
						file_move(b->theap->farmid, BAKDIR, SUBDIR, o, gettailname(b));
					GDKfree(f);
					f = GDKfilepath(b->theap->farmid, BAKDIR, o, "theap");
					if (f == NULL) {
						if (lock)
							MT_lock_unset(&GDKswapLock(i));
						ret = GDK_FAIL;
						goto bailout;
					}
					if (MT_access(f, F_OK) == 0)
						file_move(b->theap->farmid, BAKDIR, SUBDIR, o, "theap");
					GDKfree(f);
				}
			}
			if (lock)
				MT_lock_unset(&GDKswapLock(i));
		}
		if (idx < cnt)
			ret = GDK_FAIL;
	}
	TRC_DEBUG(PERF, "move time %d, %d files\n", (t1 = GDKms()) - t0, backup_files);

	/* PHASE 2: save the repository and write new BBP.dir file */
	if (ret == GDK_SUCCEED) {
		ret = BBPdir_first(subcommit != NULL, logno, transid,
				   &obbpf, &nbbpf);
	}

	for (int idx = 1; ret == GDK_SUCCEED && idx < cnt; idx++) {
		bat i = subcommit ? subcommit[idx] : idx;
		/* BBP_desc(i) may be NULL */
		BUN size = sizes ? sizes[idx] : BUN_NONE;
		BATiter bi;
		oid minpos = oid_nil, maxpos = oid_nil;

		if (BBP_status(i) & BBPPERSISTENT) {
			BAT *b = dirty_bat(&i, subcommit != NULL);
			if (i <= 0) {
				ret = GDK_FAIL;
				break;
			}
			MT_lock_set(&BBP_desc(i)->theaplock);
			bi = bat_iterator_nolock(BBP_desc(i));
			HEAPincref(bi.h);
			if (bi.vh)
				HEAPincref(bi.vh);
#ifndef NDEBUG
			bi.locked = true;
#endif
			assert(sizes == NULL || size <= bi.count);
			assert(sizes == NULL || bi.width == 0 || (bi.type == TYPE_msk ? ((size + 31) / 32) * 4 : size << bi.shift) <= bi.hfree);
			if (size > bi.count) /* includes sizes==NULL */
				size = bi.count;
			bi.b->batInserted = size;
			if (size == 0) {
				/* no need to save anything */
				MT_lock_unset(&bi.b->theaplock);
			} else {
				const ValRecord *prop;
				prop = BATgetprop_nolock(bi.b, GDK_MIN_POS);
				if (prop)
					minpos = prop->val.oval;
				prop = BATgetprop_nolock(bi.b, GDK_MAX_POS);
				if (prop)
					maxpos = prop->val.oval;
				MT_lock_unset(&bi.b->theaplock);
				if (b) {
					/* wait for BBPSAVING so
					 * that we can set it,
					 * wait for BBPUNLOADING
					 * before attempting to
					 * save */
					for (;;) {
						if (lock)
							MT_lock_set(&GDKswapLock(i));
						if (!(BBP_status(i) & (BBPSAVING|BBPUNLOADING)))
							break;
						if (lock)
							MT_lock_unset(&GDKswapLock(i));
						BBPspin(i, __func__, BBPSAVING|BBPUNLOADING);
					}
					BBP_status_on(i, BBPSAVING);
					if (lock)
						MT_lock_unset(&GDKswapLock(i));
					ret = BATsave_iter(b, &bi, size);
					BBP_status_off(i, BBPSAVING);
				}
			}
		} else {
			bi = bat_iterator(NULL);
		}
		if (ret == GDK_SUCCEED) {
			n = BBPdir_step(i, size, n, buf, sizeof(buf), &obbpf, nbbpf, &bi, (BUN) minpos, (BUN) maxpos);
			if (n < -1)
				ret = GDK_FAIL;
		}
		bat_iterator_end(&bi);
		/* we once again have a saved heap */
	}

	TRC_DEBUG(PERF, "write time %d\n", (t0 = GDKms()) - t1);

	if (ret == GDK_SUCCEED) {
		ret = BBPdir_last(n, buf, sizeof(buf), obbpf, nbbpf);
	}

	TRC_DEBUG(PERF, "dir time %d, %d bats\n", (t1 = GDKms()) - t0, (bat) ATOMIC_GET(&BBPsize));

	if (ret == GDK_SUCCEED) {
		/* atomic switchover */
		/* this is the big one: this call determines
		 * whether the operation of this function
		 * succeeded, so no changing of ret after this
		 * call anymore */

		if ((GDKdebug & TAILCHKMASK) && !GDKinmemory(0))
			BBPcheckBBPdir(subcommit != NULL);

		if (MT_rename(bakdir, deldir) < 0 &&
		    /* maybe there was an old deldir, so remove and try again */
		    (GDKremovedir(0, DELDIR) != GDK_SUCCEED ||
		     MT_rename(bakdir, deldir) < 0))
			ret = GDK_FAIL;
		if (ret != GDK_SUCCEED)
			GDKsyserror("rename(%s,%s) failed.\n", bakdir, deldir);
		TRC_DEBUG(IO_, "rename %s %s = %d\n", bakdir, deldir, (int) ret);
	}

	/* AFTERMATH */
	if (ret == GDK_SUCCEED) {
		ATOMIC_SET(&BBPlogno, logno);	/* the new value */
		ATOMIC_SET(&BBPtransid, transid);
		backup_files = subcommit ? (backup_files - backup_subdir) : 0;
		backup_dir = backup_subdir = 0;
		if (GDKremovedir(0, DELDIR) != GDK_SUCCEED)
			fprintf(stderr, "#BBPsync: cannot remove directory %s\n", DELDIR);
		(void) BBPprepare(false); /* (try to) remove DELDIR and set up new BAKDIR */
		if (backup_files > 1) {
			TRC_DEBUG(PERF, "backup_files %d > 1\n", backup_files);
			backup_files = 1;
		}
	}
	TRC_DEBUG(PERF, "%s (ready time %d)\n",
		  ret == GDK_SUCCEED ? "" : " failed",
		  (t0 = GDKms()) - t1);
  bailout:
	/* turn off the BBPSYNCING bits for all bats, even when things
	 * didn't go according to plan (i.e., don't check for ret ==
	 * GDK_SUCCEED) */
	for (int idx = 1; idx < cnt; idx++) {
		bat i = subcommit ? subcommit[idx] : idx;
		BBP_status_off(i, BBPSYNCING);
	}

	GDKfree(bakdir);
	GDKfree(deldir);
	return ret;
}

/*
 * Recovery just moves all files back to their original location. this
 * is an incremental process: if something fails, just stop with still
 * files left for moving in BACKUP/.  The recovery process can resume
 * later with the left over files.
 */
static gdk_return
force_move(int farmid, const char *srcdir, const char *dstdir, const char *name)
{
	const char *p;
	char *dstpath, *killfile;
	gdk_return ret = GDK_SUCCEED;

	if ((p = strrchr(name, '.')) != NULL && strcmp(p, ".kill") == 0) {
		/* Found a X.new.kill file, ie remove the X.new file */
		ptrdiff_t len = p - name;
		long_str srcpath;

		strncpy(srcpath, name, len);
		srcpath[len] = '\0';
		if(!(dstpath = GDKfilepath(farmid, dstdir, srcpath, NULL))) {
			return GDK_FAIL;
		}

		/* step 1: remove the X.new file that is going to be
		 * overridden by X */
		if (MT_remove(dstpath) != 0 && errno != ENOENT) {
			/* if it exists and cannot be removed, all
			 * this is going to fail */
			GDKsyserror("force_move: remove(%s)\n", dstpath);
			GDKfree(dstpath);
			return GDK_FAIL;
		}
		GDKfree(dstpath);

		/* step 2: now remove the .kill file. This one is
		 * crucial, otherwise we'll never finish recovering */
		if(!(killfile = GDKfilepath(farmid, srcdir, name, NULL))) {
			return GDK_FAIL;
		}
		if (MT_remove(killfile) != 0) {
			ret = GDK_FAIL;
			GDKsyserror("force_move: remove(%s)\n", killfile);
		}
		GDKfree(killfile);
		return ret;
	}
	/* try to rename it */
	ret = GDKmove(farmid, srcdir, name, NULL, dstdir, name, NULL, false);

	if (ret != GDK_SUCCEED) {
		char *srcpath;

		/* two legal possible causes: file exists or dir
		 * doesn't exist */
		if(!(dstpath = GDKfilepath(farmid, dstdir, name, NULL)))
			return GDK_FAIL;
		if(!(srcpath = GDKfilepath(farmid, srcdir, name, NULL))) {
			GDKfree(dstpath);
			return GDK_FAIL;
		}
		if (MT_remove(dstpath) != 0)	/* clear destination */
			ret = GDK_FAIL;
		TRC_DEBUG(IO_, "remove %s = %d\n", dstpath, (int) ret);

		(void) GDKcreatedir(dstdir); /* if fails, move will fail */
		ret = GDKmove(farmid, srcdir, name, NULL, dstdir, name, NULL, true);
		TRC_DEBUG(IO_, "link %s %s = %d\n", srcpath, dstpath, (int) ret);
		GDKfree(dstpath);
		GDKfree(srcpath);
	}
	return ret;
}

gdk_return
BBPrecover(int farmid)
{
	str bakdirpath;
	str leftdirpath;
	DIR *dirp;
	struct dirent *dent;
	long_str path, dstpath;
	bat i;
	size_t j = strlen(BATDIR);
	gdk_return ret = GDK_SUCCEED;
	bool dirseen = false;
	str dstdir;

	bakdirpath = GDKfilepath(farmid, NULL, BAKDIR, NULL);
	leftdirpath = GDKfilepath(farmid, NULL, LEFTDIR, NULL);
	if (bakdirpath == NULL || leftdirpath == NULL) {
		GDKfree(bakdirpath);
		GDKfree(leftdirpath);
		return GDK_FAIL;
	}
	dirp = opendir(bakdirpath);
	if (dirp == NULL) {
		if (errno != ENOENT)
			GDKsyserror("cannot open directory %s\n", bakdirpath);
		GDKfree(bakdirpath);
		GDKfree(leftdirpath);
		return GDK_SUCCEED;	/* nothing to do */
	}
	memcpy(dstpath, BATDIR, j);
	dstpath[j] = DIR_SEP;
	dstpath[++j] = 0;
	dstdir = dstpath + j;
	TRC_DEBUG(IO_, "start\n");

	if (MT_mkdir(leftdirpath) < 0 && errno != EEXIST) {
		GDKsyserror("cannot create directory %s\n", leftdirpath);
		closedir(dirp);
		GDKfree(bakdirpath);
		GDKfree(leftdirpath);
		return GDK_FAIL;
	}

	/* move back all files */
	while ((dent = readdir(dirp)) != NULL) {
		const char *q = strchr(dent->d_name, '.');

		if (q == dent->d_name) {
			char *fn;

			if (strcmp(dent->d_name, ".") == 0 ||
			    strcmp(dent->d_name, "..") == 0)
				continue;
			fn = GDKfilepath(farmid, BAKDIR, dent->d_name, NULL);
			if (fn) {
				int uret = MT_remove(fn);
				TRC_DEBUG(IO_, "remove %s = %d\n",
					  fn, uret);
				GDKfree(fn);
			}
			continue;
		} else if (strcmp(dent->d_name, "BBP.dir") == 0) {
			dirseen = true;
			continue;
		}
		if (q == NULL)
			q = dent->d_name + strlen(dent->d_name);
		if ((j = q - dent->d_name) + 1 > sizeof(path)) {
			/* name too long: ignore */
			continue;
		}
		strncpy(path, dent->d_name, j);
		path[j] = 0;
		if (GDKisdigit(*path)) {
			i = strtol(path, NULL, 8);
		} else {
			i = BBP_find(path, false);
			if (i < 0)
				i = -i;
		}
		if (i == 0 || i >= (bat) ATOMIC_GET(&BBPsize) || !BBPvalid(i)) {
			force_move(farmid, BAKDIR, LEFTDIR, dent->d_name);
		} else {
			BBPgetsubdir(dstdir, i);
			if (force_move(farmid, BAKDIR, dstpath, dent->d_name) != GDK_SUCCEED)
				ret = GDK_FAIL;
		}
	}
	closedir(dirp);
	if (dirseen && ret == GDK_SUCCEED) {	/* we have a saved BBP.dir; it should be moved back!! */
		struct stat st;
		char *fn;

		fn = GDKfilepath(farmid, BATDIR, "BBP", "dir");
		if (fn == NULL) {
			ret = GDK_FAIL;
		} else {
			ret = recover_dir(farmid, MT_stat(fn, &st) == 0);
			GDKfree(fn);
		}
	}

	if (ret == GDK_SUCCEED) {
		if (MT_rmdir(bakdirpath) < 0) {
			GDKsyserror("cannot remove directory %s\n", bakdirpath);
			ret = GDK_FAIL;
		}
		TRC_DEBUG(IO_, "rmdir %s = %d\n", bakdirpath, (int) ret);
	}
	if (ret != GDK_SUCCEED)
		GDKerror("recovery failed.\n");

	TRC_DEBUG(IO_, "end\n");
	GDKfree(bakdirpath);
	GDKfree(leftdirpath);
	return ret;
}

/*
 * SUBDIR recovery is quite mindlessly moving all files back to the
 * parent (BAKDIR).  We do recognize moving back BBP.dir and set
 * backed_up_subdir accordingly.
 */
gdk_return
BBPrecover_subdir(void)
{
	str subdirpath;
	DIR *dirp;
	struct dirent *dent;
	gdk_return ret = GDK_SUCCEED;

	subdirpath = GDKfilepath(0, NULL, SUBDIR, NULL);
	if (subdirpath == NULL)
		return GDK_FAIL;
	dirp = opendir(subdirpath);
	if (dirp == NULL && errno != ENOENT)
		GDKsyserror("cannot open directory %s\n", subdirpath);
	GDKfree(subdirpath);
	if (dirp == NULL) {
		return GDK_SUCCEED;	/* nothing to do */
	}
	TRC_DEBUG(IO_, "start\n");

	/* move back all files */
	while ((dent = readdir(dirp)) != NULL) {
		if (dent->d_name[0] == '.')
			continue;
		ret = GDKmove(0, SUBDIR, dent->d_name, NULL, BAKDIR, dent->d_name, NULL, true);
		if (ret == GDK_SUCCEED && strcmp(dent->d_name, "BBP.dir") == 0)
			backup_dir = 1;
		if (ret != GDK_SUCCEED)
			break;
	}
	closedir(dirp);

	/* delete the directory */
	if (ret == GDK_SUCCEED) {
		ret = GDKremovedir(0, SUBDIR);
		if (backup_dir == 2) {
			TRC_DEBUG(IO_, "%s%cBBP.dir had disappeared!\n", SUBDIR, DIR_SEP);
			backup_dir = 0;
		}
	}
	TRC_DEBUG(IO_, "end = %d\n", (int) ret);

	if (ret != GDK_SUCCEED)
		GDKerror("recovery failed.\n");
	return ret;
}

/*
 * @- The diskscan
 * The BBPdiskscan routine walks through the BAT dir, cleans up
 * leftovers, and measures disk occupancy.  Leftovers are files that
 * cannot belong to a BAT. in order to establish this for [ht]heap
 * files, the BAT descriptor is loaded in order to determine whether
 * these files are still required.
 *
 * The routine gathers all bat sizes in a bat that contains bat-ids
 * and bytesizes. The return value is the number of bytes of space
 * freed.
 */
static bool
persistent_bat(bat bid)
{
	if (bid >= 0 && bid < (bat) ATOMIC_GET(&BBPsize) && BBPvalid(bid)) {
		BAT *b = BBP_cache(bid);

		if (b == NULL || b->batCopiedtodisk) {
			return true;
		}
	}
	return false;
}

static BAT *
getdesc(bat bid)
{
	BAT *b = NULL;

	if (is_bat_nil(bid))
		return NULL;
	assert(bid > 0);
	if (bid < (bat) ATOMIC_GET(&BBPsize) && BBP_logical(bid))
		b = BBP_desc(bid);
	if (b == NULL)
		BBPclear(bid, true);
	return b;
}

static bool
BBPdiskscan(const char *parent, size_t baseoff)
{
	DIR *dirp = opendir(parent);
	struct dirent *dent;
	char fullname[FILENAME_MAX];
	str dst = fullname;
	size_t dstlen = sizeof(fullname);
	const char *src = parent;

	if (dirp == NULL) {
		if (errno != ENOENT)
			GDKsyserror("cannot open directory %s\n", parent);
		return true;	/* nothing to do */
	}

	while (*src) {
		*dst++ = *src++;
		dstlen--;
	}
	if (dst > fullname && dst[-1] != DIR_SEP) {
		*dst++ = DIR_SEP;
		dstlen--;
	}

	while ((dent = readdir(dirp)) != NULL) {
		const char *p;
		bat bid;
		bool ok, delete;

		if (dent->d_name[0] == '.')
			continue;	/* ignore .dot files and directories (. ..) */

		if (strncmp(dent->d_name, "BBP.", 4) == 0 &&
		    (strcmp(parent + baseoff, BATDIR) == 0 ||
		     strncmp(parent + baseoff, BAKDIR, strlen(BAKDIR)) == 0 ||
		     strncmp(parent + baseoff, SUBDIR, strlen(SUBDIR)) == 0))
			continue;

		p = strchr(dent->d_name, '.');

		if (strlen(dent->d_name) >= dstlen) {
			/* found a file with too long a name
			   (i.e. unknown); stop pruning in this
			   subdir */
			fprintf(stderr, "unexpected file %s, leaving %s.\n", dent->d_name, parent);
			break;
		}
		strncpy(dst, dent->d_name, dstlen);
		fullname[sizeof(fullname) - 1] = 0;

		if (p == NULL && !BBPdiskscan(fullname, baseoff)) {
			/* it was a directory */
			continue;
		}

		if (p && strcmp(p + 1, "tmp") == 0) {
			delete = true;
			ok = true;
			bid = 0;
		} else {
			bid = strtol(dent->d_name, NULL, 8);
			ok = p && bid;
			delete = false;

			if (!ok || !persistent_bat(bid)) {
				delete = true;
			} else if (strncmp(p + 1, "tail", 4) == 0) {
				BAT *b = getdesc(bid);
				delete = (b == NULL || !b->ttype || !b->batCopiedtodisk);
				if (!delete) {
					if (b->ttype == TYPE_str) {
						switch (b->twidth) {
						case 1:
							delete = strcmp(p + 1, "tail1") != 0;
							break;
						case 2:
							delete = strcmp(p + 1, "tail2") != 0;
							break;
#if SIZEOF_VAR_T == 8
						case 4:
							delete = strcmp(p + 1, "tail4") != 0;
							break;
#endif
						default:
							delete = strcmp(p + 1, "tail") != 0;
							break;
						}
					} else {
						delete = strcmp(p + 1, "tail") != 0;
					}
				}
			} else if (strncmp(p + 1, "theap", 5) == 0) {
				BAT *b = getdesc(bid);
				delete = (b == NULL || !b->tvheap || !b->batCopiedtodisk);
			} else if (strncmp(p + 1, "thashl", 6) == 0 ||
				   strncmp(p + 1, "thashb", 6) == 0) {
#ifdef PERSISTENTHASH
				BAT *b = getdesc(bid);
				delete = b == NULL;
				if (!delete)
					b->thash = (Hash *) 1;
#else
				delete = true;
#endif
			} else if (strncmp(p + 1, "thash", 5) == 0) {
				/* older versions used .thash which we
				 * can simply ignore */
				delete = true;
			} else if (strncmp(p + 1, "thsh", 4) == 0) {
				/* temporary hash files which we can
				 * simply ignore */
				delete = true;
			} else if (strncmp(p + 1, "timprints", 9) == 0) {
				BAT *b = getdesc(bid);
				delete = b == NULL;
				if (!delete)
					b->timprints = (Imprints *) 1;
			} else if (strncmp(p + 1, "torderidx", 9) == 0) {
#ifdef PERSISTENTIDX
				BAT *b = getdesc(bid);
				delete = b == NULL;
				if (!delete)
					b->torderidx = (Heap *) 1;
#else
				delete = true;
#endif
			} else if (strncmp(p + 1, "new", 3) != 0) {
				ok = false;
			}
		}
		if (!ok) {
			/* found an unknown file; stop pruning in this
			 * subdir */
			fprintf(stderr, "unexpected file %s, leaving %s.\n", dent->d_name, parent);
			break;
		}
		if (delete) {
			if (MT_remove(fullname) != 0 && errno != ENOENT) {
				GDKsyserror("remove(%s)", fullname);
				continue;
			}
			TRC_DEBUG(IO_, "remove(%s) = 0\n", fullname);
		}
	}
	closedir(dirp);
	return false;
}

void
gdk_bbp_reset(void)
{
	int i;

	for (i = 0; i <= BBP_THREADMASK; i++) {
		GDKbbpLock[i].free = 0;
	}
	while (BBPlimit > 0) {
		BBPlimit -= BBPINIT;
		assert(BBPlimit >= 0);
		GDKfree(BBP[BBPlimit >> BBPINITLOG]);
		BBP[BBPlimit >> BBPINITLOG] = NULL;
	}
	ATOMIC_SET(&BBPsize, 0);
	for (i = 0; i < MAXFARMS; i++)
		GDKfree((void *) BBPfarms[i].dirname); /* loose "const" */
	memset(BBPfarms, 0, sizeof(BBPfarms));
	GDKfree(BBP_hash);
	BBP_hash = NULL;
	BBP_mask = 0;

	locked_by = 0;
	BBPunloadCnt = 0;
	backup_files = 0;
	backup_dir = 0;
	backup_subdir = 0;
}
