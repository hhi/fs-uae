
/* Real hardware UAE state file loader */
/* Copyright 2019 Toni Wilen */

#define VER "0.2"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <graphics/gfxbase.h>
#include <dos/dosextens.h>
#include <hardware/cia.h>
#include <hardware/custom.h>

#include "header.h"

extern struct GfxBase *GfxBase;
extern struct DosLibrary *DosBase;

static const UBYTE *const version = "$VER: uaestateload " VER " (" REVDATE ")";

static const char *const chunknames[] =
{
	"ASF ",
	"CPU ", "CHIP", "AGAC",
	"CIAA", "CIAB", "ROM ",
	"DSK0", "DSK1", "DSK2", "DSK3",
	"AUD0", "AUD1", "AUD2", "AUD3",
	"END ",
	NULL
};
static const char *const memchunknames[] =
{
	"CRAM", "BRAM", "FRAM",
	NULL
};
static const char *const unsupportedchunknames[] =
{
	"FRA2", "FRA3", "FRA4",
	"ZRA2", "ZRA3", "ZRA4",
	"ZCRM", "PRAM",
	"A3K1", "A3K2",
	"BORO", "P96 ",
	"FSYS",
	NULL
};

static ULONG getlong(UBYTE *chunk, int offset)
{
	ULONG v;
	
	chunk += offset;
	v = (chunk[0] << 24) | (chunk[1] << 16) | (chunk[2] << 8) | (chunk[3] << 0);
	return v;
}
static ULONG getword(UBYTE *chunk, int offset)
{
	ULONG v;
	
	chunk += offset;
	v = (chunk[0] << 8) | (chunk[1] << 0);
	return v;
}

static void set_agacolor(UBYTE *p)
{
	volatile struct Custom *c = (volatile struct Custom*)0xdff000;

	int aga = (c->vposr & 0x0f00) == 0x0300;
	if (!aga)
		return;
	
	for (int i = 0; i < 8; i++) {
		for (int k = 0; k < 2; k++) {
			c->bplcon3 = (i << 13) | (k ? (1 << 9) : 0);
			for (int j = 0; j < 32; j++) {
				ULONG c32 = getlong(p, (j + i * 32) * 4);
				if (!k)
					c32 >>= 4;
				// R1R2G1G2B1B2 -> R2G2B2
				UWORD col = ((c32 & 0x00000f) << 0) | ((c32 & 0x000f00) >> 4) | ((c32 & 0x0f0000) >> 8);
				if (!k && (c32 & 0x80000000))
					col |= 0x8000; // genlock transparency bit
				c->color[j] = col;
			}			
		}
	}
	c->bplcon3 = 0x0c00;
}

static void wait_lines(WORD lines)
{
	volatile struct Custom *c = (volatile struct Custom*)0xdff000;

	UWORD line = c->vhposr & 0xff00;
	while (lines-- > 0) {
		for (;;) {
			UWORD line2 = c->vhposr & 0xff00;
			if (line == line2)
				continue;
			line = line2;
			break;
		}
	}
}

static void step_floppy(void)
{
	volatile struct CIA *ciab = (volatile struct CIA*)0xbfd000;
	ciab->ciaprb &= ~CIAF_DSKSTEP;
	// delay
	ciab->ciaprb &= ~CIAF_DSKSTEP;
	ciab->ciaprb |= CIAF_DSKSTEP;
	wait_lines(200);
}

static void set_floppy(UBYTE *p, ULONG num)
{
	ULONG id = getlong(p, 0);
	UBYTE state = p[4];
	UBYTE track = p[5];

 	// drive disabled?
	if (state & 2)
		return;
	// invalid track?
	if (track >= 80)
		return;

	volatile struct CIA *ciaa = (volatile struct CIA*)0xbfe001;
	volatile struct CIA *ciab = (volatile struct CIA*)0xbfd000;

	ciab->ciaprb = 0xff;
	
	// motor on?
	if (state & 1) {
		ciab->ciaprb &= ~CIAF_DSKMOTOR;
	}
	// select drive
	ciab->ciaprb &= ~(CIAF_DSKSEL0 << num);

	wait_lines(100);
	int seekcnt = 80;
	while (seekcnt-- > 0) {
		if (!(ciaa->ciapra & CIAF_DSKTRACK0))
			break;
		step_floppy();
	}
	wait_lines(100);
	if (seekcnt <= 0) {
		// no track0 after 80 steps: drive missing or not responding
		ciab->ciaprb |= CIAF_DSKMOTOR;
		ciab->ciaprb |= CIAF_DSKSEL0 << num;
		return;
	}
	
	ciab->ciaprb &= ~CIAF_DSKDIREC;
	wait_lines(100);
	for (UBYTE i = 0; i < track; i++) {
		step_floppy();
	}

	ciab->ciaprb |= CIAF_DSKSEL0 << num;
}

static void set_audio(UBYTE *p, ULONG num)
{
	volatile UWORD *c = (volatile UWORD*)(0xdff0a0 + 16 * num);
	c[8 / 2] = p[1]; // AUDxVOL
	c[4 / 2] = getword(p, 1 + 1 + 1 + 1 + 2); // AUDxLEN
	c[6 / 2] = getword(p, 1 + 1 + 1 + 1 + 2 + 2 + 2); // AUDxPER
	c[0 / 2] = getword(p, 1 + 1 + 1 + 1 + 2 + 2 + 2 + 2); // AUDxLCH
	c[2 / 2] = getword(p, 1 + 1 + 1 + 1 + 2 + 2 + 2 + 2 + 2); // AUDxLCL
}

static void set_sprite(UBYTE *p, ULONG num)
{
	volatile UWORD *cpt = (volatile UWORD*)(0xdff120 + 4 * num);
	volatile UWORD *c = (volatile UWORD*)(0xdff140 + 8 * num);
	
	cpt[0 / 2] = getword(p, 0); // SPRxPTH
	cpt[2 / 2] = getword(p, 2); // SPRxPTL
	c[0 / 2] = getword(p, 2 + 2); // SPRxPOS
	c[2 / 2] = getword(p, 2 + 2 + 2); // SPRxCTL
}

static void set_custom(UBYTE *p)
{
	volatile UWORD *c = (volatile UWORD*)0xdff000;
	p += 4;
	for (WORD i = 0; i < 0x1fe; i += 2, c++) {

		// sprites
		if (i >= 0x120 && i < 0x180)
			continue;
			
		// audio
		if (i >= 0xa0 && i < 0xe0)
			continue;

		// skip blitter start, DMACON and INTENA
		if (i == 0x58 || i == 0x5e || i == 0x96 || i == 0x9a) {
			p += 2;
			continue;
		}

		// skip programmed sync registers except BEAMCON0
		if (i >= 0x1c0 && i != 0x1fc && i != 0x1dc) {
			p += 2;
			continue;
		}

		UWORD v = getword(p, 0);
		p += 2;

 		// BEAMCON0: PAL/NTSC only
		if (i == 0x1dc)
			v &= 0x20;
		// ADKCON
		if (i == 0x9e)
			v |= 0x8000;

		*c = v;
	}
}

void set_custom_final(UBYTE *p)
{
	volatile struct Custom *c = (volatile struct Custom*)0xdff000;
	c->intena = 0x7fff;
	c->intreq = 0x7fff;
	c->dmacon = 0x7fff;
	c->dmacon = getword(p, 4 + 0x96) | 0x8000;
	c->intena = getword(p, 4 + 0x9a) | 0x8000;
	c->intreq = getword(p, 4 + 0x9c) | 0x8000;
}

static void set_cia(UBYTE *p, ULONG num)
{
	volatile struct CIA *cia = (volatile struct CIA*)(num ? 0xbfd000 : 0xbfe001);
	volatile struct Custom *c = (volatile struct Custom*)0xdff000;

	cia->ciacra &= ~(CIACRAF_START | CIACRAF_RUNMODE);
	cia->ciacrb &= ~(CIACRBF_START | CIACRBF_RUNMODE);
	UBYTE dummy = cia->ciaicr;
	cia->ciaicr = 0x7f;
	c->intreq = 0x7fff;
	
	UBYTE flags = p[16 + 1 + 2 * 2 + 3 + 3];
	
	cia->ciapra = p[0];
	cia->ciaprb = p[1];
	cia->ciaddra = p[2];
	cia->ciaddrb = p[3];
	
	// load timers
	cia->ciatalo = p[4];
	cia->ciatahi = p[5];
	cia->ciatblo = p[6];
	cia->ciatbhi = p[7];
	cia->ciacra |= CIACRAF_LOAD;
	cia->ciacrb |= CIACRBF_LOAD;
	// load timer latches
	cia->ciatalo = p[16 + 1];
	cia->ciatahi = p[16 + 2];
	cia->ciatblo = p[16 + 3];
	cia->ciatbhi = p[16 + 4];
	
	// load alarm
	UBYTE *alarm = &p[16 + 1 + 2 * 2 + 3];
	cia->ciacrb |= CIACRBF_ALARM;
	if (flags & 2) {
		// leave latched
		cia->ciatodlow = alarm[0];	
		cia->ciatodmid = alarm[1];
		cia->ciatodhi = alarm[2];
	} else {
		cia->ciatodhi = alarm[2];
		cia->ciatodmid = alarm[1];
		cia->ciatodlow = alarm[0];
	}
	cia->ciacrb &= ~CIACRBF_ALARM;

	// load tod
	UBYTE *tod = &p[8];
	if (flags & 1) {
		// leave latched
		cia->ciatodlow = tod[0];
		cia->ciatodmid = tod[1];
		cia->ciatodhi = tod[2];
	} else {
		cia->ciatodhi = tod[2];
		cia->ciatodmid = tod[1];
		cia->ciatodlow = tod[0];
	}
}

void set_cia_final(UBYTE *p, ULONG num)
{
	volatile struct CIA *cia = (volatile struct CIA*)(num ? 0xbfd000 : 0xbfe001);
	UBYTE dummy = cia->ciaicr;
	cia->ciacra = p[14] & ~CIACRAF_LOAD;
	cia->ciacrb = p[15] & ~CIACRBF_LOAD;
	cia->ciaicr = p[16] | CIAICRF_SETCLR;	
}

static void free_allocations(struct uaestate *st)
{
	for (int i = st->num_allocations - 1; i >= 0; i--) {
		struct Allocation *a = &st->allocations[i];
		if (a->mh) {
			Deallocate(a->mh, a->addr, a->size);		
		} else {
			FreeMem(a->addr, a->size);
		}
	}
}

static UBYTE *extra_allocate(ULONG size, struct uaestate *st)
{
	UBYTE *b;
	
	for (;;) {
		b = AllocAbs(size, st->extra_mem_pointer);
		if (b) {
			struct Allocation *a = &st->allocations[st->num_allocations++];
			a->addr = b;
			a->size = size;
			st->extra_mem_pointer += (size + 7) & ~7;
			return b;
		}
		st->extra_mem_pointer += 8;
		if (st->extra_mem_pointer + size >= st->extra_ram + st->extra_ram_size)
			return NULL;
	}
}

// allocate from extra mem
static UBYTE *tempmem_allocate(ULONG size, struct uaestate *st)
{
	UBYTE *b = NULL;
	if (st->extra_mem_head) {
		b = Allocate(st->extra_mem_head, size);
		if (b) {
			struct Allocation *a = &st->allocations[st->num_allocations++];
			a->mh = st->extra_mem_head;
			a->addr = b;
			a->size = size;
		}
	}
	if (!b) {
		b = extra_allocate(size, st);
	}
	return b;
}

// allocate from statefile reserved bank index
static UBYTE *tempmem_allocate_reserved(ULONG size, WORD index, struct uaestate *st)
{
	struct MemoryBank *mb = &st->membanks[index];
	if (!mb->targetsize)
		return NULL;
	UBYTE *addr = mb->targetaddr;
	for (;;) {
		addr += 65536;
		if (addr - mb->targetaddr + size >= mb->targetsize)
			return NULL;
		UBYTE *b = AllocAbs(size, addr);
		if (b) {
			struct Allocation *a = &st->allocations[st->num_allocations++];
			a->addr = b;
			a->size = size;
			return b;
		}
	}
}

static void load_memory(FILE *f, WORD index, struct uaestate *st)
{
	struct MemoryBank *mb = &st->membanks[index];
	ULONG oldoffset = ftell(f);
	ULONG chunksize = mb->size + 12;
	fseek(f, mb->offset, SEEK_SET);
	printf("Memory '%s', size %luk, offset %lu. Target %08lx.\n", mb->chunk, chunksize >> 10, mb->offset, mb->targetaddr);
	// if Chip RAM and space in another statefile block? Put it there because chip ram is decompressed first.
	if (index == MB_CHIP) {
		mb->addr = tempmem_allocate_reserved(chunksize, MB_SLOW, st);
		if (!mb->addr)
			mb->addr = tempmem_allocate_reserved(chunksize, MB_FAST, st);
	} else if (index == MB_SLOW) {
		mb->addr = tempmem_allocate_reserved(chunksize, MB_FAST, st);
	}
	if (!mb->addr)
		mb->addr = tempmem_allocate(chunksize, st);
	if (mb->addr) {
		printf(" - Address %08lx - %08lx.\n", mb->addr, mb->addr + chunksize - 1);
		int v = fread(mb->addr, 1, chunksize, f);
		if (v != chunksize) {
			printf("ERROR: Read error (%lu != %lu).\n", v, chunksize);
			st->errors++;
		}		
	} else {
		printf("ERROR: Out of memory.\n");
		st->errors++;
	}
	fseek(f, oldoffset, SEEK_SET);
}

static int read_chunk_head(FILE *f, UBYTE *cnamep, ULONG *sizep, ULONG *flagsp)
{
	ULONG size = 0, flags = 0;
	UBYTE cname[5];

	*flagsp = 0;
	*sizep = 0;
	cnamep[0] = 0;
	if (fread(cname, 1, 4, f) != 4) {
		return 0;
	}
	cname[4] = 0;
	strcpy(cnamep, cname);

	if (fread(&size, 1, 4, f) != 4) {
		cnamep[0] = 0;
		return 0;
	}

	if (fread(&flags, 1, 4, f) == 0) {
		return 1;
	}

	if (size < 8)
		return 1;

	if (size < 12) {
		size = 0;
		flags = 0;
	} else {
		size -= 12;
	}
	*sizep = size;
	*flagsp = flags;
	return 1;
}

static UBYTE *load_chunk(FILE *f, UBYTE *cname, ULONG size, struct uaestate *st)
{
	UBYTE *b = NULL;
	int acate = 0;
	
	//printf("Allocating %lu bytes for '%s'.\n", size, cname);

	b = tempmem_allocate(size, st);
	
	//printf("Reading chunk '%s', %lu bytes to address %08x\.n", cname, size, b);
	
	if (!b) {
		printf("ERROR: Not enough memory (%ul bytes required).\n", size);
		return NULL;
	}
	
	if (fread(b, 1, size, f) != size) {
		printf("ERROR: Read error.\n");
		return NULL;
	}
	
	fseek(f, 4 - (size & 3), SEEK_CUR);
		
	return b;
}

static UBYTE *read_chunk(FILE *f, UBYTE *cname, ULONG *sizep, ULONG *flagsp, struct uaestate *st)
{
	ULONG size, orgsize, flags;

	if (!read_chunk_head(f, cname, &size, &flags))
		return NULL;
	orgsize = size;
	*flagsp = flags;

	if (size == 0)
		return NULL;

	ULONG maxsize = 0x7fffffff;

	for (int i = 0; unsupportedchunknames[i]; i++) {
		if (!strcmp(cname, unsupportedchunknames[i])) {
			printf("Unsupported chunk '%s', %lu bytes, flags %08x.\n", cname, size, flags);
			return NULL;
		}
	}

	int found = 0;
	for (int i = 0; chunknames[i]; i++) {
		if (!strcmp(cname, chunknames[i])) {
			found = 1;
			printf("Reading chunk '%s', %lu bytes, flags %08x.\n", cname, size, flags);
			break;
		}
	}
	if (!found) {
		// read only header if memory chunk
		for (int i = 0; memchunknames[i]; i++) {
			if (!strcmp(cname, memchunknames[i])) {
				found = 1;
				maxsize = 16;
				printf("Checking memory chunk '%s', %lu bytes, flags %08x.\n", cname, size, flags);
				break;
			}	
		}
	}
	
	if (!found) {
		//printf("Skipped chunk '%s', %ld bytes, flags %08x\n", cname, size, flags);
		fseek(f, size, SEEK_CUR);
		if (size)
			fseek(f, 4 - (size & 3), SEEK_CUR);
		return NULL;
	}

	*sizep = size;
	if (size > maxsize)
		size = maxsize;	
	UBYTE *chunk = malloc(size);
	if (!chunk) {
		printf("ERROR: Not enough memory.\n");
		return NULL;
	}
	if (fread(chunk, 1, size, f) != size) {
		printf("ERROR: Read error.\n");
		free(chunk);
		return NULL;
	}
	if (orgsize > size) {
		fseek(f, orgsize - size, SEEK_CUR);
	}
	fseek(f, 4 - (orgsize & 3), SEEK_CUR);
	return chunk;	
}

static void find_extra_ram(struct uaestate *st)
{
	Forbid();
	struct MemHeader *mh = (struct MemHeader*)SysBase->MemList.lh_Head;
	while (mh->mh_Node.ln_Succ) {
		ULONG mstart = ((ULONG)mh->mh_Lower) & 0xffff0000;
		ULONG msize = ((((ULONG)mh->mh_Upper) + 0xffff) & 0xffff0000) - mstart;
		int i;
		for (i = 0; i < MEMORY_REGIONS; i++) {
			if (st->mem_allocated[i] == mh)
				break;
		}
		if (i == MEMORY_REGIONS) {
			if (msize > st->extra_ram_size) {
				st->extra_ram = (UBYTE*)mstart;
				st->extra_ram_size = msize;
				st->extra_mem_head = mh;
			}	
		}
		mh = (struct MemHeader*)mh->mh_Node.ln_Succ;
	}
	Permit();
}

static ULONG check_ram(UBYTE *cname, UBYTE *chunk, WORD index, ULONG addr, ULONG offset, ULONG chunksize, ULONG flags, struct uaestate *st)
{
	ULONG size;
	if (flags & 1) // compressed
		size = getlong(chunk, 0);
	else
		size = chunksize;
	printf("Statefile RAM: Address %08x, size %luk.\n", addr, size >> 10);
	int found = 0;
	ULONG mstart, msize;
	Forbid();
	struct MemHeader *mh = (struct MemHeader*)SysBase->MemList.lh_Head;
	while (mh->mh_Node.ln_Succ) {
		mstart = ((ULONG)mh->mh_Lower) & 0xffff0000;
		msize = ((((ULONG)mh->mh_Upper) + 0xffff) & 0xffff0000) - mstart;
		if (mstart == addr) {
			if (msize >= size)
				found = 1;
			else
				found = -1;
			break;
		}
		mh = (struct MemHeader*)mh->mh_Node.ln_Succ;
	}
	Permit();
	if (!found) {
		printf("ERROR: Not found in this system.\n");
		st->errors++;
		return 0;
	}
	st->mem_allocated[index] = mh;
	struct MemoryBank *mb = &st->membanks[index];
	mb->size = chunksize;
	mb->offset = offset;
	mb->targetaddr = (UBYTE*)addr;
	mb->targetsize = msize;
	mb->flags = flags;
	strcpy(mb->chunk, cname);
	printf("- Detected memory at %08x, total size %luk.\n", mstart, msize >> 10);
	if (found > 0) {
		printf("- Is usable (%luk required, %luk unused, offset %lu).\n", size >> 10, (msize - size) >> 10, offset);
		ULONG extrasize = msize - size;
		if (extrasize >= 524288) {
			if ((mstart >= 0x00200000 && st->extra_ram < (UBYTE*)0x00200000) || extrasize > st->extra_ram_size) {
				st->extra_ram = (UBYTE*)(mstart + size);
				st->extra_ram_size = extrasize;
			}
		}
		return 1;
	}
	printf("ERROR: Not enough memory available (%luk required).\n", size >> 10);
	st->errors++;
	return 0;
}

static void floppy_info(int num, UBYTE *p)
{
	UBYTE state = p[4];
	UBYTE track = p[5];
	if (state & 2) // disabled
		return;
	printf("DF%d: Track %d, '%s'.\n", num, track, &p[4 + 1 + 1 + 1 + 1 + 4 + 4]);
}

static void check_rom(UBYTE *p, struct uaestate *st)
{
	UWORD ver = getword(p, 4 + 4 + 4);
	UWORD rev = getword(p, 4 + 4 + 4 + 2);
	
	UWORD *rom = (UWORD*)0xf80000;
	UWORD rver = rom[12 / 2];
	UWORD rrev = rom[14 / 2];
	
	ULONG start = getlong(p, 0);
	ULONG len = getlong(p, 4);
	if (start == 0xf80000 && len == 262144)
		start = 0xfc0000;
	ULONG crc32 = getlong(p, 4 + 4 + 4 + 4);
	
	UBYTE *path = &p[4 + 4 + 4 + 4 + 4];
	while (*path++);
	
	printf("ROM %08lx-%08lx %d.%d (CRC=%08x).\n", start, start + len - 1, ver, rev, crc32);
	if (ver != rver || rev != rrev) {
		printf("- '%s'\n", path);
		printf("WARNING: KS ROM version mismatch.\n");
	}
}

static int parse_pass_2(FILE *f, struct uaestate *st)
{
	for (int i = 0; i < MEMORY_REGIONS; i++) {
		struct MemoryBank *mb = &st->membanks[i];
		if (mb->size) {
			load_memory(f, i, st);
		}
	}
	
	for (;;) {
		ULONG size, flags;
		UBYTE cname [5];
		
		if (!read_chunk_head(f, cname, &size, &flags)) {
			return -1;
		}
		
		if (!strcmp(cname, "END "))
			break;

		if (!strcmp(cname, "CPU ")) {
			st->cpu_chunk = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "CHIP")) {
			st->custom_chunk = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "AGAC")) {
			st->aga_colors_chunk = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "CIAA")) {
			st->ciaa_chunk = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "CIAB")) {
			st->ciab_chunk = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "DSK0")) {
			st->floppy_chunk[0] = load_chunk(f, cname, size, st);
			floppy_info(0, st->floppy_chunk[0]);
		} else if (!strcmp(cname, "DSK1")) {
			st->floppy_chunk[1] = load_chunk(f, cname, size, st);
			floppy_info(1, st->floppy_chunk[1]);
		} else if (!strcmp(cname, "DSK2")) {
			st->floppy_chunk[2] = load_chunk(f, cname, size, st);
			floppy_info(2, st->floppy_chunk[2]);
		} else if (!strcmp(cname, "DSK3")) {
			st->floppy_chunk[3] = load_chunk(f, cname, size, st);
			floppy_info(3, st->floppy_chunk[3]);
		} else if (!strcmp(cname, "AUD0")) {
			st->audio_chunk[0] = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "AUD1")) {
			st->audio_chunk[1] = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "AUD2")) {
			st->audio_chunk[2] = load_chunk(f, cname, size, st);
		} else if (!strcmp(cname, "AUD3")) {
			st->audio_chunk[3] = load_chunk(f, cname, size, st);
		} else {
			fseek(f, size, SEEK_CUR);
			fseek(f, 4 - (size & 3), SEEK_CUR);
		}
	}

	return st->errors;
}

static int parse_pass_1(FILE *f, struct uaestate *st)
{
	int first = 1;
	UBYTE *b = NULL;

	for (;;) {
		ULONG offset = ftell(f);
		ULONG size, flags;
		UBYTE cname[5];
		b = read_chunk(f, cname, &size, &flags, st);
		if (!strcmp(cname, "END "))
			break;
		if (!b) {
			if (!cname[0])
				return -1;
			continue;
		}

		if (first) {
			if (strcmp(cname, "ASF ")) {
				printf("ERROR: Not UAE statefile.\n");
				return -1;
			}
			first = 0;
			continue;
		}

		if (!strcmp(cname, "CPU ")) {
			ULONG smodel = 68000;
			for (int i = 0; i < 4; i++) {
				if (SysBase->AttnFlags & (1 << i))
					smodel += 10;
			}
			if (SysBase->AttnFlags & 0x80)
				smodel = 68060;
			ULONG model = getlong(b, 0);
			if (smodel != model) {
				printf("- WARNING: %lu CPU statefile.\n", model);
			}
			if (model > 68030) {
				printf("- ERROR: Only 68000/68010/68020/68030 statefiles are supported.\n");
				st->errors++;
			}
		} else if (!strcmp(cname, "CHIP")) {
			UWORD vposr = getword(b, 4 + 4); // VPOSR
			volatile struct Custom *c = (volatile struct Custom*)0xdff000;
			UWORD svposr = c->vposr;
			int aga = (vposr & 0x0f00) == 0x0300;
			int ecs = (vposr & 0x2000) == 0x2000;
			int ntsc = (vposr & 0x1000) == 0x1000;
			int saga = (svposr & 0x0f00) == 0x0300;
			int secs = (svposr & 0x2000) == 0x2000;
			int sntsc = (svposr & 0x1000) == 0x1000;
			printf("Chipset: %s %s (0x%04X).\n", aga ? "AGA" : (ecs ? "ECS" : "OCS"), ntsc ? "NTSC" : "PAL", vposr);
			if (aga && !saga) {
				printf("- WARNING: AGA statefile.\n");
			}
			if (saga && !aga) {
				printf("- WARNING: OCS/ECS statefile.\n");
			}
			if (!sntsc && !ecs && ntsc) {
				printf("- WARNING: NTSC statefile.\n");
			}
			if (sntsc && !ecs && !ntsc) {
				printf("- WARNING: PAL statefile.\n");
			}
		} else if (!strcmp(cname, "CRAM")) {
			check_ram(cname, b, MB_CHIP, 0x000000, offset, size, flags, st);
		} else if (!strcmp(cname, "BRAM")) {
			check_ram(cname, b, MB_SLOW, 0xc00000, offset, size, flags, st);
		} else if (!strcmp(cname, "FRAM")) {
			check_ram(cname, b, MB_FAST, 0x200000, offset, size, flags, st);
		} else if (!strcmp(cname, "ROM ")) {
			check_rom(b, st);
		}

		free(b);
		b = NULL;
	}
	
	if (!st->errors) {
		find_extra_ram(st);
		if (!st->extra_ram) {
			printf("ERROR: At least 512k unused RAM required.\n");
			st->errors++;
		} else {
			printf("%luk extra RAM at %08x.\n", st->extra_ram_size >> 10, st->extra_ram);
			st->extra_mem_pointer = st->extra_ram;
			st->errors = 0;
		}
	} else {
		printf("ERROR: Incompatible hardware configuration.\n");
		st->errors++;
	}

	free(b);
	
	return st->errors;
}

extern void runit(void*);
extern void callinflate(UBYTE*, UBYTE*);

static void handlerambank(struct MemoryBank *mb, struct uaestate *st)
{
	UBYTE *sa = mb->addr + 16; /* skip chunk header + RAM size */
	if (mb->flags & 1) {
		// +2 = skip zlib header
		callinflate(mb->targetaddr, sa + 2);
	} else {
		ULONG *s = (ULONG*)sa;
		ULONG *d = (ULONG*)mb->targetaddr;
		for (int i = 0; i < mb->size / 4; i++) {
			*d++ = *s++;
		}		
	}
}

// Interrupts are off, supervisor state
static void processstate(struct uaestate *st)
{
	volatile struct Custom *c = (volatile struct Custom*)0xdff000;

	for (int i = 0; i < MEMORY_REGIONS; i++) {
		if (i == MB_CHIP)
			c->color[0] = 0x400;
		if (i == MB_SLOW)
			c->color[0] = 0x040;
		if (i == MB_FAST)
			c->color[0] = 0x004;
		struct MemoryBank *mb = &st->membanks[i];
		if (mb->addr) {
			handlerambank(mb, st);
		}
	}
	c->color[0] = 0x440;
	
	// must be before set_cia
	for (int i = 0; i < 4; i++) {
		set_floppy(st->floppy_chunk[i], i);
	}

	c->color[0] = 0x444;

	set_agacolor(st->aga_colors_chunk);
	set_custom(st->custom_chunk);
	for (int i = 0; i < 4; i++) {
		set_audio(st->audio_chunk[i], i);
	}
	for (int i = 0; i < 8; i++) {
		set_sprite(st->sprite_chunk[i], i);
	}
	set_cia(st->ciaa_chunk, 0);
	set_cia(st->ciab_chunk, 1);

	runit(st);
}

static void take_over(struct uaestate *st)
{
	// Copy stack, variables and code to safe location

	UBYTE *tempsp = tempmem_allocate(TEMP_STACK_SIZE, st);
	if (!tempsp) {
		printf("Out of memory for temp stack (%lu bytes).\n", TEMP_STACK_SIZE);
		return;
	}

	struct uaestate *tempst = (struct uaestate*)tempmem_allocate(sizeof(struct uaestate), st);
	if (!tempst) {
		printf("Out of memory for temp state variables (%lu bytes).\n", sizeof(struct uaestate));
		return;	
	}
	memcpy(tempst, st, sizeof(struct uaestate));

	struct Process *me = (struct Process*)FindTask(0);
	struct CommandLineInterface *cli = (struct CommandLineInterface*)((((ULONG)me->pr_CLI) << 2));
	if (!cli) {
		printf("CLI == NULL?\n");
		return;
	}
	ULONG *module = (ULONG*)(cli->cli_Module << 2);
	ULONG hunksize = module[-1] << 2;
	UBYTE *newcode = tempmem_allocate(hunksize, st);
	if (!newcode) {
		printf("Out of memory for temp code (%lu bytes).\n", hunksize);
		return;
	}
	memcpy(newcode, module, hunksize);
	
	// ugly relocation hack but jumps to other module (asm.S) are always absolute..
	// TODO: process the executable after linking
	UWORD *cp = (UWORD*)newcode;
	for (int i = 0; i < hunksize / 2; i++) {
		// JSR/JMP xxxxxxxx.L?
		if (*cp == 0x4eb9 || *cp == 0x4ef9) {
			ULONG *ap = (ULONG*)(cp + 1);
			ULONG *app = (ULONG*)(*ap);
			void *addr = (void*)app;
			if (addr == runit || addr == callinflate) {
				*ap = (ULONG)addr - (ULONG)module + (ULONG)newcode;
				//printf("Relocated %08x: %08x -> %08x\n", cp, addr, *ap);
			}
		}
		cp++;
	}
	
	printf("Code=%08lx Stack=%08lx Data=%08lx. Press RETURN!\n", newcode, tempsp, tempst);
	Delay(100); // So that key release gets processed by AmigaOS
	
#if 0
	if (SysBase->LibNode.lib_Version >= 37) {
		CacheClearU();
	}
#endif

	UBYTE b;
	fread(&b, 1, 1, stdin);
	
	if (GfxBase->LibNode.lib_Version >= 37) {
		LoadView(NULL);
		WaitTOF();
		WaitTOF();	
	}
	
	// No turning back!
	extern void *killsystem(UBYTE*, struct uaestate*, ULONG);
	killsystem(tempsp + TEMP_STACK_SIZE, tempst, (ULONG)processstate - (ULONG)module + (ULONG)newcode);
}

int main(int argc, char *argv[])
{
	FILE *f;
	UBYTE *b;
	ULONG size;
	UBYTE cname[5];
	struct uaestate *st;
	
	printf("uaestateload v" VER " (" REVTIME " " REVDATE ")\n");
	if (argc < 2) {
		printf("Syntax: uaestateload <statefile.uss>.\n");
		return 0;
	}
	
	f = fopen(argv[1], "rb");
	if (!f) {
		printf("Couldn't open '%s'\n", argv[1]);
		return 0;
	}

	st = calloc(sizeof(struct uaestate), 1);
	if (!st) {
		printf("Out of memory.\n");
		return 0;
	}
	
	if (!parse_pass_1(f, st)) {
		fseek(f, 0, SEEK_SET);
		if (!parse_pass_2(f, st)) {
			take_over(st);			
		} else {
			printf("Pass #2 failed (%ld errors).\n", st->errors);
		}
	} else {
		printf("Pass #1 failed (%ld errors).\n", st->errors);
	}
	
	free(st);
	
	fclose(f);

	free_allocations(st);

	return 0;
}
