#include "hist.h"
#include "session.h"
#include "sort.h"
#include <math.h>

struct callchain_param	callchain_param = {
	.mode	= CHAIN_GRAPH_REL,
	.min_percent = 0.5
};

static void perf_session__add_cpumode_count(struct hist_entry *he,
					    unsigned int cpumode, u64 count)
{
	switch (cpumode) {
	case PERF_RECORD_MISC_KERNEL:
		he->count_sys += count;
		break;
	case PERF_RECORD_MISC_USER:
		he->count_us += count;
		break;
	case PERF_RECORD_MISC_GUEST_KERNEL:
		he->count_guest_sys += count;
		break;
	case PERF_RECORD_MISC_GUEST_USER:
		he->count_guest_us += count;
		break;
	default:
		break;
	}
}

/*
 * histogram, sorted on item, collects counts
 */

static struct hist_entry *hist_entry__new(struct hist_entry *template)
{
	size_t callchain_size = symbol_conf.use_callchain ? sizeof(struct callchain_node) : 0;
	struct hist_entry *self = malloc(sizeof(*self) + callchain_size);

	if (self != NULL) {
		*self = *template;
		if (symbol_conf.use_callchain)
			callchain_init(self->callchain);
	}

	return self;
}

struct hist_entry *__perf_session__add_hist_entry(struct rb_root *hists,
						  struct addr_location *al,
						  struct symbol *sym_parent,
						  u64 count)
{
	struct rb_node **p = &hists->rb_node;
	struct rb_node *parent = NULL;
	struct hist_entry *he;
	struct hist_entry entry = {
		.thread	= al->thread,
		.ms = {
			.map	= al->map,
			.sym	= al->sym,
		},
		.ip	= al->addr,
		.level	= al->level,
		.count	= count,
		.parent = sym_parent,
	};
	int cmp;

	while (*p != NULL) {
		parent = *p;
		he = rb_entry(parent, struct hist_entry, rb_node);

		cmp = hist_entry__cmp(&entry, he);

		if (!cmp) {
			he->count += count;
			goto out;
		}

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	he = hist_entry__new(&entry);
	if (!he)
		return NULL;
	rb_link_node(&he->rb_node, parent, p);
	rb_insert_color(&he->rb_node, hists);
out:
	perf_session__add_cpumode_count(he, al->cpumode, count);
	return he;
}

int64_t
hist_entry__cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct sort_entry *se;
	int64_t cmp = 0;

	list_for_each_entry(se, &hist_entry__sort_list, list) {
		cmp = se->se_cmp(left, right);
		if (cmp)
			break;
	}

	return cmp;
}

int64_t
hist_entry__collapse(struct hist_entry *left, struct hist_entry *right)
{
	struct sort_entry *se;
	int64_t cmp = 0;

	list_for_each_entry(se, &hist_entry__sort_list, list) {
		int64_t (*f)(struct hist_entry *, struct hist_entry *);

		f = se->se_collapse ?: se->se_cmp;

		cmp = f(left, right);
		if (cmp)
			break;
	}

	return cmp;
}

void hist_entry__free(struct hist_entry *he)
{
	free(he);
}

/*
 * collapse the histogram
 */

static void collapse__insert_entry(struct rb_root *root, struct hist_entry *he)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct hist_entry *iter;
	int64_t cmp;

	while (*p != NULL) {
		parent = *p;
		iter = rb_entry(parent, struct hist_entry, rb_node);

		cmp = hist_entry__collapse(iter, he);

		if (!cmp) {
			iter->count += he->count;
			hist_entry__free(he);
			return;
		}

		if (cmp < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&he->rb_node, parent, p);
	rb_insert_color(&he->rb_node, root);
}

void perf_session__collapse_resort(struct rb_root *hists)
{
	struct rb_root tmp;
	struct rb_node *next;
	struct hist_entry *n;

	if (!sort__need_collapse)
		return;

	tmp = RB_ROOT;
	next = rb_first(hists);

	while (next) {
		n = rb_entry(next, struct hist_entry, rb_node);
		next = rb_next(&n->rb_node);

		rb_erase(&n->rb_node, hists);
		collapse__insert_entry(&tmp, n);
	}

	*hists = tmp;
}

/*
 * reverse the map, sort on count.
 */

static void perf_session__insert_output_hist_entry(struct rb_root *root,
						   struct hist_entry *he,
						   u64 min_callchain_hits)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct hist_entry *iter;

	if (symbol_conf.use_callchain)
		callchain_param.sort(&he->sorted_chain, he->callchain,
				      min_callchain_hits, &callchain_param);

	while (*p != NULL) {
		parent = *p;
		iter = rb_entry(parent, struct hist_entry, rb_node);

		if (he->count > iter->count)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&he->rb_node, parent, p);
	rb_insert_color(&he->rb_node, root);
}

u64 perf_session__output_resort(struct rb_root *hists, u64 total_samples)
{
	struct rb_root tmp;
	struct rb_node *next;
	struct hist_entry *n;
	u64 min_callchain_hits;
	u64 nr_hists = 0;

	min_callchain_hits =
		total_samples * (callchain_param.min_percent / 100);

	tmp = RB_ROOT;
	next = rb_first(hists);

	while (next) {
		n = rb_entry(next, struct hist_entry, rb_node);
		next = rb_next(&n->rb_node);

		rb_erase(&n->rb_node, hists);
		perf_session__insert_output_hist_entry(&tmp, n,
						       min_callchain_hits);
		++nr_hists;
	}

	*hists = tmp;
	return nr_hists;
}

static size_t callchain__fprintf_left_margin(FILE *fp, int left_margin)
{
	int i;
	int ret = fprintf(fp, "            ");

	for (i = 0; i < left_margin; i++)
		ret += fprintf(fp, " ");

	return ret;
}

static size_t ipchain__fprintf_graph_line(FILE *fp, int depth, int depth_mask,
					  int left_margin)
{
	int i;
	size_t ret = callchain__fprintf_left_margin(fp, left_margin);

	for (i = 0; i < depth; i++)
		if (depth_mask & (1 << i))
			ret += fprintf(fp, "|          ");
		else
			ret += fprintf(fp, "           ");

	ret += fprintf(fp, "\n");

	return ret;
}

static size_t ipchain__fprintf_graph(FILE *fp, struct callchain_list *chain,
				     int depth, int depth_mask, int count,
				     u64 total_samples, int hits,
				     int left_margin)
{
	int i;
	size_t ret = 0;

	ret += callchain__fprintf_left_margin(fp, left_margin);
	for (i = 0; i < depth; i++) {
		if (depth_mask & (1 << i))
			ret += fprintf(fp, "|");
		else
			ret += fprintf(fp, " ");
		if (!count && i == depth - 1) {
			double percent;

			percent = hits * 100.0 / total_samples;
			ret += percent_color_fprintf(fp, "--%2.2f%%-- ", percent);
		} else
			ret += fprintf(fp, "%s", "          ");
	}
	if (chain->ms.sym)
		ret += fprintf(fp, "%s\n", chain->ms.sym->name);
	else
		ret += fprintf(fp, "%p\n", (void *)(long)chain->ip);

	return ret;
}

static struct symbol *rem_sq_bracket;
static struct callchain_list rem_hits;

static void init_rem_hits(void)
{
	rem_sq_bracket = malloc(sizeof(*rem_sq_bracket) + 6);
	if (!rem_sq_bracket) {
		fprintf(stderr, "Not enough memory to display remaining hits\n");
		return;
	}

	strcpy(rem_sq_bracket->name, "[...]");
	rem_hits.ms.sym = rem_sq_bracket;
}

static size_t __callchain__fprintf_graph(FILE *fp, struct callchain_node *self,
					 u64 total_samples, int depth,
					 int depth_mask, int left_margin)
{
	struct rb_node *node, *next;
	struct callchain_node *child;
	struct callchain_list *chain;
	int new_depth_mask = depth_mask;
	u64 new_total;
	u64 remaining;
	size_t ret = 0;
	int i;
	uint entries_printed = 0;

	if (callchain_param.mode == CHAIN_GRAPH_REL)
		new_total = self->children_hit;
	else
		new_total = total_samples;

	remaining = new_total;

	node = rb_first(&self->rb_root);
	while (node) {
		u64 cumul;

		child = rb_entry(node, struct callchain_node, rb_node);
		cumul = cumul_hits(child);
		remaining -= cumul;

		/*
		 * The depth mask manages the output of pipes that show
		 * the depth. We don't want to keep the pipes of the current
		 * level for the last child of this depth.
		 * Except if we have remaining filtered hits. They will
		 * supersede the last child
		 */
		next = rb_next(node);
		if (!next && (callchain_param.mode != CHAIN_GRAPH_REL || !remaining))
			new_depth_mask &= ~(1 << (depth - 1));

		/*
		 * But we keep the older depth mask for the line separator
		 * to keep the level link until we reach the last child
		 */
		ret += ipchain__fprintf_graph_line(fp, depth, depth_mask,
						   left_margin);
		i = 0;
		list_for_each_entry(chain, &child->val, list) {
			ret += ipchain__fprintf_graph(fp, chain, depth,
						      new_depth_mask, i++,
						      new_total,
						      cumul,
						      left_margin);
		}
		ret += __callchain__fprintf_graph(fp, child, new_total,
						  depth + 1,
						  new_depth_mask | (1 << depth),
						  left_margin);
		node = next;
		if (++entries_printed == callchain_param.print_limit)
			break;
	}

	if (callchain_param.mode == CHAIN_GRAPH_REL &&
		remaining && remaining != new_total) {

		if (!rem_sq_bracket)
			return ret;

		new_depth_mask &= ~(1 << (depth - 1));

		ret += ipchain__fprintf_graph(fp, &rem_hits, depth,
					      new_depth_mask, 0, new_total,
					      remaining, left_margin);
	}

	return ret;
}

static size_t callchain__fprintf_graph(FILE *fp, struct callchain_node *self,
				       u64 total_samples, int left_margin)
{
	struct callchain_list *chain;
	bool printed = false;
	int i = 0;
	int ret = 0;
	u32 entries_printed = 0;

	list_for_each_entry(chain, &self->val, list) {
		if (!i++ && sort__first_dimension == SORT_SYM)
			continue;

		if (!printed) {
			ret += callchain__fprintf_left_margin(fp, left_margin);
			ret += fprintf(fp, "|\n");
			ret += callchain__fprintf_left_margin(fp, left_margin);
			ret += fprintf(fp, "---");

			left_margin += 3;
			printed = true;
		} else
			ret += callchain__fprintf_left_margin(fp, left_margin);

		if (chain->ms.sym)
			ret += fprintf(fp, " %s\n", chain->ms.sym->name);
		else
			ret += fprintf(fp, " %p\n", (void *)(long)chain->ip);

		if (++entries_printed == callchain_param.print_limit)
			break;
	}

	ret += __callchain__fprintf_graph(fp, self, total_samples, 1, 1, left_margin);

	return ret;
}

static size_t callchain__fprintf_flat(FILE *fp, struct callchain_node *self,
				      u64 total_samples)
{
	struct callchain_list *chain;
	size_t ret = 0;

	if (!self)
		return 0;

	ret += callchain__fprintf_flat(fp, self->parent, total_samples);


	list_for_each_entry(chain, &self->val, list) {
		if (chain->ip >= PERF_CONTEXT_MAX)
			continue;
		if (chain->ms.sym)
			ret += fprintf(fp, "                %s\n", chain->ms.sym->name);
		else
			ret += fprintf(fp, "                %p\n",
					(void *)(long)chain->ip);
	}

	return ret;
}

static size_t hist_entry_callchain__fprintf(FILE *fp, struct hist_entry *self,
					    u64 total_samples, int left_margin)
{
	struct rb_node *rb_node;
	struct callchain_node *chain;
	size_t ret = 0;
	u32 entries_printed = 0;

	rb_node = rb_first(&self->sorted_chain);
	while (rb_node) {
		double percent;

		chain = rb_entry(rb_node, struct callchain_node, rb_node);
		percent = chain->hit * 100.0 / total_samples;
		switch (callchain_param.mode) {
		case CHAIN_FLAT:
			ret += percent_color_fprintf(fp, "           %6.2f%%\n",
						     percent);
			ret += callchain__fprintf_flat(fp, chain, total_samples);
			break;
		case CHAIN_GRAPH_ABS: /* Falldown */
		case CHAIN_GRAPH_REL:
			ret += callchain__fprintf_graph(fp, chain, total_samples,
							left_margin);
		case CHAIN_NONE:
		default:
			break;
		}
		ret += fprintf(fp, "\n");
		if (++entries_printed == callchain_param.print_limit)
			break;
		rb_node = rb_next(rb_node);
	}

	return ret;
}

int hist_entry__snprintf(struct hist_entry *self,
			   char *s, size_t size,
			   struct perf_session *pair_session,
			   bool show_displacement,
			   long displacement, bool color,
			   u64 session_total)
{
	struct sort_entry *se;
	u64 count, total, count_sys, count_us, count_guest_sys, count_guest_us;
	const char *sep = symbol_conf.field_sep;
	int ret;

	if (symbol_conf.exclude_other && !self->parent)
		return 0;

	if (pair_session) {
		count = self->pair ? self->pair->count : 0;
		total = pair_session->events_stats.total;
		count_sys = self->pair ? self->pair->count_sys : 0;
		count_us = self->pair ? self->pair->count_us : 0;
		count_guest_sys = self->pair ? self->pair->count_guest_sys : 0;
		count_guest_us = self->pair ? self->pair->count_guest_us : 0;
	} else {
		count = self->count;
		total = session_total;
		count_sys = self->count_sys;
		count_us = self->count_us;
		count_guest_sys = self->count_guest_sys;
		count_guest_us = self->count_guest_us;
	}

	if (total) {
		if (color)
			ret = percent_color_snprintf(s, size,
						     sep ? "%.2f" : "   %6.2f%%",
						     (count * 100.0) / total);
		else
			ret = snprintf(s, size, sep ? "%.2f" : "   %6.2f%%",
				       (count * 100.0) / total);
		if (symbol_conf.show_cpu_utilization) {
			ret += percent_color_snprintf(s + ret, size - ret,
					sep ? "%.2f" : "   %6.2f%%",
					(count_sys * 100.0) / total);
			ret += percent_color_snprintf(s + ret, size - ret,
					sep ? "%.2f" : "   %6.2f%%",
					(count_us * 100.0) / total);
			if (perf_guest) {
				ret += percent_color_snprintf(s + ret,
						size - ret,
						sep ? "%.2f" : "   %6.2f%%",
						(count_guest_sys * 100.0) /
								total);
				ret += percent_color_snprintf(s + ret,
						size - ret,
						sep ? "%.2f" : "   %6.2f%%",
						(count_guest_us * 100.0) /
								total);
			}
		}
	} else
		ret = snprintf(s, size, sep ? "%lld" : "%12lld ", count);

	if (symbol_conf.show_nr_samples) {
		if (sep)
			ret += snprintf(s + ret, size - ret, "%c%lld", *sep, count);
		else
			ret += snprintf(s + ret, size - ret, "%11lld", count);
	}

	if (pair_session) {
		char bf[32];
		double old_percent = 0, new_percent = 0, diff;

		if (total > 0)
			old_percent = (count * 100.0) / total;
		if (session_total > 0)
			new_percent = (self->count * 100.0) / session_total;

		diff = new_percent - old_percent;

		if (fabs(diff) >= 0.01)
			snprintf(bf, sizeof(bf), "%+4.2F%%", diff);
		else
			snprintf(bf, sizeof(bf), " ");

		if (sep)
			ret += snprintf(s + ret, size - ret, "%c%s", *sep, bf);
		else
			ret += snprintf(s + ret, size - ret, "%11.11s", bf);

		if (show_displacement) {
			if (displacement)
				snprintf(bf, sizeof(bf), "%+4ld", displacement);
			else
				snprintf(bf, sizeof(bf), " ");

			if (sep)
				ret += snprintf(s + ret, size - ret, "%c%s", *sep, bf);
			else
				ret += snprintf(s + ret, size - ret, "%6.6s", bf);
		}
	}

	list_for_each_entry(se, &hist_entry__sort_list, list) {
		if (se->elide)
			continue;

		ret += snprintf(s + ret, size - ret, "%s", sep ?: "  ");
		ret += se->se_snprintf(self, s + ret, size - ret,
				       se->se_width ? *se->se_width : 0);
	}

	return ret;
}

int hist_entry__fprintf(struct hist_entry *self,
			struct perf_session *pair_session,
			bool show_displacement,
			long displacement, FILE *fp,
			u64 session_total)
{
	char bf[512];
	hist_entry__snprintf(self, bf, sizeof(bf), pair_session,
			     show_displacement, displacement,
			     true, session_total);
	return fprintf(fp, "%s\n", bf);
}

static size_t hist_entry__fprintf_callchain(struct hist_entry *self, FILE *fp,
					    u64 session_total)
{
	int left_margin = 0;

	if (sort__first_dimension == SORT_COMM) {
		struct sort_entry *se = list_first_entry(&hist_entry__sort_list,
							 typeof(*se), list);
		left_margin = se->se_width ? *se->se_width : 0;
		left_margin -= thread__comm_len(self->thread);
	}

	return hist_entry_callchain__fprintf(fp, self, session_total,
					     left_margin);
}

size_t perf_session__fprintf_hists(struct rb_root *hists,
				   struct perf_session *pair,
				   bool show_displacement, FILE *fp,
				   u64 session_total)
{
	struct sort_entry *se;
	struct rb_node *nd;
	size_t ret = 0;
	unsigned long position = 1;
	long displacement = 0;
	unsigned int width;
	const char *sep = symbol_conf.field_sep;
	char *col_width = symbol_conf.col_width_list_str;

	init_rem_hits();

	fprintf(fp, "# %s", pair ? "Baseline" : "Overhead");

	if (symbol_conf.show_nr_samples) {
		if (sep)
			fprintf(fp, "%cSamples", *sep);
		else
			fputs("  Samples  ", fp);
	}

	if (symbol_conf.show_cpu_utilization) {
		if (sep) {
			ret += fprintf(fp, "%csys", *sep);
			ret += fprintf(fp, "%cus", *sep);
			if (perf_guest) {
				ret += fprintf(fp, "%cguest sys", *sep);
				ret += fprintf(fp, "%cguest us", *sep);
			}
		} else {
			ret += fprintf(fp, "  sys  ");
			ret += fprintf(fp, "  us  ");
			if (perf_guest) {
				ret += fprintf(fp, "  guest sys  ");
				ret += fprintf(fp, "  guest us  ");
			}
		}
	}

	if (pair) {
		if (sep)
			ret += fprintf(fp, "%cDelta", *sep);
		else
			ret += fprintf(fp, "  Delta    ");

		if (show_displacement) {
			if (sep)
				ret += fprintf(fp, "%cDisplacement", *sep);
			else
				ret += fprintf(fp, " Displ");
		}
	}

	list_for_each_entry(se, &hist_entry__sort_list, list) {
		if (se->elide)
			continue;
		if (sep) {
			fprintf(fp, "%c%s", *sep, se->se_header);
			continue;
		}
		width = strlen(se->se_header);
		if (se->se_width) {
			if (symbol_conf.col_width_list_str) {
				if (col_width) {
					*se->se_width = atoi(col_width);
					col_width = strchr(col_width, ',');
					if (col_width)
						++col_width;
				}
			}
			width = *se->se_width = max(*se->se_width, width);
		}
		fprintf(fp, "  %*s", width, se->se_header);
	}
	fprintf(fp, "\n");

	if (sep)
		goto print_entries;

	fprintf(fp, "# ........");
	if (symbol_conf.show_nr_samples)
		fprintf(fp, " ..........");
	if (pair) {
		fprintf(fp, " ..........");
		if (show_displacement)
			fprintf(fp, " .....");
	}
	list_for_each_entry(se, &hist_entry__sort_list, list) {
		unsigned int i;

		if (se->elide)
			continue;

		fprintf(fp, "  ");
		if (se->se_width)
			width = *se->se_width;
		else
			width = strlen(se->se_header);
		for (i = 0; i < width; i++)
			fprintf(fp, ".");
	}

	fprintf(fp, "\n#\n");

print_entries:
	for (nd = rb_first(hists); nd; nd = rb_next(nd)) {
		struct hist_entry *h = rb_entry(nd, struct hist_entry, rb_node);

		if (show_displacement) {
			if (h->pair != NULL)
				displacement = ((long)h->pair->position -
					        (long)position);
			else
				displacement = 0;
			++position;
		}
		ret += hist_entry__fprintf(h, pair, show_displacement,
					   displacement, fp, session_total);

		if (symbol_conf.use_callchain)
			ret += hist_entry__fprintf_callchain(h, fp, session_total);

		if (h->ms.map == NULL && verbose > 1) {
			__map_groups__fprintf_maps(&h->thread->mg,
						   MAP__FUNCTION, verbose, fp);
			fprintf(fp, "%.10s end\n", graph_dotted_line);
		}
	}

	free(rem_sq_bracket);

	return ret;
}
