/*
 * Copyright (C) 2018 Michael Neuling <mikey@linux.ibm.com>, IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "tlb.h"

#define TLB_SIZE 256
#define TLB_FLAGS_AVALIABLE (TLB_FLAGS_RELOC)
struct tlbe {
	uint64_t ea;
	uint64_t ra;
	uint64_t size;
	uint64_t flags;
	uint64_t hit_count;
	uint64_t miss_count;
	bool valid;
};
struct tlb_cache {
	struct tlbe tlb[TLB_SIZE];
	int next;
	int translations;
	int no_translation;
	int translation_changes;
};
struct tlb_cache tlb;

int tlb_debug = 0;

static inline uint64_t tlb_mask_offset(struct tlbe *t)
{
	return t->size - 1;
}

static inline uint64_t tlb_mask_rpn(struct tlbe *t)
{
	return ~(tlb_mask_offset(t));
}

static inline void tlb_pagesize_validate(uint64_t size)
{
	assert((size == 4096) || (size == 65536) || (size == 16777216));
}

static inline void tlb_flags_validate(uint64_t flags)
{
	assert((flags & ~(TLB_FLAGS_AVALIABLE)) == 0);
}

static bool tlb_equal(struct tlbe *t1, struct tlbe *t2)
{
	if (t1->ea != t2->ea)
		return false;
	if (t1->ra != t2->ra)
		return false;
	if (t1->size != t2->size)
		return false;
	if (t1->flags != t2->flags)
		return false;
	if (t1->valid != t2->valid)
		return false;
	/* Don't check count */

	return true;
}

static inline void tlb_entry_validate(struct tlbe *t)
{
	uint64_t mask;

	assert(t->valid);
	tlb_pagesize_validate(t->size);
	tlb_flags_validate(t->flags);
	mask = tlb_mask_offset(t);
	assert((t->ea & mask) == 0);
	assert((t->ra & mask) == 0);
}

static inline void tlb_print(struct tlbe *t)
{
	printf("ea:%016"PRIx64" ra:%016"PRIx64" size:%08"PRIx64" "
	       "flags:%"PRIx64" miss:%"PRIi64" hit:%"PRIi64"\n",
	       t->ea, t->ra, t->size, t->flags, t->miss_count, t->hit_count);
}

void tlb_dump(void)
{
	int i;

	for (i = 0; i < tlb.next; i++) {
		printf("TLBDUMP %02i: ", i);
		tlb_print(&tlb.tlb[i]);
	}
	printf("TLBDUMP no translation: %i of %i\n",
	       tlb.no_translation, tlb.translations);
	printf("TLBDUMP replaced translations: %i\n",
	       tlb.translation_changes);
}

static inline bool tlb_match(uint64_t ea, uint64_t flags, struct tlbe *t)
{
	tlb_entry_validate(t);

	if (tlb_debug > 0) {
		printf("%s ea:%016"PRIx64" flags:%"PRIx64" ",
		       __func__, ea, flags);
		tlb_print(t);
	}

	if (ea < t->ea)
		return false;
	if (ea >= (t->ea + t->size))
		return false;
	if (flags != t->flags)
		return false;

	return true;
}


static inline int __tlb_get_index(uint64_t ea, uint64_t flags, int start)
{
	struct tlbe *t;
	int i;

	/* FIXME: linear search... *barf* */
	for (i = start; i < tlb.next; i++) {
		t = &tlb.tlb[i];
		if (tlb_match(ea, flags, t)) {
			tlb_entry_validate(t);
			/* This hit in the hardware hence we had to do
			 * the translation
			 */
			t->hit_count++;
			return i;
		}
	}
	return -1;
}

static inline int tlb_get_index(uint64_t ea, uint64_t flags)
{
	return __tlb_get_index(ea, flags, 0);
}

static inline void tlb_validate(void)
{
	struct tlbe *t;
	int i;
	bool valid_last;

	assert(tlb.next <= TLB_SIZE);

	/* Check for overlaps */
	for (i = 0; i < tlb.next; i++) {
		t = &tlb.tlb[i];
		/* Check this ea doesn't match other entries */
		/* Check start of page */
		assert(__tlb_get_index(t->ea, t->flags, i + 1) == -1);
		/* Check end page */
		assert(__tlb_get_index(t->ea + t->size - 1, t->flags, i + 1)
		       == -1);
	}

	/* Check for holes */
	valid_last = true;
	for (i = 0; i < TLB_SIZE; i++) {
		t = &tlb.tlb[i];
		assert(!t->valid || valid_last);
		valid_last = t->valid;
	}
}

static inline uint64_t tlb_translate(uint64_t ea, uint64_t flags,
				     struct tlbe *t)
{
	uint64_t ra;

	/* Double check this is a match */
	assert(ea >= t->ea);
	assert(ea < (t->ea + t->size));
	/* Other checks */
	tlb_flags_validate(flags); /* flags unused other than this check */
	tlb_entry_validate(t);

	/* Actual translation */
	ra = ea & tlb_mask_offset(t);
	ra |= t->ra & tlb_mask_rpn(t);

	return ra;
}

void tlb_init(void)
{
	memset(&tlb, 0, sizeof(tlb));
	tlb_validate();
}

void tlb_exit(void)
{
	tlb_validate();
}

bool tlb_ra_get(uint64_t ea, uint64_t flags,
		uint64_t *ra, uint64_t *pagesize)
{
	struct tlbe *t;
	int index;

	assert(ra);
	assert(pagesize);

	tlb.translations++;
	/* Find entry */
	index = tlb_get_index(ea, flags);
	if (index < 0) {
		tlb.no_translation++;
		return false;
	}

	/* Get entry */
	t = &tlb.tlb[index];

	/* Do translation */
	*ra = tlb_translate(ea, flags, t);
	*pagesize = t->size;

	return true;
}

/*
 * Set a new entry.
 * If old entry exists, delete it
 */
void tlb_ra_set(uint64_t ea, uint64_t flags,
		uint64_t ra, uint64_t pagesize)
{
	struct tlbe *t;
	struct tlbe tnew;
	int index;

//	tlb_debug = 1;


	tlb_pagesize_validate(pagesize);
	tlb_flags_validate(flags);

	index = tlb_get_index(ea, flags);
	if (index < 0) {
		/* No entry found, so put it at the end */
		index = tlb.next;
		tlb.next++;
		assert(tlb.next <= TLB_SIZE);
	}
	tlb_debug = 0;

	t = &tlb.tlb[index];

	/* Generate new entry */
	tnew.size = pagesize;
	tnew.ea = ea & tlb_mask_rpn(&tnew);
	tnew.ra = ra & tlb_mask_rpn(&tnew);
	tnew.flags = flags;
	tnew.valid = true;

	if (tlb_equal(&tnew, t)) {
		/* This missed in the hardware */
		t->miss_count++;
		return;
	} else if (t->valid) { /* new entry */
		/* Same RA but different RA */
		tlb.translation_changes++;
		/*
		printf("TLB different: %i\n", index);
		printf("TLB Existing: "); tlb_print(t);
		printf("TLB New:      "); tlb_print(&tnew);
		*/
	}

	/* Set entry */
	memcpy(t, &tnew, sizeof(tnew));

	/* Check if we've screwed something up  */
	tlb_validate();
}
