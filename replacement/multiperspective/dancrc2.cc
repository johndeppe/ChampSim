/*
 * dancrc2.cc
 *
 * Multiperspective Reuse Predictor
 * 
 */

#include "champsim.h"
#include "cache.h"

#include <string.h>
#include <stdlib.h>

using namespace std;

// set from get_config_number()

unsigned int
	llc_sets, 
	num_core;

#define MAX_PATH_LENGTH 16
#define LLC_WAYS	16

// for printing space calculations

bool
	**plru_bits, 	// per-set pseudo-LRU bits
	*lastmiss_bits;	// for lastmiss feature

unsigned char
	**rrpv;		// for RRIP policy

// one sampler entry

struct sdbp_sampler_entry {
	unsigned int 	
		lru_stack_position,
		tag,

		// copy of the trace used for the most recent prediction

		trace_buffer[MAX_PATH_LENGTH+1];
		
	// confidence from most recent prediction

	int conf;

	// constructor

	sdbp_sampler_entry (void) {
		lru_stack_position = 0;
		tag = 0;
	};
};

// one sampler set (just a pointer to the entries)

struct sdbp_sampler_set {
	sdbp_sampler_entry *blocks;

	sdbp_sampler_set (void);
};

// the dead block predictor

struct perceptron_predictor {
	int **tables; 	// tables of two-bit counters
	int *table_sizes; // size of each table (not counted against h/w budget)

	perceptron_predictor (void);
	int get_prediction (uint32_t tid, int set);
	void block_is_dead (uint32_t tid, sdbp_sampler_entry *, unsigned int *, bool, int, int);
};

// the sampler

struct sdbp_sampler {
	sdbp_sampler_set *sets;
	int 
		nsampler_sets;   // number of sampler sets

	perceptron_predictor *pred;
	sdbp_sampler (int nsets, int assoc);
	void access (uint32_t tid, int set, int real_set, uint64_t tag, uint64_t PC, int, uint64_t);
};

struct sdbp_sampler; // forward declaration of sampler type
sdbp_sampler *samp; // pointer to the sampler

int Get_Sampler_Victim ( uint32_t tid, uint32_t setIndex, const BLOCK *current_set, uint32_t assoc, uint64_t PC, uint64_t paddr, uint32_t accessType);

// find replacement victim
// return value should be 0 ~ 15 or 16 (bypass)

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t PC, uint64_t paddr, uint32_t type) {
	return Get_Sampler_Victim (cpu, set, current_set, LLC_WAYS, PC, paddr, type);
}


// use this function to print out your own stats on every heartbeat 
void PrintStats_Heartbeat() { }

// use this function to print out your own stats at the end of simulation
void CACHE::replacement_final_stats() {}

static int 
	lognsets6, // log_2 number of sets plus 6
	lognsets;  // log_2 number of sets

// trace is built here before prediction

static unsigned int
	trace_buffer[MAX_PATH_LENGTH+1];

// per-core array of recent PCs

static unsigned int
	**addresses;

// placement vector (initialized elsewhere)

int plv[3][2] = {
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
};

// for set-dueling the best bypass threshold

static int 
	psel = 0;

// boundaries of leader sets

static unsigned int 
	leaders1 = 32, 
	leaders2 = 64;

// parameters

static int
	dan_promotion_threshold = 256,
	dan_init_weight = 1,
	dan_dt1 = 55, dan_dt2 = 1024,
	dan_rrip_place_position = 0,
	dan_leaders = 34,
	dan_ignore_prefetch = 1, // don't predict hitting prefetches
	dan_use_plru = 0, // 0 means use MDPP, 1 means use PLRU
	dan_use_rrip = 0, // 1 means use RRIP instead of MDPP/PLRU
	dan_bypass_threshold = 1000,
	dan_record_types = 27, // mask for deciding which kinds of memory accesses to record in history
	// sampler associativity 
	dan_sampler_assoc = 18,
	// number of bits used to index predictor; determines number of
	// entries in prediction tables
	dan_predictor_index_bits = 8,
	// number of prediction tables
	dan_predictor_tables = 16, // default specs have 16 features
	// width of prediction saturating counters
	dan_counter_width = 6,
	// predictor must meet this threshold to predict a block is dead
	dan_threshold = 8,
	dan_theta2 = 210,
	dan_theta = 110,
	// number of partial tag bits kept per sampler entry
	dan_sampler_tag_bits = 16,
	dan_samplers = 80,
	// number of entries in prediction table; derived from # of index bits
	dan_predictor_table_entries,
	// maximum value of saturating counter; derived from counter width
	dan_counter_min,
	dan_counter_max;

// called on every cache hit and cache fill

void UpdateSampler (uint32_t setIndex, uint64_t tag, uint32_t tid, uint64_t PC, int32_t way, bool hit, uint32_t accessType, uint64_t paddr);


void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t paddr, uint64_t PC, uint64_t victim_addr, uint32_t type, uint8_t hit) {
	if (hit && (type == WRITEBACK)) return;
	uint64_t tag = paddr / (llc_sets * 64);
	UpdateSampler (set, tag, cpu, PC, way, hit, type, paddr);
}

// feature types

#define F_PC    0       // PC xor which PC (unless which = 0, then it's just PC)
#define F_TAG   1       // address
#define F_BIAS  2       // 0
#define F_BURST 3
#define F_INS	4	// this is an insertion
#define F_LM	5
#define F_OFF	6

// a feature specifier

struct feature_spec {
	int     type;   // type of feature
	int     assoc;  // beyond which a block is considered evicted (dead)
	int     begin;  // beginning bit
	int     end;    // ending bit
	int     which;  // which (for PC)
	int     xorpc;  // xorpc & 1 => XOR with PC, xorpc & 2 => XOR with PREFETCH
};

// maximum number of specifiers to read

#define MAX_SPECS       (MAX_PATH_LENGTH+1)

// features for original MICRO 2016 paper

#if 0
static feature_spec perceptron_specs[] = {
{ F_PC, 15, 0, 8, 0, 0 },
{ F_PC, 15, 1, 9, 1, 0 },
{ F_PC, 15, 2, 10, 2, 0 },
{ F_PC, 15, 3, 11, 3, 0 },
{ F_TAG, 15, 25, 33, 0, 0 },
{ F_TAG, 15, 28, 36, 0, 0 },
};
#endif

// features for the various configurations

static feature_spec default_single_1_specs[] = {
{ F_OFF, 15, 1, 6, 0, 1 },
{ F_PC, 7, 14, 43, 11, 0 },
{ F_PC, 16, 3, 11, 16, 1 },
{ F_INS, 16, 0, 0, 0, 1 },
{ F_OFF, 10, 0, 6, 0, 1 },
{ F_PC, 10, 1, 53, 10, 0 },
{ F_BIAS, 16, 0, 0, 0, 0 },
{ F_INS, 8, 0, 0, 0, 1 },
{ F_PC, 17, 6, 20, 0, 1 },
{ F_BURST, 6, 11, 22, 9, 0 },
{ F_LM, 9, 0, 0, 0, 0 },
{ F_PC, 17, 6, 20, 0, 1 },
{ F_INS, 16, 2, 47, 2, 0 },
{ F_INS, 17, 0, 0, 0, 1 },
{ F_PC, 16, 8, 16, 5, 0 },
{ F_PC, 17, 6, 20, 14, 1 },
};

static feature_spec default_single_2_specs[] = {
{ F_INS, 15, 7, 55, 1, 0 },
{ F_INS, 16, 0, 0, 0, 1 },
{ F_INS, 6, 13, 38, 0, 1 },
{ F_OFF, 14, 0, 7, 0, 2 },
{ F_BIAS, 17, 8, 23, 10, 3 },
{ F_BURST, 8, 1, 11, 13, 2 },
{ F_PC, 6, 5, 48, 0, 3 },
{ F_LM, 15, 16, 44, 0, 2 },
{ F_TAG, 17, 1, 32, 14, 2 },
{ F_PC, 17, 6, 20, 0, 1 },
{ F_PC, 6, 4, 11, 2, 2 },
{ F_BIAS, 13, 8, 67, 7, 2 },
{ F_OFF, 8, 1, 6, 0, 2 },
{ F_PC, 6, 5, 77, 4, 1 },
{ F_TAG, 11, 8, 19, 7, 0 },
{ F_TAG, 16, 8, 16, 0, 0 },
// # 3.511360
};

static feature_spec default_multi_3_specs[] = {
{ F_BIAS, 1, 0, 0, 0, 0 },
{ F_PC, 16, 9, 25, 9, 1 },
{ F_INS, 8, 4, 8, 7, 2 },
{ F_PC, 6, 9, 28, 12, 1 },
{ F_OFF, 14, 1, 4, 0, 3 },
{ F_LM, 7, 7, 51, 3, 1 },
{ F_PC, 10, 1, 54, 13, 3 },
{ F_PC, 10, 3, 32, 5, 1 },
{ F_PC, 14, 5, 24, 0, 1 },
{ F_OFF, 13, 4, 4, 0, 2 },
{ F_TAG, 8, 4, 47, 11, 2 },
{ F_TAG, 2, 24, 32, 0, 1 },
{ F_PC, 12, 10, 30, 0, 1 },
{ F_PC, 12, 9, 28, 0, 2 },
{ F_PC, 12, 5, 31, 2, 2 },
{ F_TAG, 8, 10, 8, 7, 1 },
// # 23.392479
};

static feature_spec default_multi_4_specs[] = {
{ F_LM, 9, 5, 17, 4, 0 },
{ F_PC, 8, 6, 8, 14, 3 },
{ F_BIAS, 13, 9, 40, 10, 3 },
{ F_OFF, 8, 2, 2, 0, 2 },
{ F_TAG, 16, 3, 14, 11, 2 },
{ F_BURST, 16, 14, 28, 9, 2 },
{ F_INS, 10, 4, 14, 3, 2 },
{ F_PC, 14, 3, 18, 10, 2 },
{ F_INS, 6, 11, 18, 9, 0 },
{ F_PC, 17, 1, 14, 5, 0 },
{ F_OFF, 11, 2, 5, 0, 0 },
{ F_OFF, 15, 0, 7, 0, 3 },
{ F_TAG, 9, 2, 13, 7, 2 },
{ F_TAG, 15, 4, 34, 3, 2 },
{ F_OFF, 10, 0, 6, 0, 1 },
{ F_PC, 11, 7, 23, 0, 2 },
// # 9.734541
};

// which features to use

static feature_spec *specs = NULL;

// for computig space overhead

static int total_bits = 0;

// the configuration number

static int config = 1;

// set the parameters based on configuration number

void set_parameters (void) {
	switch (config) {
	case 1: case 5:
		dan_promotion_threshold = 82;
		dan_init_weight = 1;
		dan_dt1 = 56;
		dan_dt2 = 256;
		dan_leaders = 34;
		dan_ignore_prefetch = 1;
		dan_use_plru = 0;
		dan_use_rrip = 0;
		dan_bypass_threshold = 48;
		dan_record_types = 27;
		dan_sampler_assoc = 18;
		dan_predictor_index_bits = 8;
		dan_predictor_tables = 16;
		dan_counter_width = 6;
		dan_threshold = 128;
		dan_theta = 109;
		dan_theta2 = 135;
		dan_sampler_tag_bits = 16;
		dan_samplers = 80;
		specs = default_single_1_specs;
		plv[0][0] = -15;
		plv[0][1] = 0;
		plv[1][0] = 35;
		plv[1][1] = 12;
		plv[2][0] = 44;
		plv[2][1] = 15;
		break;
	case 2: case 6:
		dan_promotion_threshold = 178;
		dan_init_weight = 1;
		dan_dt1 = 42;
		dan_dt2 = 48;
		dan_leaders = 34;
		dan_ignore_prefetch = 1;
		dan_use_plru = 0;
		dan_use_rrip = 0;
		dan_bypass_threshold = 48;
		dan_record_types = 27;
		dan_sampler_assoc = 18;
		dan_predictor_index_bits = 8;
		dan_predictor_tables = 16;
		dan_counter_width = 6;
		dan_threshold = 128;
		dan_theta = 109;
		dan_theta2 = 135;
		dan_sampler_tag_bits = 16;
		dan_samplers = 80;
		specs = default_single_2_specs;
		plv[0][0] = -15;
		plv[0][1] = 0;
		plv[1][0] = 35;
		plv[1][1] = 12;
		plv[2][0] = 44;
		plv[2][1] = 15;
		break;
	case 3:
		dan_promotion_threshold = 256;
		dan_rrip_place_position = 2,
		dan_init_weight = 1;
		dan_dt1 = 27;
		dan_dt2 = 229;
		dan_leaders = 34;
		dan_ignore_prefetch = 1;
		dan_use_plru = 0;
		dan_use_rrip = 1;
		dan_bypass_threshold = -3;
		dan_record_types = 27;
		dan_sampler_assoc = 18;
		dan_predictor_index_bits = 8;
		dan_predictor_tables = 16;
		dan_counter_width = 6;
		dan_threshold = 0;
		dan_theta = 0;
		dan_theta2 = 0;
		dan_sampler_tag_bits = 16;
		dan_samplers = 308;
		specs = default_multi_3_specs;
		plv[0][0] = -230;
		plv[0][1] = 1;
		plv[1][0] = 12;
		plv[1][1] = 2;
		plv[2][0] = 22;
		plv[2][1] = 3;
		break;
	case 4:
		dan_promotion_threshold = 256;
		dan_rrip_place_position = 2,
		dan_init_weight = 1;
		dan_dt1 = 100;
		dan_dt2 = 154;
		dan_leaders = 34;
		dan_ignore_prefetch = 1;
		dan_use_plru = 0;
		dan_use_rrip = 1;
		dan_bypass_threshold = -3;
		dan_record_types = 27;
		dan_sampler_assoc = 18;
		dan_predictor_index_bits = 8;
		dan_predictor_tables = 16;
		dan_counter_width = 6;
		dan_threshold = 0;
		dan_theta = 0;
		dan_theta2 = 0;
		dan_sampler_tag_bits = 16;
		dan_samplers = 337;
		specs = default_multi_4_specs;
		plv[0][0] = -111;
		plv[0][1] = 0;
		plv[1][0] = -110;
		plv[1][1] = 2;
		plv[2][0] = 20;
		plv[2][1] = 3;
		break;
	default: assert (0);
	}
}

// initialize replacement state

void CACHE::initialize_replacement() {
	config = 2;
	switch (config) {
		case 1: case 2:
			num_core = 1;
			llc_sets = 2048;
			break;
		case 3: case 4:
			num_core = 4;
			llc_sets = 8192;
			break;
		case 5: case 6:
			num_core = 1;
			llc_sets = 8192;
			break;
		default: assert (0);
	}
	printf ("config %d, num_core %d, llc_sets %d\n", config, num_core, llc_sets);

	// default parameters

	set_parameters ();

	// this variable helps put physical addresses back together

	if (llc_sets == 2048) {
		lognsets = 11;
	} else if (llc_sets == 8192) {
		lognsets = 13;
	} else assert (0);
	lognsets6 = lognsets + 6;

	// initialize replacement state

	if (num_core == 4) {

		// multi-core configurations use RRIP

		rrpv = new unsigned char*[llc_sets];
		int rrpv_bits = 0;
		for (unsigned int i=0; i<llc_sets; i++) {
			rrpv[i] = new unsigned char[LLC_WAYS];
			memset (rrpv[i], 3, LLC_WAYS);
			rrpv_bits += 2 * LLC_WAYS;
		}
		total_bits += rrpv_bits;
	} else if (num_core == 1) {

		// single-core configurations use MDPP/PLRU

		int bits = 0;
		plru_bits = new bool*[llc_sets];
		for (unsigned int i=0; i<llc_sets; i++) {
			plru_bits[i] = new bool[LLC_WAYS-1];
			memset (plru_bits[0], 0, LLC_WAYS-1);
			bits += LLC_WAYS-1;
		}
		total_bits += bits;
	} else assert (0);

	// per-core arrays of recent PCs (we count the bits later when we know the max PC width)

	addresses = new unsigned int*[num_core];
	for (unsigned int i=0; i<num_core; i++) {
		addresses[i] = new unsigned int[MAX_PATH_LENGTH];
		memset (addresses[i], 0, sizeof (int) * MAX_PATH_LENGTH);
	}

	// initialize lastmiss feature

	lastmiss_bits = new bool[llc_sets];
	memset (lastmiss_bits, 0, llc_sets);
	total_bits += llc_sets;

	// initialize sampler

	samp = new sdbp_sampler (llc_sets, LLC_WAYS);

	// at this point, the specs are set; we can compute the sizes

	int trace_bits = 0;
	for (int i=0; i<dan_predictor_tables; i++) {
		switch (specs[i].type) {
		case F_BIAS:
		case F_BURST:
		case F_LM:
		case F_INS:
			if (specs[i].xorpc == 0 || specs[i].xorpc == 2) trace_bits += 1; else trace_bits += dan_predictor_index_bits;
			break;
		case F_OFF:
			if (specs[i].xorpc == 0 || specs[i].xorpc == 2) trace_bits += (specs[i].end-specs[i].begin); 
			else trace_bits += dan_predictor_index_bits;
			break;
		default:
			trace_bits += dan_predictor_index_bits;
			break;
		}
	}
	int sampler_set_bits = dan_sampler_assoc * (
		4 // lru stack position
	+	dan_sampler_tag_bits
	+	trace_bits		// bits for the trace stored in each sampler entry
	+ 	9 // confidence bits
	);

	total_bits += trace_bits;	// global trace buffer


	// figure out how many bits we need to store the global array of PCs

	int max_end = 0;
	for (int i=0; i<dan_predictor_tables; i++) {
		if (specs[i].type == F_PC) if (specs[i].end > max_end) max_end = specs[i].end;
	}
	total_bits += max_end * MAX_PATH_LENGTH * num_core;
	//int total_bits_allowed = 8 * 32768 * num_core;
	//int sampler_bits = total_bits_allowed - total_bits;
	total_bits += 10; // for psel
	total_bits += 512; // in case someone wants to be pedantic about something
	total_bits += dan_samplers * sampler_set_bits;
}

// tree-based PseudoLRU operations

#define PLRU_LEFT(i)    ((i)*2+2)
#define PLRU_RIGHT(i)   ((i)*2+1)
#define SETBIT(z,k) ((z)|=(1<<(k)))
#define GETBIT(z,k) (!!((z)&(1<<(k))))

// most recent access was a cache burst

static bool was_burst;

// update the PseudoLRU replacement state, using the placement vector and
// predictor confidence on a placement, and promotion threshold on a hit

static void update_plru_mdpp (int set, int32_t wayID, bool hit, int *vector, uint32_t accessType, bool really = true, int conf = 0) {
	assert (!dan_use_rrip);
	assert (wayID < 16);
	bool P[LLC_WAYS-1];
	memcpy (P, plru_bits[set], LLC_WAYS-1);
	unsigned int idx = 0;
	unsigned int x;
	bool wasodd = false;
	unsigned int newidx = 0;
	assert (vector);
	if (!hit) {
		if (conf >= plv[2][0]) newidx = plv[2][1];
		else if (conf >= plv[1][0]) newidx = plv[1][1];
		else if (conf >= plv[0][0]) newidx = plv[0][1];
	} else {
		// get the current plru index
		// build the index starting from a leaf and going to the root
		x = wayID + LLC_WAYS - 1;
		int i = 0;
		while (x) {
			wasodd = x & 1;
			x = (x - 1) >> 1;
			// FIXME: here we say 3 but it should be log2(assoc)-1
			if (P[x] == wasodd) SETBIT(idx,3-i);
			i++;
		}
		assert (i == 4);
		newidx = vector[idx];
	}

	// set the new plru index for this block

	wasodd = false;
	x = wayID + LLC_WAYS - 1;
	int i = 0;
	bool changed = false;
	while (x) {
		wasodd = x & 1;
		x = (x - 1) >> 1;
		// FIXME: here we say 3 but it should be log2(assoc)-1
		bool oldbit = P[x];
		P[x] = wasodd == GETBIT(newidx,3-i);
		if (P[x] != oldbit) changed = true;
		i++;
	}
	was_burst = !changed;
	assert (i == 4);
	if (conf > dan_promotion_threshold) really = false;

	// we might not "really" want to update state if we are just
	// calling this function to see if we had a cache burst

	if (really) memcpy (plru_bits[set], P, LLC_WAYS-1);
}

// get the PseudoLRU victim

static int get_mdpp_plru_victim (int set) {
	// find the pseudo-lru block
	bool *P = plru_bits[set];

	int a = LLC_WAYS - 1;
	unsigned int x = 0;
	int level = 0;
	while (a) {
		level++;
		if (P[x])
			x = PLRU_RIGHT(x);
		else
			x = PLRU_LEFT(x);
		a /= 2;
	}
	x -= (LLC_WAYS-1);
	return x;
}

// make a trace from a PC (just extract some bits)

static void make_trace (uint32_t tid, perceptron_predictor *pred, uint32_t setIndex, uint64_t PC, uint32_t tag, int accessType, bool burst, bool insertion, bool lastmiss, unsigned int offset) {
	for (int i=0; i<dan_predictor_tables; i++) {
		feature_spec *f = &specs[i];
		int begin_shift = f->begin;
		int end_mask = (1<<(f->end - begin_shift))-1; // TODO: make this a hash instead of a simple bit extract
		switch (f->type) {
		case F_PC:
			if (f->which == 0)
				trace_buffer[i] = (PC >> begin_shift) & end_mask;
			else
				trace_buffer[i] = (addresses[tid][f->which-1] >> begin_shift) & end_mask;
			break;
		case F_TAG:
			trace_buffer[i] = (((tag << lognsets6) | (setIndex << 6)) >> begin_shift) & end_mask;
			break;
		case F_BIAS:
			trace_buffer[i] = 0;
			break;
		case F_BURST:
			trace_buffer[i] = burst;
			break;
		case F_INS:
			trace_buffer[i] = insertion;
			break;
		case F_LM:
			trace_buffer[i] = lastmiss;
			break;
		case F_OFF:
			trace_buffer[i] = (offset >> begin_shift) & end_mask;
			break;
		}
		if (f->xorpc & 1) trace_buffer[i] ^= PC;
		if (f->xorpc & 2) trace_buffer[i] ^= 2 * (accessType == PREFETCH);
	}
}

// multiply bit vector x by matrix m (for shuffling set indices to determine
// sampler sets)

unsigned int mm (unsigned int x, unsigned int m[]) {
        unsigned int r = 0;
        for (int i=0; i<lognsets; i++) {
                r <<= 1;
                unsigned int d = x & m[i];
                r |= __builtin_parity (d);
        }
        return r;
}

// update replacement policy

void UpdateSampler (uint32_t setIndex, uint64_t tag, uint32_t tid, uint64_t PC, int32_t way, bool hit, uint32_t accessType, uint64_t paddr) {

	// don't need to update on a bypass

	if (way >= 16) return;

	// make distinct PCs for hitting/missing prefetches

	if (accessType == PREFETCH) PC ^= (0xdeadbeef + hit);

	// make distinct PCs for hitting/missing writebacks, and skip a
	// bunch of stuff

	if (accessType == WRITEBACK) {
		PC ^= 0x7e57ab1e;
		goto stuff;
	}

	// ignore hitting prefetches

	if (dan_ignore_prefetch == 1) {
		if ((accessType == PREFETCH) && hit) goto stuff;
	}

	// update up/down counter for set-dueling

	if (setIndex < leaders1) {
		if (!hit) if (psel < 1023) psel++;
	} else if (setIndex < leaders2) {
		if (!hit) if (psel > -1023) psel--;
	}

	// another place where we can ignore hitting prefetches

	if (dan_ignore_prefetch == 2) {
		if ((accessType == PREFETCH) && hit) goto stuff;
	}
	// determine if this is a sampler set
	{
		// multiply set index by specially crafted invertible
		// matrix to distribute sampled sets across cache

		static unsigned int le11[] = { 0x37f, 0x431, 0x71d, 0x25c, 0x719, 0x4d5, 0x4b6, 0x2ca, 0x26d, 0x64f, 0x46d };
		static unsigned int le13[] = { 0x5c5, 0xcc5, 0xb6b, 0x1bc5, 0x8b, 0x1782, 0x190, 0x15dd, 0x1af8, 0x75e, 0x4a1, 0xb4b, 0x1196 };
		unsigned int *le = (num_core == 1) ? le11 : le13;
		int set = mm (setIndex, le);

		// if this is a sampler set, access it

		if (set >= 0 && set < samp->nsampler_sets)
			samp->access (tid, set, setIndex, tag, PC, accessType, paddr);

		// update default replacement policy (MDPP or PLRU)

		int *vector;
		static int vecmdpp[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 0, 1, 0, 0 };
		static int vecplru[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		vector = dan_use_plru ? vecplru : vecmdpp;

		// do a fake promotion to see if this was a burst so we can get a prediction.

		if (dan_use_rrip) {
			was_burst = rrpv[setIndex][way] == 0;
		} else {
			update_plru_mdpp (setIndex, way, hit, vector, accessType, false);
		}

		// make the trace

		make_trace (tid, samp->pred, setIndex, PC, tag, accessType, was_burst, !hit, lastmiss_bits[setIndex], paddr & 63);

		// get the next prediction for this block using that trace

		int conf = samp->pred->get_prediction (tid, setIndex);
		if (dan_use_rrip) {

			// what is the current RRPV value for this block?

			int position = rrpv[setIndex][way];
			if (!hit) {

				// on a placement, use the placement vector

				position = dan_rrip_place_position;
				if (conf >= plv[2][0]) position = plv[2][1];
				else if (conf >= plv[1][0]) position = plv[1][1];
				else if (conf >= plv[0][0]) position = plv[0][1];
			} else {

				// on a hit, use the promotion threshold

				if (conf < dan_promotion_threshold)
					position = 0;
			}

			// assign the new RRPV

			rrpv[setIndex][way] = position;
		} else {

			// MDPP/PLRU replacement

			update_plru_mdpp (setIndex, way, hit, vector, accessType, true, conf);
		}
	}
stuff:
	// update address path

	bool record = false;
	if (accessType == LOAD) if (dan_record_types & 1) record = true;
	if (accessType == RFO) if (dan_record_types & 2) record = true;
	if (accessType == WRITEBACK) if (dan_record_types & 8) record = true;
	if (accessType == PREFETCH) if (dan_record_types & 16) record = true;
	if (record) {
		memmove (&addresses[tid][1], &addresses[tid][0], (MAX_PATH_LENGTH-1) * sizeof (unsigned int));
		addresses[tid][0] = PC;
	}
	lastmiss_bits[setIndex] = !hit;
}

int Get_Sampler_Victim ( uint32_t tid, uint32_t setIndex, const BLOCK *current_set, uint32_t assoc, uint64_t PC, uint64_t paddr, uint32_t accessType) {

	// select a victim using default pseudo LRU policy

	assert (setIndex < llc_sets);
	int r;
	if (dan_use_rrip) {
startover:
		int lrus[LLC_WAYS], n = 0;
		for (unsigned int wayID=0; wayID<assoc; wayID++) {
			if (rrpv[setIndex][wayID] == 3) lrus[n++] = wayID;
		}
		if (n) {
			r = lrus[rand()%n];
		} else {
			for (unsigned int wayID=0; wayID<assoc; wayID++) rrpv[setIndex][wayID]++;
			goto startover;
		}
	} else {
		r = get_mdpp_plru_victim (setIndex);
	}
	// we now have a victim, r; predict whether this block is
	// "dead on arrival" and bypass for non-writeback accesses

	if (accessType != WRITEBACK) {
		uint32_t tag = paddr / (llc_sets * 64);
		make_trace (tid, samp->pred, setIndex, PC, tag, accessType, false, true, lastmiss_bits[setIndex], paddr & 63);
		int prediction;
		int conf = samp->pred->get_prediction (tid, setIndex);
		if (dan_dt2) {
			if (setIndex < leaders1) 
				prediction = conf >= dan_dt1;
			else if (setIndex < leaders2)
				prediction = conf >= dan_dt2;
			else {
				if (psel >= 0)
					prediction = conf >= dan_dt2;
				else
					prediction = conf >= dan_dt1;
			}
		} else {
			prediction = conf >= dan_bypass_threshold;
		}
	
		// if block is predicted dead, then it should bypass the cache
	
		if (prediction) {
			r = LLC_WAYS; // means bypass
		}
	}

	// return the selected victim

	return r;
}

// constructor for a sampler set

sdbp_sampler_set::sdbp_sampler_set (void) {

	// allocate some sampler entries

	blocks = new sdbp_sampler_entry[dan_sampler_assoc];

	// initialize the LRU replacement algorithm for these entries

	for (int i=0; i<dan_sampler_assoc; i++)
		blocks[i].lru_stack_position = i;
}

// access the sampler with an LLC tag

void sdbp_sampler::access (uint32_t tid, int set, int real_set, uint64_t tag, uint64_t PC, int accessType, uint64_t paddr) {

	// get a pointer to this set's sampler entries

	sdbp_sampler_entry *blocks = &sets[set].blocks[0];

	// get a partial tag to search for

	unsigned int partial_tag = tag & ((1<<dan_sampler_tag_bits)-1);

	// this will be the way of the sampler entry we end up hitting or replacing

	int i;

	// search for a matching tag

	// no valid bits; tags are initialized to 0, and if we accidentally
	// match a 0 that's OK because we don't need correctness

	for (i=0; i<dan_sampler_assoc; i++) if (blocks[i].tag == partial_tag) {

		// we know this block is not dead; inform the predictor

		pred->block_is_dead (tid, &blocks[i], blocks[i].trace_buffer, false, blocks[i].conf, blocks[i].lru_stack_position);
		break;
	}

	// did we find a match?

	bool is_fill = false;

	if (i == dan_sampler_assoc) {

		// find the LRU block

		if (i == dan_sampler_assoc) {
			int j;
			for (j=0; j<dan_sampler_assoc; j++)
				if (blocks[j].lru_stack_position == (unsigned int) (dan_sampler_assoc-1)) break;
			assert (j < dan_sampler_assoc);
			i = j;
		}

		// previous trace leads to block being dead; inform the predictor

		pred->block_is_dead (tid, &blocks[i], blocks[i].trace_buffer, true, blocks[i].conf, dan_sampler_assoc);

		// reminds us to fill the block later (after we're done
		// using the current victim's metadata)

		is_fill = true;
	}

	// now the replaced or hit entry should be moved to the MRU position

	unsigned int position = blocks[i].lru_stack_position;
	for(int way=0; way<dan_sampler_assoc; way++) {
		if (blocks[way].lru_stack_position < position) {
			blocks[way].lru_stack_position++;
			// inform the predictor that this block has reached
			// this position
			pred->block_is_dead (tid, &blocks[way], blocks[way].trace_buffer, true, blocks[way].conf, blocks[way].lru_stack_position);
		}
	}
	blocks[i].lru_stack_position = 0;

	if (is_fill) {
		// fill the victim block

		blocks[i].tag = partial_tag;
	}

	// record the trace

	make_trace (tid, pred, real_set, PC, tag, accessType, position == 0, is_fill, lastmiss_bits[real_set], paddr & 63);
	memcpy (blocks[i].trace_buffer, trace_buffer, (MAX_PATH_LENGTH+1) * sizeof (unsigned int));

	// get the next prediction for this entry

	blocks[i].conf = pred->get_prediction (tid, -1);
}

sdbp_sampler::sdbp_sampler (int nsets, int assoc) {
	if (num_core == 1) assert (!dan_use_rrip);
	if (num_core == 4) assert (dan_use_rrip);
	leaders1 = dan_leaders * 1;
	leaders2 = dan_leaders * 2;

	// here, we figure out the total number of bits used by the various
	// structures etc.  along the way we will figure out how many
	// sampler sets we have room for

	dan_predictor_table_entries = 1 << dan_predictor_index_bits;
	nsampler_sets = dan_samplers;

	if (nsampler_sets > nsets) {
		nsampler_sets = nsets;
		fprintf (stderr, "warning: number of sampler sets exceeds number of real sets, setting nsampler_sets to %d\n", nsampler_sets);
		fflush (stderr);
	}

	// compute the maximum saturating counter value; predictor constructor
	// needs this so we do it here

	dan_counter_max = (1 << (dan_counter_width-1)) -1;
	dan_counter_min = -(1 << (dan_counter_width-1));

	// make a predictor

	pred = new perceptron_predictor ();

	// we should have at least one sampler set

	assert (nsampler_sets >= 0);

	// make the sampler sets

	sets = new sdbp_sampler_set [nsampler_sets];
}

// constructor for the predictor

perceptron_predictor::perceptron_predictor (void) {

	// make the tables

	tables = new int* [dan_predictor_tables];
	table_sizes = new int[dan_predictor_tables];

	// initialize each table

	for (int i=0; i<dan_predictor_tables; i++) {
		int table_entries;
		switch (specs[i].type) {
		// 1 bit features
		case F_BIAS:
		case F_BURST:
		case F_LM:
		case F_INS:
			if (specs[i].xorpc == 0) table_entries = 2; else if (specs[i].xorpc == 2) table_entries = 4; else table_entries = dan_predictor_table_entries;
			break;
		case F_OFF:
			if (specs[i].xorpc == false) table_entries = 1<<(specs[i].end-specs[i].begin); else table_entries = dan_predictor_table_entries;
			break;
		default:
			table_entries = dan_predictor_table_entries;
		}
		table_sizes[i] = table_entries;
		tables[i] = new int[table_entries];
		for (int j=0; j<table_entries; j++) tables[i][j] = dan_init_weight;
		total_bits += 6 * table_entries;
	}
}

// inform the predictor that a block is either dead or not dead
// NOTE: the trace_buffer parameter here is from the block in the sampled
// set, not the global trace_buffer variable. yes, it is a hack.

void perceptron_predictor::block_is_dead (uint32_t tid, sdbp_sampler_entry *block, unsigned int *trace_buffer, bool d, int conf, int pos) {

	bool prediction = conf >= dan_threshold;
	bool correct = prediction == d;

	// perceptron learning rule: don't train if the prediction is
	// correct and the confidence is greater than some theta

	bool do_train = false;
	if (conf < 0) {
		if (conf > -dan_theta2) do_train = true;
	} else {
		if (conf < dan_theta) do_train = true;
	}
	if (!correct) do_train = true;
	if (!do_train) return;

	for (int i=0; i<dan_predictor_tables; i++) {
		// for a "dead" block, only train wrt the associativity
		// for this feature

		if (d) if (specs[i].assoc != pos) continue;

		// for a "live" block, only train if it would have been a hit not a placement

		if (!d) if (specs[i].assoc <= pos) continue;

		// ...get a pointer to the corresponding entry in that table

		//int *c = &tables[i][trace_buffer[i] & ((1<<dan_predictor_index_bits)-1)];
		int *c = &tables[i][trace_buffer[i] % table_sizes[i]];

		// if the block is dead, increment the counter

		if (d) {
			if (*c < dan_counter_max) (*c)++;
		} else {
			if (*c > dan_counter_min) (*c)--;
		}
	}
}

// get a prediction for a given trace
// the trace is in trace_buffer[0..MAX_PATH_LENGTH]

int perceptron_predictor::get_prediction (uint32_t tid, int set) {

	// start the confidence sum as 0

	int conf = 0;

	// for each table...
	for (int i=0; i<dan_predictor_tables; i++) {

		// ...get the counter value for that table...

		//int val = tables[i][trace_buffer[i] & ((1<<dan_predictor_index_bits)-1)];
		int val = tables[i][trace_buffer[i] % table_sizes[i]];

		// and add it to the running total

		conf += val;
	}

	// if the counter is at least the threshold, the block is predicted dead

	// keep stored confidence to 9 bits
	if (conf > 255) conf = 255;
	if (conf < -256) conf = -256;
	return conf;
}
