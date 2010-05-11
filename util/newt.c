#define _GNU_SOURCE
#include <stdio.h>
#undef _GNU_SOURCE

#include <stdlib.h>
#include <newt.h>
#include <sys/ttydefaults.h>

#include "cache.h"
#include "hist.h"
#include "session.h"
#include "sort.h"
#include "symbol.h"

struct ui_progress {
	newtComponent form, scale;
};

struct ui_progress *ui_progress__new(const char *title, u64 total)
{
	struct ui_progress *self = malloc(sizeof(*self));

	if (self != NULL) {
		int cols;
		newtGetScreenSize(&cols, NULL);
		cols -= 4;
		newtCenteredWindow(cols, 1, title);
		self->form  = newtForm(NULL, NULL, 0);
		if (self->form == NULL)
			goto out_free_self;
		self->scale = newtScale(0, 0, cols, total);
		if (self->scale == NULL)
			goto out_free_form;
		newtFormAddComponent(self->form, self->scale);
		newtRefresh();
	}

	return self;

out_free_form:
	newtFormDestroy(self->form);
out_free_self:
	free(self);
	return NULL;
}

void ui_progress__update(struct ui_progress *self, u64 curr)
{
	newtScaleSet(self->scale, curr);
	newtRefresh();
}

void ui_progress__delete(struct ui_progress *self)
{
	newtFormDestroy(self->form);
	newtPopWindow();
	free(self);
}

static char browser__last_msg[1024];

int browser__show_help(const char *format, va_list ap)
{
	int ret;
	static int backlog;

        ret = vsnprintf(browser__last_msg + backlog,
			sizeof(browser__last_msg) - backlog, format, ap);
	backlog += ret;

	if (browser__last_msg[backlog - 1] == '\n') {
		newtPopHelpLine();
		newtPushHelpLine(browser__last_msg);
		newtRefresh();
		backlog = 0;
	}

	return ret;
}

static void newt_form__set_exit_keys(newtComponent self)
{
	newtFormAddHotKey(self, NEWT_KEY_ESCAPE);
	newtFormAddHotKey(self, 'Q');
	newtFormAddHotKey(self, 'q');
	newtFormAddHotKey(self, CTRL('c'));
}

static newtComponent newt_form__new(void)
{
	newtComponent self = newtForm(NULL, NULL, 0);
	if (self)
		newt_form__set_exit_keys(self);
	return self;
}

static int popup_menu(int argc, char * const argv[])
{
	struct newtExitStruct es;
	int i, rc = -1, max_len = 5;
	newtComponent listbox, form = newt_form__new();

	if (form == NULL)
		return -1;

	listbox = newtListbox(0, 0, argc, NEWT_FLAG_RETURNEXIT);
	if (listbox == NULL)
		goto out_destroy_form;

	newtFormAddComponent(form, listbox);

	for (i = 0; i < argc; ++i) {
		int len = strlen(argv[i]);
		if (len > max_len)
			max_len = len;
		if (newtListboxAddEntry(listbox, argv[i], (void *)(long)i))
			goto out_destroy_form;
	}

	newtCenteredWindow(max_len, argc, NULL);
	newtFormRun(form, &es);
	rc = newtListboxGetCurrent(listbox) - NULL;
	if (es.reason == NEWT_EXIT_HOTKEY)
		rc = -1;
	newtPopWindow();
out_destroy_form:
	newtFormDestroy(form);
	return rc;
}

static bool dialog_yesno(const char *msg)
{
	/* newtWinChoice should really be accepting const char pointers... */
	char yes[] = "Yes", no[] = "No";
	return newtWinChoice(NULL, yes, no, (char *)msg) == 1;
}

/*
 * When debugging newt problems it was useful to be able to "unroll"
 * the calls to newtCheckBoxTreeAdd{Array,Item}, so that we can generate
 * a source file with the sequence of calls to these methods, to then
 * tweak the arrays to get the intended results, so I'm keeping this code
 * here, may be useful again in the future.
 */
#undef NEWT_DEBUG

static void newt_checkbox_tree__add(newtComponent tree, const char *str,
				    void *priv, int *indexes)
{
#ifdef NEWT_DEBUG
	/* Print the newtCheckboxTreeAddArray to tinker with its index arrays */
	int i = 0, len = 40 - strlen(str);

	fprintf(stderr,
		"\tnewtCheckboxTreeAddItem(tree, %*.*s\"%s\", (void *)%p, 0, ",
		len, len, " ", str, priv);
	while (indexes[i] != NEWT_ARG_LAST) {
		if (indexes[i] != NEWT_ARG_APPEND)
			fprintf(stderr, " %d,", indexes[i]);
		else
			fprintf(stderr, " %s,", "NEWT_ARG_APPEND");
		++i;
	}
	fprintf(stderr, " %s", " NEWT_ARG_LAST);\n");
	fflush(stderr);
#endif
	newtCheckboxTreeAddArray(tree, str, priv, 0, indexes);
}

static char *callchain_list__sym_name(struct callchain_list *self,
				      char *bf, size_t bfsize)
{
	if (self->ms.sym)
		return self->ms.sym->name;

	snprintf(bf, bfsize, "%#Lx", self->ip);
	return bf;
}

static void __callchain__append_graph_browser(struct callchain_node *self,
					      newtComponent tree, u64 total,
					      int *indexes, int depth)
{
	struct rb_node *node;
	u64 new_total, remaining;
	int idx = 0;

	if (callchain_param.mode == CHAIN_GRAPH_REL)
		new_total = self->children_hit;
	else
		new_total = total;

	remaining = new_total;
	node = rb_first(&self->rb_root);
	while (node) {
		struct callchain_node *child = rb_entry(node, struct callchain_node, rb_node);
		struct rb_node *next = rb_next(node);
		u64 cumul = cumul_hits(child);
		struct callchain_list *chain;
		int first = true, printed = 0;
		int chain_idx = -1;
		remaining -= cumul;

		indexes[depth] = NEWT_ARG_APPEND;
		indexes[depth + 1] = NEWT_ARG_LAST;

		list_for_each_entry(chain, &child->val, list) {
			char ipstr[BITS_PER_LONG / 4 + 1],
			     *alloc_str = NULL;
			const char *str = callchain_list__sym_name(chain, ipstr, sizeof(ipstr));

			if (first) {
				double percent = cumul * 100.0 / new_total;

				first = false;
				if (asprintf(&alloc_str, "%2.2f%% %s", percent, str) < 0)
					str = "Not enough memory!";
				else
					str = alloc_str;
			} else {
				indexes[depth] = idx;
				indexes[depth + 1] = NEWT_ARG_APPEND;
				indexes[depth + 2] = NEWT_ARG_LAST;
				++chain_idx;
			}
			newt_checkbox_tree__add(tree, str, &chain->ms, indexes);
			free(alloc_str);
			++printed;
		}

		indexes[depth] = idx;
		if (chain_idx != -1)
			indexes[depth + 1] = chain_idx;
		if (printed != 0)
			++idx;
		__callchain__append_graph_browser(child, tree, new_total, indexes,
						  depth + (chain_idx != -1 ? 2 : 1));
		node = next;
	}
}

static void callchain__append_graph_browser(struct callchain_node *self,
					    newtComponent tree, u64 total,
					    int *indexes, int parent_idx)
{
	struct callchain_list *chain;
	int i = 0;

	indexes[1] = NEWT_ARG_APPEND;
	indexes[2] = NEWT_ARG_LAST;

	list_for_each_entry(chain, &self->val, list) {
		char ipstr[BITS_PER_LONG / 4 + 1], *str;

		if (chain->ip >= PERF_CONTEXT_MAX)
			continue;

		if (!i++ && sort__first_dimension == SORT_SYM)
			continue;

		str = callchain_list__sym_name(chain, ipstr, sizeof(ipstr));
		newt_checkbox_tree__add(tree, str, &chain->ms, indexes);
	}

	indexes[1] = parent_idx;
	indexes[2] = NEWT_ARG_APPEND;
	indexes[3] = NEWT_ARG_LAST;
	__callchain__append_graph_browser(self, tree, total, indexes, 2);
}

static void hist_entry__append_callchain_browser(struct hist_entry *self,
						 newtComponent tree, u64 total, int parent_idx)
{
	struct rb_node *rb_node;
	int indexes[1024] = { [0] = parent_idx, };
	int idx = 0;
	struct callchain_node *chain;

	rb_node = rb_first(&self->sorted_chain);
	while (rb_node) {
		chain = rb_entry(rb_node, struct callchain_node, rb_node);
		switch (callchain_param.mode) {
		case CHAIN_FLAT:
			break;
		case CHAIN_GRAPH_ABS: /* falldown */
		case CHAIN_GRAPH_REL:
			callchain__append_graph_browser(chain, tree, total, indexes, idx++);
			break;
		case CHAIN_NONE:
		default:
			break;
		}
		rb_node = rb_next(rb_node);
	}
}

static size_t hist_entry__append_browser(struct hist_entry *self,
					 newtComponent tree, u64 total)
{
	char s[256];
	size_t ret;

	if (symbol_conf.exclude_other && !self->parent)
		return 0;

	ret = hist_entry__snprintf(self, s, sizeof(s), NULL,
				   false, 0, false, total);
	if (symbol_conf.use_callchain) {
		int indexes[2];

		indexes[0] = NEWT_ARG_APPEND;
		indexes[1] = NEWT_ARG_LAST;
		newt_checkbox_tree__add(tree, s, &self->ms, indexes);
	} else
		newtListboxAppendEntry(tree, s, &self->ms);

	return ret;
}

static void map_symbol__annotate_browser(const struct map_symbol *self,
					 const char *input_name)
{
	FILE *fp;
	int cols, rows;
	newtComponent form, tree;
	struct newtExitStruct es;
	char *str;
	size_t line_len, max_line_len = 0;
	size_t max_usable_width;
	char *line = NULL;

	if (self->sym == NULL)
		return;

	if (asprintf(&str, "perf annotate -i \"%s\" -d \"%s\" %s 2>&1 | expand",
		     input_name, self->map->dso->name, self->sym->name) < 0)
		return;

	fp = popen(str, "r");
	if (fp == NULL)
		goto out_free_str;

	newtPushHelpLine("Press ESC to exit");
	newtGetScreenSize(&cols, &rows);
	tree = newtListbox(0, 0, rows - 5, NEWT_FLAG_SCROLL);

	while (!feof(fp)) {
		if (getline(&line, &line_len, fp) < 0 || !line_len)
			break;
		while (line_len != 0 && isspace(line[line_len - 1]))
			line[--line_len] = '\0';

		if (line_len > max_line_len)
			max_line_len = line_len;
		newtListboxAppendEntry(tree, line, NULL);
	}
	fclose(fp);
	free(line);

	max_usable_width = cols - 22;
	if (max_line_len > max_usable_width)
		max_line_len = max_usable_width;

	newtListboxSetWidth(tree, max_line_len);

	newtCenteredWindow(max_line_len + 2, rows - 5, self->sym->name);
	form = newt_form__new();
	newtFormAddComponent(form, tree);

	newtFormRun(form, &es);
	newtFormDestroy(form);
	newtPopWindow();
	newtPopHelpLine();
out_free_str:
	free(str);
}

static const void *newt__symbol_tree_get_current(newtComponent self)
{
	if (symbol_conf.use_callchain)
		return newtCheckboxTreeGetCurrent(self);
	return newtListboxGetCurrent(self);
}

static void hist_browser__selection(newtComponent self, void *data)
{
	const struct map_symbol **symbol_ptr = data;
	*symbol_ptr = newt__symbol_tree_get_current(self);
}

struct hist_browser {
	newtComponent		form, tree;
	const struct map_symbol *selection;
};

static struct hist_browser *hist_browser__new(void)
{
	struct hist_browser *self = malloc(sizeof(*self));

	if (self != NULL)
		self->form = NULL;

	return self;
}

static void hist_browser__delete(struct hist_browser *self)
{
	newtFormDestroy(self->form);
	newtPopWindow();
	free(self);
}

static int hist_browser__populate(struct hist_browser *self, struct hists *hists,
				  const char *title)
{
	int max_len = 0, idx, cols, rows;
	struct ui_progress *progress;
	struct rb_node *nd;
	u64 curr_hist = 0;
	char seq[] = ".";
	char str[256];

	if (self->form) {
		newtFormDestroy(self->form);
		newtPopWindow();
	}

	snprintf(str, sizeof(str), "Samples: %Ld                            ",
		 hists->stats.total);
	newtDrawRootText(0, 0, str);

	newtGetScreenSize(NULL, &rows);

	if (symbol_conf.use_callchain)
		self->tree = newtCheckboxTreeMulti(0, 0, rows - 5, seq,
						   NEWT_FLAG_SCROLL);
	else
		self->tree = newtListbox(0, 0, rows - 5,
					(NEWT_FLAG_SCROLL |
					 NEWT_FLAG_RETURNEXIT));

	newtComponentAddCallback(self->tree, hist_browser__selection,
				 &self->selection);

	progress = ui_progress__new("Adding entries to the browser...",
				    hists->nr_entries);
	if (progress == NULL)
		return -1;

	idx = 0;
	for (nd = rb_first(&hists->entries); nd; nd = rb_next(nd)) {
		struct hist_entry *h = rb_entry(nd, struct hist_entry, rb_node);
		int len;

		if (h->filtered)
			continue;

		len = hist_entry__append_browser(h, self->tree, hists->stats.total);
		if (len > max_len)
			max_len = len;
		if (symbol_conf.use_callchain)
			hist_entry__append_callchain_browser(h, self->tree,
							     hists->stats.total, idx++);
		++curr_hist;
		if (curr_hist % 5)
			ui_progress__update(progress, curr_hist);
	}

	ui_progress__delete(progress);

	newtGetScreenSize(&cols, &rows);

	if (max_len > cols)
		max_len = cols - 3;

	if (!symbol_conf.use_callchain)
		newtListboxSetWidth(self->tree, max_len);

	newtCenteredWindow(max_len + (symbol_conf.use_callchain ? 5 : 0),
			   rows - 5, title);
	self->form = newt_form__new();
	if (self->form == NULL)
		return -1;

	newtFormAddHotKey(self->form, 'A');
	newtFormAddHotKey(self->form, 'a');
	newtFormAddHotKey(self->form, NEWT_KEY_RIGHT);
	newtFormAddComponents(self->form, self->tree, NULL);
	self->selection = newt__symbol_tree_get_current(self->tree);

	return 0;
}

static struct thread *hist_browser__selected_thread(struct hist_browser *self)
{
	int *indexes;

	if (!symbol_conf.use_callchain)
		goto out;

	indexes = newtCheckboxTreeFindItem(self->tree, (void *)self->selection);
	if (indexes) {
		bool is_hist_entry = indexes[1] == NEWT_ARG_LAST;
		free(indexes);
		if (is_hist_entry)
			goto out;
	}
	return NULL;
out:
	return *(struct thread **)(self->selection + 1);
}

static int hist_browser__title(char *bf, size_t size, const char *input_name,
			       const struct dso *dso, const struct thread *thread)
{
	int printed = 0;

	if (thread)
		printed += snprintf(bf + printed, size - printed,
				    "Thread: %s(%d)",
				    (thread->comm_set ?  thread->comm : ""),
				    thread->pid);
	if (dso)
		printed += snprintf(bf + printed, size - printed,
				    "%sDSO: %s", thread ? " " : "",
				    dso->short_name);
	return printed ?: snprintf(bf, size, "Report: %s", input_name);
}

int hists__browse(struct hists *self, const char *helpline, const char *input_name)
{
	struct hist_browser *browser = hist_browser__new();
	const struct thread *thread_filter = NULL;
	const struct dso *dso_filter = NULL;
	struct newtExitStruct es;
	char msg[160];
	int err = -1;

	if (browser == NULL)
		return -1;

	newtPushHelpLine(helpline);

	hist_browser__title(msg, sizeof(msg), input_name,
			    dso_filter, thread_filter);
	if (hist_browser__populate(browser, self, msg) < 0)
		goto out;

	while (1) {
		const struct thread *thread;
		const struct dso *dso;
		char *options[16];
		int nr_options = 0, choice = 0, i,
		    annotate = -2, zoom_dso = -2, zoom_thread = -2;

		newtFormRun(browser->form, &es);
		if (es.reason == NEWT_EXIT_HOTKEY) {
			if (toupper(es.u.key) == 'A')
				goto do_annotate;
			if (es.u.key == NEWT_KEY_ESCAPE ||
			    toupper(es.u.key) == 'Q' ||
			    es.u.key == CTRL('c')) {
				if (dialog_yesno("Do you really want to exit?"))
					break;
				else
					continue;
			}
		}

		if (browser->selection->sym != NULL &&
		    asprintf(&options[nr_options], "Annotate %s",
			     browser->selection->sym->name) > 0)
			annotate = nr_options++;

		thread = hist_browser__selected_thread(browser);
		if (thread != NULL &&
		    asprintf(&options[nr_options], "Zoom %s %s(%d) thread",
			     (thread_filter ? "out of" : "into"),
			     (thread->comm_set ? thread->comm : ""),
			     thread->pid) > 0)
			zoom_thread = nr_options++;

		dso = browser->selection->map ? browser->selection->map->dso : NULL;
		if (dso != NULL &&
		    asprintf(&options[nr_options], "Zoom %s %s DSO",
			     (dso_filter ? "out of" : "into"),
			     (dso->kernel ? "the Kernel" : dso->short_name)) > 0)
			zoom_dso = nr_options++;

		options[nr_options++] = (char *)"Exit";

		choice = popup_menu(nr_options, options);

		for (i = 0; i < nr_options - 1; ++i)
			free(options[i]);

		if (choice == nr_options - 1)
			break;

		if (choice == -1)
			continue;
do_annotate:
		if (choice == annotate) {
			if (browser->selection->map->dso->origin == DSO__ORIG_KERNEL) {
				newtPopHelpLine();
				newtPushHelpLine("No vmlinux file found, can't "
						 "annotate with just a "
						 "kallsyms file");
				continue;
			}
			map_symbol__annotate_browser(browser->selection, input_name);
		} else if (choice == zoom_dso) {
			if (dso_filter) {
				newtPopHelpLine();
				dso_filter = NULL;
			} else {
				snprintf(msg, sizeof(msg),
					 "To zoom out press -> + \"Zoom out of %s DSO\"",
					 dso->kernel ? "the Kernel" : dso->short_name);
				newtPushHelpLine(msg);
				dso_filter = dso;
			}
			hists__filter_by_dso(self, dso_filter);
			hist_browser__title(msg, sizeof(msg), input_name,
					    dso_filter, thread_filter);
			if (hist_browser__populate(browser, self, msg) < 0)
				goto out;
		} else if (choice == zoom_thread) {
			if (thread_filter) {
				newtPopHelpLine();
				thread_filter = NULL;
			} else {
				snprintf(msg, sizeof(msg),
					 "To zoom out press -> + \"Zoom out of %s(%d) thread\"",
					 (thread->comm_set ? thread->comm : ""),
					 thread->pid);
				newtPushHelpLine(msg);
				thread_filter = thread;
			}
			hists__filter_by_thread(self, thread_filter);
			hist_browser__title(msg, sizeof(msg), input_name,
					    dso_filter, thread_filter);
			if (hist_browser__populate(browser, self, msg) < 0)
				goto out;
		}
	}
	err = 0;
out:
	hist_browser__delete(browser);
	return err;
}

void setup_browser(void)
{
	if (!isatty(1))
		return;

	use_browser = true;
	newtInit();
	newtCls();
	newtPushHelpLine(" ");
}

void exit_browser(bool wait_for_ok)
{
	if (use_browser) {
		if (wait_for_ok) {
			char title[] = "Fatal Error", ok[] = "Ok";
			newtWinMessage(title, ok, browser__last_msg);
		}
		newtFinished();
	}
}
