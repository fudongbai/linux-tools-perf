#include "util.h"
#include "build-id.h"
#include "hist.h"
#include "session.h"
#include "sort.h"
#include "evsel.h"
#include <math.h>

static bool hists__filter_entry_by_dso(struct hists *hists,
				       struct hist_entry *he);
static bool hists__filter_entry_by_thread(struct hists *hists,
					  struct hist_entry *he);
static bool hists__filter_entry_by_symbol(struct hists *hists,
					  struct hist_entry *he);

struct callchain_param	callchain_param = {
	.mode	= CHAIN_GRAPH_REL,
	.min_percent = 0.5,
	.order  = ORDER_CALLEE,
	.key	= CCKEY_FUNCTION
};

u16 hists__col_len(struct hists *hists, enum hist_column col)
{
	return hists->col_len[col];
}

void hists__set_col_len(struct hists *hists, enum hist_column col, u16 len)
{
	hists->col_len[col] = len;
}

bool hists__new_col_len(struct hists *hists, enum hist_column col, u16 len)
{
	if (len > hists__col_len(hists, col)) {
		hists__set_col_len(hists, col, len);
		return true;
	}
	return false;
}

void hists__reset_col_len(struct hists *hists)
{
	enum hist_column col;

	for (col = 0; col < HISTC_NR_COLS; ++col)
		hists__set_col_len(hists, col, 0);
}

static void hists__set_unres_dso_col_len(struct hists *hists, int dso)
{
	const unsigned int unresolved_col_width = BITS_PER_LONG / 4;

	if (hists__col_len(hists, dso) < unresolved_col_width &&
	    !symbol_conf.col_width_list_str && !symbol_conf.field_sep &&
	    !symbol_conf.dso_list)
		hists__set_col_len(hists, dso, unresolved_col_width);
}

void hists__calc_col_len(struct hists *hists, struct hist_entry *h)
{
	const unsigned int unresolved_col_width = BITS_PER_LONG / 4;
	int symlen;
	u16 len;

	/*
	 * +4 accounts for '[x] ' priv level info
	 * +2 accounts for 0x prefix on raw addresses
	 * +3 accounts for ' y ' symtab origin info
	 */
	if (h->ms.sym) {
		symlen = h->ms.sym->namelen + 4;
		if (verbose)
			symlen += BITS_PER_LONG / 4 + 2 + 3;
		hists__new_col_len(hists, HISTC_SYMBOL, symlen);
	} else {
		symlen = unresolved_col_width + 4 + 2;
		hists__new_col_len(hists, HISTC_SYMBOL, symlen);
		hists__set_unres_dso_col_len(hists, HISTC_DSO);
	}

	len = thread__comm_len(h->thread);
	if (hists__new_col_len(hists, HISTC_COMM, len))
		hists__set_col_len(hists, HISTC_THREAD, len + 6);

	if (h->ms.map) {
		len = dso__name_len(h->ms.map->dso);
		hists__new_col_len(hists, HISTC_DSO, len);
	}

	if (h->parent)
		hists__new_col_len(hists, HISTC_PARENT, h->parent->namelen);

	if (h->branch_info) {
		if (h->branch_info->from.sym) {
			symlen = (int)h->branch_info->from.sym->namelen + 4;
			if (verbose)
				symlen += BITS_PER_LONG / 4 + 2 + 3;
			hists__new_col_len(hists, HISTC_SYMBOL_FROM, symlen);

			symlen = dso__name_len(h->branch_info->from.map->dso);
			hists__new_col_len(hists, HISTC_DSO_FROM, symlen);
		} else {
			symlen = unresolved_col_width + 4 + 2;
			hists__new_col_len(hists, HISTC_SYMBOL_FROM, symlen);
			hists__set_unres_dso_col_len(hists, HISTC_DSO_FROM);
		}

		if (h->branch_info->to.sym) {
			symlen = (int)h->branch_info->to.sym->namelen + 4;
			if (verbose)
				symlen += BITS_PER_LONG / 4 + 2 + 3;
			hists__new_col_len(hists, HISTC_SYMBOL_TO, symlen);

			symlen = dso__name_len(h->branch_info->to.map->dso);
			hists__new_col_len(hists, HISTC_DSO_TO, symlen);
		} else {
			symlen = unresolved_col_width + 4 + 2;
			hists__new_col_len(hists, HISTC_SYMBOL_TO, symlen);
			hists__set_unres_dso_col_len(hists, HISTC_DSO_TO);
		}
	}

	if (h->mem_info) {
		if (h->mem_info->daddr.sym) {
			symlen = (int)h->mem_info->daddr.sym->namelen + 4
			       + unresolved_col_width + 2;
			hists__new_col_len(hists, HISTC_MEM_DADDR_SYMBOL,
					   symlen);
		} else {
			symlen = unresolved_col_width + 4 + 2;
			hists__new_col_len(hists, HISTC_MEM_DADDR_SYMBOL,
					   symlen);
		}
		if (h->mem_info->daddr.map) {
			symlen = dso__name_len(h->mem_info->daddr.map->dso);
			hists__new_col_len(hists, HISTC_MEM_DADDR_DSO,
					   symlen);
		} else {
			symlen = unresolved_col_width + 4 + 2;
			hists__set_unres_dso_col_len(hists, HISTC_MEM_DADDR_DSO);
		}
	} else {
		symlen = unresolved_col_width + 4 + 2;
		hists__new_col_len(hists, HISTC_MEM_DADDR_SYMBOL, symlen);
		hists__set_unres_dso_col_len(hists, HISTC_MEM_DADDR_DSO);
	}

	hists__new_col_len(hists, HISTC_MEM_LOCKED, 6);
	hists__new_col_len(hists, HISTC_MEM_TLB, 22);
	hists__new_col_len(hists, HISTC_MEM_SNOOP, 12);
	hists__new_col_len(hists, HISTC_MEM_LVL, 21 + 3);
	hists__new_col_len(hists, HISTC_LOCAL_WEIGHT, 12);
	hists__new_col_len(hists, HISTC_GLOBAL_WEIGHT, 12);

	if (h->transaction)
		hists__new_col_len(hists, HISTC_TRANSACTION,
				   hist_entry__transaction_len());
}

void hists__output_recalc_col_len(struct hists *hists, int max_rows)
{
	struct rb_node *next = rb_first(&hists->entries);
	struct hist_entry *n;
	int row = 0;

	hists__reset_col_len(hists);

	while (next && row++ < max_rows) {
		n = rb_entry(next, struct hist_entry, rb_node);
		if (!n->filtered)
			hists__calc_col_len(hists, n);
		next = rb_next(&n->rb_node);
	}
}

static void he_stat__add_cpumode_period(struct he_stat *he_stat,
					unsigned int cpumode, u64 period)
{
	switch (cpumode) {
	case PERF_RECORD_MISC_KERNEL:
		he_stat->period_sys += period;
		break;
	case PERF_RECORD_MISC_USER:
		he_stat->period_us += period;
		break;
	case PERF_RECORD_MISC_GUEST_KERNEL:
		he_stat->period_guest_sys += period;
		break;
	case PERF_RECORD_MISC_GUEST_USER:
		he_stat->period_guest_us += period;
		break;
	default:
		break;
	}
}

static void he_stat__add_period(struct he_stat *he_stat, u64 period,
				u64 weight)
{

	he_stat->period		+= period;
	he_stat->weight		+= weight;
	he_stat->nr_events	+= 1;
}

static void he_stat__add_stat(struct he_stat *dest, struct he_stat *src)
{
	dest->period		+= src->period;
	dest->period_sys	+= src->period_sys;
	dest->period_us		+= src->period_us;
	dest->period_guest_sys	+= src->period_guest_sys;
	dest->period_guest_us	+= src->period_guest_us;
	dest->nr_events		+= src->nr_events;
	dest->weight		+= src->weight;
}

static void he_stat__decay(struct he_stat *he_stat)
{
	he_stat->period = (he_stat->period * 7) / 8;
	he_stat->nr_events = (he_stat->nr_events * 7) / 8;
	/* XXX need decay for weight too? */
}

static bool hists__decay_entry(struct hists *hists, struct hist_entry *he)
{
	u64 prev_period = he->stat.period;
	u64 diff;

	if (prev_period == 0)
		return true;

	he_stat__decay(&he->stat);

	diff = prev_period - he->stat.period;

	hists->stats.total_period -= diff;
	if (!he->filtered)
		hists->stats.total_non_filtered_period -= diff;

	return he->stat.period == 0;
}

void hists__decay_entries(struct hists *hists, bool zap_user, bool zap_kernel)
{
	struct rb_node *next = rb_first(&hists->entries);
	struct hist_entry *n;

	while (next) {
		n = rb_entry(next, struct hist_entry, rb_node);
		next = rb_next(&n->rb_node);
		/*
		 * We may be annotating this, for instance, so keep it here in
		 * case some it gets new samples, we'll eventually free it when
		 * the user stops browsing and it agains gets fully decayed.
		 */
		if (((zap_user && n->level == '.') ||
		     (zap_kernel && n->level != '.') ||
		     hists__decay_entry(hists, n)) &&
		    !n->used) {
			rb_erase(&n->rb_node, &hists->entries);

			if (sort__need_collapse)
				rb_erase(&n->rb_node_in, &hists->entries_collapsed);

			--hists->nr_entries;
			if (!n->filtered)
				--hists->nr_non_filtered_entries;

			hist_entry__free(n);
		}
	}
}

/*
 * histogram, sorted on item, collects periods
 */

static struct hist_entry *hist_entry__new(struct hist_entry *template)
{
	size_t callchain_size = symbol_conf.use_callchain ? sizeof(struct callchain_root) : 0;
	struct hist_entry *he = zalloc(sizeof(*he) + callchain_size);

	if (he != NULL) {
		*he = *template;

		if (he->ms.map)
			he->ms.map->referenced = true;

		if (he->branch_info) {
			/*
			 * This branch info is (a part of) allocated from
			 * sample__resolve_bstack() and will be freed after
			 * adding new entries.  So we need to save a copy.
			 */
			he->branch_info = malloc(sizeof(*he->branch_info));
			if (he->branch_info == NULL) {
				free(he);
				return NULL;
			}

			memcpy(he->branch_info, template->branch_info,
			       sizeof(*he->branch_info));

			if (he->branch_info->from.map)
				he->branch_info->from.map->referenced = true;
			if (he->branch_info->to.map)
				he->branch_info->to.map->referenced = true;
		}

		if (he->mem_info) {
			if (he->mem_info->iaddr.map)
				he->mem_info->iaddr.map->referenced = true;
			if (he->mem_info->daddr.map)
				he->mem_info->daddr.map->referenced = true;
		}

		if (symbol_conf.use_callchain)
			callchain_init(he->callchain);

		INIT_LIST_HEAD(&he->pairs.node);
	}

	return he;
}

static u8 symbol__parent_filter(const struct symbol *parent)
{
	if (symbol_conf.exclude_other && parent == NULL)
		return 1 << HIST_FILTER__PARENT;
	return 0;
}

static struct hist_entry *add_hist_entry(struct hists *hists,
					 struct hist_entry *entry,
					 struct addr_location *al)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hist_entry *he;
	int64_t cmp;
	u64 period = entry->stat.period;
	u64 weight = entry->stat.weight;

	p = &hists->entries_in->rb_node;

	while (*p != NULL) {
		parent = *p;
		he = rb_entry(parent, struct hist_entry, rb_node_in);

		/*
		 * Make sure that it receives arguments in a same order as
		 * hist_entry__collapse() so that we can use an appropriate
		 * function when searching an entry regardless which sort
		 * keys were used.
		 */
		cmp = hist_entry__cmp(he, entry);

		if (!cmp) {
			he_stat__add_period(&he->stat, period, weight);

			/*
			 * This mem info was allocated from sample__resolve_mem
			 * and will not be used anymore.
			 */
			zfree(&entry->mem_info);

			/* If the map of an existing hist_entry has
			 * become out-of-date due to an exec() or
			 * similar, update it.  Otherwise we will
			 * mis-adjust symbol addresses when computing
			 * the history counter to increment.
			 */
			if (he->ms.map != entry->ms.map) {
				he->ms.map = entry->ms.map;
				if (he->ms.map)
					he->ms.map->referenced = true;
			}
			goto out;
		}

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	he = hist_entry__new(entry);
	if (!he)
		return NULL;

	rb_link_node(&he->rb_node_in, parent, p);
	rb_insert_color(&he->rb_node_in, hists->entries_in);
out:
	he_stat__add_cpumode_period(&he->stat, al->cpumode, period);
	return he;
}

struct hist_entry *__hists__add_entry(struct hists *hists,
				      struct addr_location *al,
				      struct symbol *sym_parent,
				      struct branch_info *bi,
				      struct mem_info *mi,
				      u64 period, u64 weight, u64 transaction)
{
	struct hist_entry entry = {
		.thread	= al->thread,
		.comm = thread__comm(al->thread),
		.ms = {
			.map	= al->map,
			.sym	= al->sym,
		},
		.cpu	= al->cpu,
		.ip	= al->addr,
		.level	= al->level,
		.stat = {
			.nr_events = 1,
			.period	= period,
			.weight = weight,
		},
		.parent = sym_parent,
		.filtered = symbol__parent_filter(sym_parent) | al->filtered,
		.hists	= hists,
		.branch_info = bi,
		.mem_info = mi,
		.transaction = transaction,
	};

	return add_hist_entry(hists, &entry, al);
}

int64_t
hist_entry__cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct perf_hpp_fmt *fmt;
	int64_t cmp = 0;

	perf_hpp__for_each_sort_list(fmt) {
		if (perf_hpp__should_skip(fmt))
			continue;

		cmp = fmt->cmp(left, right);
		if (cmp)
			break;
	}

	return cmp;
}

int64_t
hist_entry__collapse(struct hist_entry *left, struct hist_entry *right)
{
	struct perf_hpp_fmt *fmt;
	int64_t cmp = 0;

	perf_hpp__for_each_sort_list(fmt) {
		if (perf_hpp__should_skip(fmt))
			continue;

		cmp = fmt->collapse(left, right);
		if (cmp)
			break;
	}

	return cmp;
}

void hist_entry__free(struct hist_entry *he)
{
	zfree(&he->branch_info);
	zfree(&he->mem_info);
	free_srcline(he->srcline);
	free(he);
}

/*
 * collapse the histogram
 */

static bool hists__collapse_insert_entry(struct hists *hists __maybe_unused,
					 struct rb_root *root,
					 struct hist_entry *he)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct hist_entry *iter;
	int64_t cmp;

	while (*p != NULL) {
		parent = *p;
		iter = rb_entry(parent, struct hist_entry, rb_node_in);

		cmp = hist_entry__collapse(iter, he);

		if (!cmp) {
			he_stat__add_stat(&iter->stat, &he->stat);

			if (symbol_conf.use_callchain) {
				callchain_cursor_reset(&callchain_cursor);
				callchain_merge(&callchain_cursor,
						iter->callchain,
						he->callchain);
			}
			hist_entry__free(he);
			return false;
		}

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&he->rb_node_in, parent, p);
	rb_insert_color(&he->rb_node_in, root);
	return true;
}

static struct rb_root *hists__get_rotate_entries_in(struct hists *hists)
{
	struct rb_root *root;

	pthread_mutex_lock(&hists->lock);

	root = hists->entries_in;
	if (++hists->entries_in > &hists->entries_in_array[1])
		hists->entries_in = &hists->entries_in_array[0];

	pthread_mutex_unlock(&hists->lock);

	return root;
}

static void hists__apply_filters(struct hists *hists, struct hist_entry *he)
{
	hists__filter_entry_by_dso(hists, he);
	hists__filter_entry_by_thread(hists, he);
	hists__filter_entry_by_symbol(hists, he);
}

void hists__collapse_resort(struct hists *hists, struct ui_progress *prog)
{
	struct rb_root *root;
	struct rb_node *next;
	struct hist_entry *n;

	if (!sort__need_collapse)
		return;

	root = hists__get_rotate_entries_in(hists);
	next = rb_first(root);

	while (next) {
		if (session_done())
			break;
		n = rb_entry(next, struct hist_entry, rb_node_in);
		next = rb_next(&n->rb_node_in);

		rb_erase(&n->rb_node_in, root);
		if (hists__collapse_insert_entry(hists, &hists->entries_collapsed, n)) {
			/*
			 * If it wasn't combined with one of the entries already
			 * collapsed, we need to apply the filters that may have
			 * been set by, say, the hist_browser.
			 */
			hists__apply_filters(hists, n);
		}
		if (prog)
			ui_progress__update(prog, 1);
	}
}

static int hist_entry__sort(struct hist_entry *a, struct hist_entry *b)
{
	struct perf_hpp_fmt *fmt;
	int64_t cmp = 0;

	perf_hpp__for_each_sort_list(fmt) {
		if (perf_hpp__should_skip(fmt))
			continue;

		cmp = fmt->sort(a, b);
		if (cmp)
			break;
	}

	return cmp;
}

static void hists__reset_filter_stats(struct hists *hists)
{
	hists->nr_non_filtered_entries = 0;
	hists->stats.total_non_filtered_period = 0;
}

void hists__reset_stats(struct hists *hists)
{
	hists->nr_entries = 0;
	hists->stats.total_period = 0;

	hists__reset_filter_stats(hists);
}

static void hists__inc_filter_stats(struct hists *hists, struct hist_entry *h)
{
	hists->nr_non_filtered_entries++;
	hists->stats.total_non_filtered_period += h->stat.period;
}

void hists__inc_stats(struct hists *hists, struct hist_entry *h)
{
	if (!h->filtered)
		hists__inc_filter_stats(hists, h);

	hists->nr_entries++;
	hists->stats.total_period += h->stat.period;
}

static void __hists__insert_output_entry(struct rb_root *entries,
					 struct hist_entry *he,
					 u64 min_callchain_hits)
{
	struct rb_node **p = &entries->rb_node;
	struct rb_node *parent = NULL;
	struct hist_entry *iter;

	if (symbol_conf.use_callchain)
		callchain_param.sort(&he->sorted_chain, he->callchain,
				      min_callchain_hits, &callchain_param);

	while (*p != NULL) {
		parent = *p;
		iter = rb_entry(parent, struct hist_entry, rb_node);

		if (hist_entry__sort(he, iter) > 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&he->rb_node, parent, p);
	rb_insert_color(&he->rb_node, entries);
}

void hists__output_resort(struct hists *hists)
{
	struct rb_root *root;
	struct rb_node *next;
	struct hist_entry *n;
	u64 min_callchain_hits;

	min_callchain_hits = hists->stats.total_period * (callchain_param.min_percent / 100);

	if (sort__need_collapse)
		root = &hists->entries_collapsed;
	else
		root = hists->entries_in;

	next = rb_first(root);
	hists->entries = RB_ROOT;

	hists__reset_stats(hists);
	hists__reset_col_len(hists);

	while (next) {
		n = rb_entry(next, struct hist_entry, rb_node_in);
		next = rb_next(&n->rb_node_in);

		__hists__insert_output_entry(&hists->entries, n, min_callchain_hits);
		hists__inc_stats(hists, n);

		if (!n->filtered)
			hists__calc_col_len(hists, n);
	}
}

static void hists__remove_entry_filter(struct hists *hists, struct hist_entry *h,
				       enum hist_filter filter)
{
	h->filtered &= ~(1 << filter);
	if (h->filtered)
		return;

	/* force fold unfiltered entry for simplicity */
	h->ms.unfolded = false;
	h->row_offset = 0;

	hists->stats.nr_non_filtered_samples += h->stat.nr_events;

	hists__inc_filter_stats(hists, h);
	hists__calc_col_len(hists, h);
}


static bool hists__filter_entry_by_dso(struct hists *hists,
				       struct hist_entry *he)
{
	if (hists->dso_filter != NULL &&
	    (he->ms.map == NULL || he->ms.map->dso != hists->dso_filter)) {
		he->filtered |= (1 << HIST_FILTER__DSO);
		return true;
	}

	return false;
}

void hists__filter_by_dso(struct hists *hists)
{
	struct rb_node *nd;

	hists->stats.nr_non_filtered_samples = 0;

	hists__reset_filter_stats(hists);
	hists__reset_col_len(hists);

	for (nd = rb_first(&hists->entries); nd; nd = rb_next(nd)) {
		struct hist_entry *h = rb_entry(nd, struct hist_entry, rb_node);

		if (symbol_conf.exclude_other && !h->parent)
			continue;

		if (hists__filter_entry_by_dso(hists, h))
			continue;

		hists__remove_entry_filter(hists, h, HIST_FILTER__DSO);
	}
}

static bool hists__filter_entry_by_thread(struct hists *hists,
					  struct hist_entry *he)
{
	if (hists->thread_filter != NULL &&
	    he->thread != hists->thread_filter) {
		he->filtered |= (1 << HIST_FILTER__THREAD);
		return true;
	}

	return false;
}

void hists__filter_by_thread(struct hists *hists)
{
	struct rb_node *nd;

	hists->stats.nr_non_filtered_samples = 0;

	hists__reset_filter_stats(hists);
	hists__reset_col_len(hists);

	for (nd = rb_first(&hists->entries); nd; nd = rb_next(nd)) {
		struct hist_entry *h = rb_entry(nd, struct hist_entry, rb_node);

		if (hists__filter_entry_by_thread(hists, h))
			continue;

		hists__remove_entry_filter(hists, h, HIST_FILTER__THREAD);
	}
}

static bool hists__filter_entry_by_symbol(struct hists *hists,
					  struct hist_entry *he)
{
	if (hists->symbol_filter_str != NULL &&
	    (!he->ms.sym || strstr(he->ms.sym->name,
				   hists->symbol_filter_str) == NULL)) {
		he->filtered |= (1 << HIST_FILTER__SYMBOL);
		return true;
	}

	return false;
}

void hists__filter_by_symbol(struct hists *hists)
{
	struct rb_node *nd;

	hists->stats.nr_non_filtered_samples = 0;

	hists__reset_filter_stats(hists);
	hists__reset_col_len(hists);

	for (nd = rb_first(&hists->entries); nd; nd = rb_next(nd)) {
		struct hist_entry *h = rb_entry(nd, struct hist_entry, rb_node);

		if (hists__filter_entry_by_symbol(hists, h))
			continue;

		hists__remove_entry_filter(hists, h, HIST_FILTER__SYMBOL);
	}
}

void events_stats__inc(struct events_stats *stats, u32 type)
{
	++stats->nr_events[0];
	++stats->nr_events[type];
}

void hists__inc_nr_events(struct hists *hists, u32 type)
{
	events_stats__inc(&hists->stats, type);
}

void hists__inc_nr_samples(struct hists *hists, bool filtered)
{
	events_stats__inc(&hists->stats, PERF_RECORD_SAMPLE);
	if (!filtered)
		hists->stats.nr_non_filtered_samples++;
}

static struct hist_entry *hists__add_dummy_entry(struct hists *hists,
						 struct hist_entry *pair)
{
	struct rb_root *root;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hist_entry *he;
	int64_t cmp;

	if (sort__need_collapse)
		root = &hists->entries_collapsed;
	else
		root = hists->entries_in;

	p = &root->rb_node;

	while (*p != NULL) {
		parent = *p;
		he = rb_entry(parent, struct hist_entry, rb_node_in);

		cmp = hist_entry__collapse(he, pair);

		if (!cmp)
			goto out;

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	he = hist_entry__new(pair);
	if (he) {
		memset(&he->stat, 0, sizeof(he->stat));
		he->hists = hists;
		rb_link_node(&he->rb_node_in, parent, p);
		rb_insert_color(&he->rb_node_in, root);
		hists__inc_stats(hists, he);
		he->dummy = true;
	}
out:
	return he;
}

static struct hist_entry *hists__find_entry(struct hists *hists,
					    struct hist_entry *he)
{
	struct rb_node *n;

	if (sort__need_collapse)
		n = hists->entries_collapsed.rb_node;
	else
		n = hists->entries_in->rb_node;

	while (n) {
		struct hist_entry *iter = rb_entry(n, struct hist_entry, rb_node_in);
		int64_t cmp = hist_entry__collapse(iter, he);

		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return iter;
	}

	return NULL;
}

/*
 * Look for pairs to link to the leader buckets (hist_entries):
 */
void hists__match(struct hists *leader, struct hists *other)
{
	struct rb_root *root;
	struct rb_node *nd;
	struct hist_entry *pos, *pair;

	if (sort__need_collapse)
		root = &leader->entries_collapsed;
	else
		root = leader->entries_in;

	for (nd = rb_first(root); nd; nd = rb_next(nd)) {
		pos  = rb_entry(nd, struct hist_entry, rb_node_in);
		pair = hists__find_entry(other, pos);

		if (pair)
			hist_entry__add_pair(pair, pos);
	}
}

/*
 * Look for entries in the other hists that are not present in the leader, if
 * we find them, just add a dummy entry on the leader hists, with period=0,
 * nr_events=0, to serve as the list header.
 */
int hists__link(struct hists *leader, struct hists *other)
{
	struct rb_root *root;
	struct rb_node *nd;
	struct hist_entry *pos, *pair;

	if (sort__need_collapse)
		root = &other->entries_collapsed;
	else
		root = other->entries_in;

	for (nd = rb_first(root); nd; nd = rb_next(nd)) {
		pos = rb_entry(nd, struct hist_entry, rb_node_in);

		if (!hist_entry__has_pairs(pos)) {
			pair = hists__add_dummy_entry(leader, pos);
			if (pair == NULL)
				return -1;
			hist_entry__add_pair(pos, pair);
		}
	}

	return 0;
}

u64 hists__total_period(struct hists *hists)
{
	return symbol_conf.filter_relative ? hists->stats.total_non_filtered_period :
		hists->stats.total_period;
}

int parse_filter_percentage(const struct option *opt __maybe_unused,
			    const char *arg, int unset __maybe_unused)
{
	if (!strcmp(arg, "relative"))
		symbol_conf.filter_relative = true;
	else if (!strcmp(arg, "absolute"))
		symbol_conf.filter_relative = false;
	else
		return -1;

	return 0;
}

int perf_hist_config(const char *var, const char *value)
{
	if (!strcmp(var, "hist.percentage"))
		return parse_filter_percentage(NULL, value, 0);

	return 0;
}
