// SPDX-License-Identifier: GPL-2.0
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <regex.h>
#include "callchain.h"
#include "debug.h"
#include "event.h"
#include "evsel.h"
#include "hist.h"
#include "machine.h"
#include "map.h"
#include "sort.h"
#include "strlist.h"
#include "thread.h"
#include "vdso.h"
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unwind.h"
#include "linux/hash.h"
#include "asm/bug.h"
#include "bpf-event.h"

#include "sane_ctype.h"
#include <symbol/kallsyms.h>
#include <linux/mman.h>

static void __machine__remove_thread(struct machine *machine, struct thread *th, bool lock);

static void dsos__init(struct dsos *dsos)
{
	INIT_LIST_HEAD(&dsos->head);
	dsos->root = RB_ROOT;
	init_rwsem(&dsos->lock);
}

static void machine__threads_init(struct machine *machine)
{
	int i;

	for (i = 0; i < THREADS__TABLE_SIZE; i++) {
		struct threads *threads = &machine->threads[i];
		threads->entries = RB_ROOT_CACHED;
		init_rwsem(&threads->lock);
		threads->nr = 0;
		INIT_LIST_HEAD(&threads->dead);
		threads->last_match = NULL;
	}
}

static int machine__set_mmap_name(struct machine *machine)
{
	if (machine__is_host(machine))
		machine->mmap_name = strdup("[kernel.kallsyms]");
	else if (machine__is_default_guest(machine))
		machine->mmap_name = strdup("[guest.kernel.kallsyms]");
	else if (asprintf(&machine->mmap_name, "[guest.kernel.kallsyms.%d]",
			  machine->pid) < 0)
		machine->mmap_name = NULL;

	return machine->mmap_name ? 0 : -ENOMEM;
}

int machine__init(struct machine *machine, const char *root_dir, pid_t pid)
{
	int err = -ENOMEM;

	memset(machine, 0, sizeof(*machine));
	map_groups__init(&machine->kmaps, machine);
	RB_CLEAR_NODE(&machine->rb_node);
	dsos__init(&machine->dsos);

	machine__threads_init(machine);

	machine->vdso_info = NULL;
	machine->env = NULL;

	machine->pid = pid;

	machine->id_hdr_size = 0;
	machine->kptr_restrict_warned = false;
	machine->comm_exec = false;
	machine->kernel_start = 0;
	machine->vmlinux_map = NULL;

	machine->root_dir = strdup(root_dir);
	if (machine->root_dir == NULL)
		return -ENOMEM;

	if (machine__set_mmap_name(machine))
		goto out;

	if (pid != HOST_KERNEL_ID) {
		struct thread *thread = machine__findnew_thread(machine, -1,
								pid);
		char comm[64];

		if (thread == NULL)
			goto out;

		snprintf(comm, sizeof(comm), "[guest/%d]", pid);
		thread__set_comm(thread, comm, 0);
		thread__put(thread);
	}

	machine->current_tid = NULL;
	err = 0;

out:
	if (err) {
		zfree(&machine->root_dir);
		zfree(&machine->mmap_name);
	}
	return 0;
}

struct machine *machine__new_host(void)
{
	struct machine *machine = malloc(sizeof(*machine));

	if (machine != NULL) {
		machine__init(machine, "", HOST_KERNEL_ID);

		if (machine__create_kernel_maps(machine) < 0)
			goto out_delete;
	}

	return machine;
out_delete:
	free(machine);
	return NULL;
}

struct machine *machine__new_kallsyms(void)
{
	struct machine *machine = machine__new_host();
	/*
	 * FIXME:
	 * 1) We should switch to machine__load_kallsyms(), i.e. not explicitly
	 *    ask for not using the kcore parsing code, once this one is fixed
	 *    to create a map per module.
	 */
	if (machine && machine__load_kallsyms(machine, "/proc/kallsyms") <= 0) {
		machine__delete(machine);
		machine = NULL;
	}

	return machine;
}

static void dsos__purge(struct dsos *dsos)
{
	struct dso *pos, *n;

	down_write(&dsos->lock);

	list_for_each_entry_safe(pos, n, &dsos->head, node) {
		RB_CLEAR_NODE(&pos->rb_node);
		pos->root = NULL;
		list_del_init(&pos->node);
		dso__put(pos);
	}

	up_write(&dsos->lock);
}

static void dsos__exit(struct dsos *dsos)
{
	dsos__purge(dsos);
	exit_rwsem(&dsos->lock);
}

void machine__delete_threads(struct machine *machine)
{
	struct rb_node *nd;
	int i;

	for (i = 0; i < THREADS__TABLE_SIZE; i++) {
		struct threads *threads = &machine->threads[i];
		down_write(&threads->lock);
		nd = rb_first_cached(&threads->entries);
		while (nd) {
			struct thread *t = rb_entry(nd, struct thread, rb_node);

			nd = rb_next(nd);
			__machine__remove_thread(machine, t, false);
		}
		up_write(&threads->lock);
	}
}

void machine__exit(struct machine *machine)
{
	int i;

	if (machine == NULL)
		return;

	machine__destroy_kernel_maps(machine);
	map_groups__exit(&machine->kmaps);
	dsos__exit(&machine->dsos);
	machine__exit_vdso(machine);
	zfree(&machine->root_dir);
	zfree(&machine->mmap_name);
	zfree(&machine->current_tid);

	for (i = 0; i < THREADS__TABLE_SIZE; i++) {
		struct threads *threads = &machine->threads[i];
		exit_rwsem(&threads->lock);
	}
}

void machine__delete(struct machine *machine)
{
	if (machine) {
		machine__exit(machine);
		free(machine);
	}
}

void machines__init(struct machines *machines)
{
	machine__init(&machines->host, "", HOST_KERNEL_ID);
	machines->guests = RB_ROOT_CACHED;
}

void machines__exit(struct machines *machines)
{
	machine__exit(&machines->host);
	/* XXX exit guest */
}

struct machine *machines__add(struct machines *machines, pid_t pid,
			      const char *root_dir)
{
	struct rb_node **p = &machines->guests.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct machine *pos, *machine = malloc(sizeof(*machine));
	bool leftmost = true;

	if (machine == NULL)
		return NULL;

	if (machine__init(machine, root_dir, pid) != 0) {
		free(machine);
		return NULL;
	}

	while (*p != NULL) {
		parent = *p;
		pos = rb_entry(parent, struct machine, rb_node);
		if (pid < pos->pid)
			p = &(*p)->rb_left;
		else {
			p = &(*p)->rb_right;
			leftmost = false;
		}
	}

	rb_link_node(&machine->rb_node, parent, p);
	rb_insert_color_cached(&machine->rb_node, &machines->guests, leftmost);

	return machine;
}

void machines__set_comm_exec(struct machines *machines, bool comm_exec)
{
	struct rb_node *nd;

	machines->host.comm_exec = comm_exec;

	for (nd = rb_first_cached(&machines->guests); nd; nd = rb_next(nd)) {
		struct machine *machine = rb_entry(nd, struct machine, rb_node);

		machine->comm_exec = comm_exec;
	}
}

struct machine *machines__find(struct machines *machines, pid_t pid)
{
	struct rb_node **p = &machines->guests.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct machine *machine;
	struct machine *default_machine = NULL;

	if (pid == HOST_KERNEL_ID)
		return &machines->host;

	while (*p != NULL) {
		parent = *p;
		machine = rb_entry(parent, struct machine, rb_node);
		if (pid < machine->pid)
			p = &(*p)->rb_left;
		else if (pid > machine->pid)
			p = &(*p)->rb_right;
		else
			return machine;
		if (!machine->pid)
			default_machine = machine;
	}

	return default_machine;
}

struct machine *machines__findnew(struct machines *machines, pid_t pid)
{
	char path[PATH_MAX];
	const char *root_dir = "";
	struct machine *machine = machines__find(machines, pid);

	if (machine && (machine->pid == pid))
		goto out;

	if ((pid != HOST_KERNEL_ID) &&
	    (pid != DEFAULT_GUEST_KERNEL_ID) &&
	    (symbol_conf.guestmount)) {
		sprintf(path, "%s/%d", symbol_conf.guestmount, pid);
		if (access(path, R_OK)) {
			static struct strlist *seen;

			if (!seen)
				seen = strlist__new(NULL, NULL);

			if (!strlist__has_entry(seen, path)) {
				pr_err("Can't access file %s\n", path);
				strlist__add(seen, path);
			}
			machine = NULL;
			goto out;
		}
		root_dir = path;
	}

	machine = machines__add(machines, pid, root_dir);
out:
	return machine;
}

void machines__process_guests(struct machines *machines,
			      machine__process_t process, void *data)
{
	struct rb_node *nd;

	for (nd = rb_first_cached(&machines->guests); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		process(pos, data);
	}
}

void machines__set_id_hdr_size(struct machines *machines, u16 id_hdr_size)
{
	struct rb_node *node;
	struct machine *machine;

	machines->host.id_hdr_size = id_hdr_size;

	for (node = rb_first_cached(&machines->guests); node;
	     node = rb_next(node)) {
		machine = rb_entry(node, struct machine, rb_node);
		machine->id_hdr_size = id_hdr_size;
	}

	return;
}

static void machine__update_thread_pid(struct machine *machine,
				       struct thread *th, pid_t pid)
{
	struct thread *leader;

	if (pid == th->pid_ || pid == -1 || th->pid_ != -1)
		return;

	th->pid_ = pid;

	if (th->pid_ == th->tid)
		return;

	leader = __machine__findnew_thread(machine, th->pid_, th->pid_);
	if (!leader)
		goto out_err;

	if (!leader->mg)
		leader->mg = map_groups__new(machine);

	if (!leader->mg)
		goto out_err;

	if (th->mg == leader->mg)
		return;

	if (th->mg) {
		/*
		 * Maps are created from MMAP events which provide the pid and
		 * tid.  Consequently there never should be any maps on a thread
		 * with an unknown pid.  Just print an error if there are.
		 */
		if (!map_groups__empty(th->mg))
			pr_err("Discarding thread maps for %d:%d\n",
			       th->pid_, th->tid);
		map_groups__put(th->mg);
	}

	th->mg = map_groups__get(leader->mg);
out_put:
	thread__put(leader);
	return;
out_err:
	pr_err("Failed to join map groups for %d:%d\n", th->pid_, th->tid);
	goto out_put;
}

/*
 * Front-end cache - TID lookups come in blocks,
 * so most of the time we dont have to look up
 * the full rbtree:
 */
static struct thread*
__threads__get_last_match(struct threads *threads, struct machine *machine,
			  int pid, int tid)
{
	struct thread *th;

	th = threads->last_match;
	if (th != NULL) {
		if (th->tid == tid) {
			machine__update_thread_pid(machine, th, pid);
			return thread__get(th);
		}

		threads->last_match = NULL;
	}

	return NULL;
}

static struct thread*
threads__get_last_match(struct threads *threads, struct machine *machine,
			int pid, int tid)
{
	struct thread *th = NULL;

	if (perf_singlethreaded)
		th = __threads__get_last_match(threads, machine, pid, tid);

	return th;
}

static void
__threads__set_last_match(struct threads *threads, struct thread *th)
{
	threads->last_match = th;
}

static void
threads__set_last_match(struct threads *threads, struct thread *th)
{
	if (perf_singlethreaded)
		__threads__set_last_match(threads, th);
}

/*
 * Caller must eventually drop thread->refcnt returned with a successful
 * lookup/new thread inserted.
 */
static struct thread *____machine__findnew_thread(struct machine *machine,
						  struct threads *threads,
						  pid_t pid, pid_t tid,
						  bool create)
{
	struct rb_node **p = &threads->entries.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct thread *th;
	bool leftmost = true;

	th = threads__get_last_match(threads, machine, pid, tid);
	if (th)
		return th;

	while (*p != NULL) {
		parent = *p;
		th = rb_entry(parent, struct thread, rb_node);

		if (th->tid == tid) {
			threads__set_last_match(threads, th);
			machine__update_thread_pid(machine, th, pid);
			return thread__get(th);
		}

		if (tid < th->tid)
			p = &(*p)->rb_left;
		else {
			p = &(*p)->rb_right;
			leftmost = false;
		}
	}

	if (!create)
		return NULL;

	th = thread__new(pid, tid);
	if (th != NULL) {
		rb_link_node(&th->rb_node, parent, p);
		rb_insert_color_cached(&th->rb_node, &threads->entries, leftmost);

		/*
		 * We have to initialize map_groups separately
		 * after rb tree is updated.
		 *
		 * The reason is that we call machine__findnew_thread
		 * within thread__init_map_groups to find the thread
		 * leader and that would screwed the rb tree.
		 */
		if (thread__init_map_groups(th, machine)) {
			rb_erase_cached(&th->rb_node, &threads->entries);
			RB_CLEAR_NODE(&th->rb_node);
			thread__put(th);
			return NULL;
		}
		/*
		 * It is now in the rbtree, get a ref
		 */
		thread__get(th);
		threads__set_last_match(threads, th);
		++threads->nr;
	}

	return th;
}

struct thread *__machine__findnew_thread(struct machine *machine, pid_t pid, pid_t tid)
{
	return ____machine__findnew_thread(machine, machine__threads(machine, tid), pid, tid, true);
}

struct thread *machine__findnew_thread(struct machine *machine, pid_t pid,
				       pid_t tid)
{
	struct threads *threads = machine__threads(machine, tid);
	struct thread *th;

	down_write(&threads->lock);
	th = __machine__findnew_thread(machine, pid, tid);
	up_write(&threads->lock);
	return th;
}

struct thread *machine__find_thread(struct machine *machine, pid_t pid,
				    pid_t tid)
{
	struct threads *threads = machine__threads(machine, tid);
	struct thread *th;

	down_read(&threads->lock);
	th =  ____machine__findnew_thread(machine, threads, pid, tid, false);
	up_read(&threads->lock);
	return th;
}

struct comm *machine__thread_exec_comm(struct machine *machine,
				       struct thread *thread)
{
	if (machine->comm_exec)
		return thread__exec_comm(thread);
	else
		return thread__comm(thread);
}

int machine__process_comm_event(struct machine *machine, union perf_event *event,
				struct perf_sample *sample)
{
	struct thread *thread = machine__findnew_thread(machine,
							event->comm.pid,
							event->comm.tid);
	bool exec = event->header.misc & PERF_RECORD_MISC_COMM_EXEC;
	int err = 0;

	if (exec)
		machine->comm_exec = true;

	if (dump_trace)
		perf_event__fprintf_comm(event, stdout);

	if (thread == NULL ||
	    __thread__set_comm(thread, event->comm.comm, sample->time, exec)) {
		dump_printf("problem processing PERF_RECORD_COMM, skipping event.\n");
		err = -1;
	}

	thread__put(thread);

	return err;
}

int machine__process_namespaces_event(struct machine *machine __maybe_unused,
				      union perf_event *event,
				      struct perf_sample *sample __maybe_unused)
{
	struct thread *thread = machine__findnew_thread(machine,
							event->namespaces.pid,
							event->namespaces.tid);
	int err = 0;

	WARN_ONCE(event->namespaces.nr_namespaces > NR_NAMESPACES,
		  "\nWARNING: kernel seems to support more namespaces than perf"
		  " tool.\nTry updating the perf tool..\n\n");

	WARN_ONCE(event->namespaces.nr_namespaces < NR_NAMESPACES,
		  "\nWARNING: perf tool seems to support more namespaces than"
		  " the kernel.\nTry updating the kernel..\n\n");

	if (dump_trace)
		perf_event__fprintf_namespaces(event, stdout);

	if (thread == NULL ||
	    thread__set_namespaces(thread, sample->time, &event->namespaces)) {
		dump_printf("problem processing PERF_RECORD_NAMESPACES, skipping event.\n");
		err = -1;
	}

	thread__put(thread);

	return err;
}

int machine__process_lost_event(struct machine *machine __maybe_unused,
				union perf_event *event, struct perf_sample *sample __maybe_unused)
{
	dump_printf(": id:%" PRIu64 ": lost:%" PRIu64 "\n",
		    event->lost.id, event->lost.lost);
	return 0;
}

int machine__process_lost_samples_event(struct machine *machine __maybe_unused,
					union perf_event *event, struct perf_sample *sample)
{
	dump_printf(": id:%" PRIu64 ": lost samples :%" PRIu64 "\n",
		    sample->id, event->lost_samples.lost);
	return 0;
}

static struct dso *machine__findnew_module_dso(struct machine *machine,
					       struct kmod_path *m,
					       const char *filename)
{
	struct dso *dso;

	down_write(&machine->dsos.lock);

	dso = __dsos__find(&machine->dsos, m->name, true);
	if (!dso) {
		dso = __dsos__addnew(&machine->dsos, m->name);
		if (dso == NULL)
			goto out_unlock;

		dso__set_module_info(dso, m, machine);
		dso__set_long_name(dso, strdup(filename), true);
	}

	dso__get(dso);
out_unlock:
	up_write(&machine->dsos.lock);
	return dso;
}

int machine__process_aux_event(struct machine *machine __maybe_unused,
			       union perf_event *event)
{
	if (dump_trace)
		perf_event__fprintf_aux(event, stdout);
	return 0;
}

int machine__process_itrace_start_event(struct machine *machine __maybe_unused,
					union perf_event *event)
{
	if (dump_trace)
		perf_event__fprintf_itrace_start(event, stdout);
	return 0;
}

int machine__process_switch_event(struct machine *machine __maybe_unused,
				  union perf_event *event)
{
	if (dump_trace)
		perf_event__fprintf_switch(event, stdout);
	return 0;
}

static int machine__process_ksymbol_register(struct machine *machine,
					     union perf_event *event,
					     struct perf_sample *sample __maybe_unused)
{
	struct symbol *sym;
	struct map *map;

	map = map_groups__find(&machine->kmaps, event->ksymbol_event.addr);
	if (!map) {
		map = dso__new_map(event->ksymbol_event.name);
		if (!map)
			return -ENOMEM;

		map->start = event->ksymbol_event.addr;
		map->pgoff = map->start;
		map->end = map->start + event->ksymbol_event.len;
		map_groups__insert(&machine->kmaps, map);
	}

	sym = symbol__new(event->ksymbol_event.addr, event->ksymbol_event.len,
			  0, 0, event->ksymbol_event.name);
	if (!sym)
		return -ENOMEM;
	dso__insert_symbol(map->dso, sym);
	return 0;
}

static int machine__process_ksymbol_unregister(struct machine *machine,
					       union perf_event *event,
					       struct perf_sample *sample __maybe_unused)
{
	struct map *map;

	map = map_groups__find(&machine->kmaps, event->ksymbol_event.addr);
	if (map)
		map_groups__remove(&machine->kmaps, map);

	return 0;
}

int machine__process_ksymbol(struct machine *machine __maybe_unused,
			     union perf_event *event,
			     struct perf_sample *sample)
{
	if (dump_trace)
		perf_event__fprintf_ksymbol(event, stdout);

	if (event->ksymbol_event.flags & PERF_RECORD_KSYMBOL_FLAGS_UNREGISTER)
		return machine__process_ksymbol_unregister(machine, event,
							   sample);
	return machine__process_ksymbol_register(machine, event, sample);
}

static void dso__adjust_kmod_long_name(struct dso *dso, const char *filename)
{
	const char *dup_filename;

	if (!filename || !dso || !dso->long_name)
		return;
	if (dso->long_name[0] != '[')
		return;
	if (!strchr(filename, '/'))
		return;

	dup_filename = strdup(filename);
	if (!dup_filename)
		return;

	dso__set_long_name(dso, dup_filename, true);
}

struct map *machine__findnew_module_map(struct machine *machine, u64 start,
					const char *filename)
{
	struct map *map = NULL;
	struct dso *dso = NULL;
	struct kmod_path m;

	if (kmod_path__parse_name(&m, filename))
		return NULL;

	map = map_groups__find_by_name(&machine->kmaps, m.name);
	if (map) {
		/*
		 * If the map's dso is an offline module, give dso__load()
		 * a chance to find the file path of that module by fixing
		 * long_name.
		 */
		dso__adjust_kmod_long_name(map->dso, filename);
		goto out;
	}

	dso = machine__findnew_module_dso(machine, &m, filename);
	if (dso == NULL)
		goto out;

	map = map__new2(start, dso);
	if (map == NULL)
		goto out;

	map_groups__insert(&machine->kmaps, map);

	/* Put the map here because map_groups__insert alread got it */
	map__put(map);
out:
	/* put the dso here, corresponding to  machine__findnew_module_dso */
	dso__put(dso);
	free(m.name);
	return map;
}

size_t machines__fprintf_dsos(struct machines *machines, FILE *fp)
{
	struct rb_node *nd;
	size_t ret = __dsos__fprintf(&machines->host.dsos.head, fp);

	for (nd = rb_first_cached(&machines->guests); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		ret += __dsos__fprintf(&pos->dsos.head, fp);
	}

	return ret;
}

size_t machine__fprintf_dsos_buildid(struct machine *m, FILE *fp,
				     bool (skip)(struct dso *dso, int parm), int parm)
{
	return __dsos__fprintf_buildid(&m->dsos.head, fp, skip, parm);
}

size_t machines__fprintf_dsos_buildid(struct machines *machines, FILE *fp,
				     bool (skip)(struct dso *dso, int parm), int parm)
{
	struct rb_node *nd;
	size_t ret = machine__fprintf_dsos_buildid(&machines->host, fp, skip, parm);

	for (nd = rb_first_cached(&machines->guests); nd; nd = rb_next(nd)) {
		struct machine *pos = rb_entry(nd, struct machine, rb_node);
		ret += machine__fprintf_dsos_buildid(pos, fp, skip, parm);
	}
	return ret;
}

size_t machine__fprintf_vmlinux_path(struct machine *machine, FILE *fp)
{
	int i;
	size_t printed = 0;
	struct dso *kdso = machine__kernel_map(machine)->dso;

	if (kdso->has_build_id) {
		char filename[PATH_MAX];
		if (dso__build_id_filename(kdso, filename, sizeof(filename),
					   false))
			printed += fprintf(fp, "[0] %s\n", filename);
	}

	for (i = 0; i < vmlinux_path__nr_entries; ++i)
		printed += fprintf(fp, "[%d] %s\n",
				   i + kdso->has_build_id, vmlinux_path[i]);

	return printed;
}

size_t machine__fprintf(struct machine *machine, FILE *fp)
{
	struct rb_node *nd;
	size_t ret;
	int i;

	for (i = 0; i < THREADS__TABLE_SIZE; i++) {
		struct threads *threads = &machine->threads[i];

		down_read(&threads->lock);

		ret = fprintf(fp, "Threads: %u\n", threads->nr);

		for (nd = rb_first_cached(&threads->entries); nd;
		     nd = rb_next(nd)) {
			struct thread *pos = rb_entry(nd, struct thread, rb_node);

			ret += thread__fprintf(pos, fp);
		}

		up_read(&threads->lock);
	}
	return ret;
}

static struct dso *machine__get_kernel(struct machine *machine)
{
	const char *vmlinux_name = machine->mmap_name;
	struct dso *kernel;

	if (machine__is_host(machine)) {
		if (symbol_conf.vmlinux_name)
			vmlinux_name = symbol_conf.vmlinux_name;

		kernel = machine__findnew_kernel(machine, vmlinux_name,
						 "[kernel]", DSO_TYPE_KERNEL);
	} else {
		if (symbol_conf.default_guest_vmlinux_name)
			vmlinux_name = symbol_conf.default_guest_vmlinux_name;

		kernel = machine__findnew_kernel(machine, vmlinux_name,
						 "[guest.kernel]",
						 DSO_TYPE_GUEST_KERNEL);
	}

	if (kernel != NULL && (!kernel->has_build_id))
		dso__read_running_kernel_build_id(kernel, machine);

	return kernel;
}

struct process_args {
	u64 start;
};

void machine__get_kallsyms_filename(struct machine *machine, char *buf,
				    size_t bufsz)
{
	if (machine__is_default_guest(machine))
		scnprintf(buf, bufsz, "%s", symbol_conf.default_guest_kallsyms);
	else
		scnprintf(buf, bufsz, "%s/proc/kallsyms", machine->root_dir);
}

const char *ref_reloc_sym_names[] = {"_text", "_stext", NULL};

/* Figure out the start address of kernel map from /proc/kallsyms.
 * Returns the name of the start symbol in *symbol_name. Pass in NULL as
 * symbol_name if it's not that important.
 */
static int machine__get_running_kernel_start(struct machine *machine,
					     const char **symbol_name, u64 *start)
{
	char filename[PATH_MAX];
	int i, err = -1;
	const char *name;
	u64 addr = 0;

	machine__get_kallsyms_filename(machine, filename, PATH_MAX);

	if (symbol__restricted_filename(filename, "/proc/kallsyms"))
		return 0;

	for (i = 0; (name = ref_reloc_sym_names[i]) != NULL; i++) {
		err = kallsyms__get_function_start(filename, name, &addr);
		if (!err)
			break;
	}

	if (err)
		return -1;

	if (symbol_name)
		*symbol_name = name;

	*start = addr;
	return 0;
}

int machine__create_extra_kernel_map(struct machine *machine,
				     struct dso *kernel,
				     struct extra_kernel_map *xm)
{
	struct kmap *kmap;
	struct map *map;

	map = map__new2(xm->start, kernel);
	if (!map)
		return -1;

	map->end   = xm->end;
	map->pgoff = xm->pgoff;

	kmap = map__kmap(map);

	kmap->kmaps = &machine->kmaps;
	strlcpy(kmap->name, xm->name, KMAP_NAME_LEN);

	map_groups__insert(&machine->kmaps, map);

	pr_debug2("Added extra kernel map %s %" PRIx64 "-%" PRIx64 "\n",
		  kmap->name, map->start, map->end);

	map__put(map);

	return 0;
}

static u64 find_entry_trampoline(struct dso *dso)
{
	/* Duplicates are removed so lookup all aliases */
	const char *syms[] = {
		"_entry_trampoline",
		"__entry_trampoline_start",
		"entry_SYSCALL_64_trampoline",
	};
	struct symbol *sym = dso__first_symbol(dso);
	unsigned int i;

	for (; sym; sym = dso__next_symbol(sym)) {
		if (sym->binding != STB_GLOBAL)
			continue;
		for (i = 0; i < ARRAY_SIZE(syms); i++) {
			if (!strcmp(sym->name, syms[i]))
				return sym->start;
		}
	}

	return 0;
}

/*
 * These values can be used for kernels that do not have symbols for the entry
 * trampolines in kallsyms.
 */
#define X86_64_CPU_ENTRY_AREA_PER_CPU	0xfffffe0000000000ULL
#define X86_64_CPU_ENTRY_AREA_SIZE	0x2c000
#define X86_64_ENTRY_TRAMPOLINE		0x6000

/* Map x86_64 PTI entry trampolines */
int machine__map_x86_64_entry_trampolines(struct machine *machine,
					  struct dso *kernel)
{
	struct map_groups *kmaps = &machine->kmaps;
	struct maps *maps = &kmaps->maps;
	int nr_cpus_avail, cpu;
	bool found = false;
	struct map *map;
	u64 pgoff;

	/*
	 * In the vmlinux case, pgoff is a virtual address which must now be
	 * mapped to a vmlinux offset.
	 */
	for (map = maps__first(maps); map; map = map__next(map)) {
		struct kmap *kmap = __map__kmap(map);
		struct map *dest_map;

		if (!kmap || !is_entry_trampoline(kmap->name))
			continue;

		dest_map = map_groups__find(kmaps, map->pgoff);
		if (dest_map != map)
			map->pgoff = dest_map->map_ip(dest_map, map->pgoff);
		found = true;
	}
	if (found || machine->trampolines_mapped)
		return 0;

	pgoff = find_entry_trampoline(kernel);
	if (!pgoff)
		return 0;

	nr_cpus_avail = machine__nr_cpus_avail(machine);

	/* Add a 1 page map for each CPU's entry trampoline */
	for (cpu = 0; cpu < nr_cpus_avail; cpu++) {
		u64 va = X86_64_CPU_ENTRY_AREA_PER_CPU +
			 cpu * X86_64_CPU_ENTRY_AREA_SIZE +
			 X86_64_ENTRY_TRAMPOLINE;
		struct extra_kernel_map xm = {
			.start = va,
			.end   = va + page_size,
			.pgoff = pgoff,
		};

		strlcpy(xm.name, ENTRY_TRAMPOLINE_NAME, KMAP_NAME_LEN);

		if (machine__create_extra_kernel_map(machine, kernel, &xm) < 0)
			return -1;
	}

	machine->trampolines_mapped = nr_cpus_avail;

	return 0;
}

int __weak machine__create_extra_kernel_maps(struct machine *machine __maybe_unused,
					     struct dso *kernel __maybe_unused)
{
	return 0;
}

static int
__machine__create_kernel_maps(struct machine *machine, struct dso *kernel)
{
	struct kmap *kmap;
	struct map *map;

	/* In case of renewal the kernel map, destroy previous one */
	machine__destroy_kernel_maps(machine);

	machine->vmlinux_map = map__new2(0, kernel);
	if (machine->vmlinux_map == NULL)
		return -1;

	machine->vmlinux_map->map_ip = machine->vmlinux_map->unmap_ip = identity__map_ip;
	map = machine__kernel_map(machine);
	kmap = map__kmap(map);
	if (!kmap)
		return -1;

	kmap->kmaps = &machine->kmaps;
	map_groups__insert(&machine->kmaps, map);

	return 0;
}

void machine__destroy_kernel_maps(struct machine *machine)
{
	struct kmap *kmap;
	struct map *map = machine__kernel_map(machine);

	if (map == NULL)
		return;

	kmap = map__kmap(map);
	map_groups__remove(&machine->kmaps, map);
	if (kmap && kmap->ref_reloc_sym) {
		zfree((char **)&kmap->ref_reloc_sym->name);
		zfree(&kmap->ref_reloc_sym);
	}

	map__zput(machine->vmlinux_map);
}

int machines__create_guest_kernel_maps(struct machines *machines)
{
	int ret = 0;
	struct dirent **namelist = NULL;
	int i, items = 0;
	char path[PATH_MAX];
	pid_t pid;
	char *endp;

	if (symbol_conf.default_guest_vmlinux_name ||
	    symbol_conf.default_guest_modules ||
	    symbol_conf.default_guest_kallsyms) {
		machines__create_kernel_maps(machines, DEFAULT_GUEST_KERNEL_ID);
	}

	if (symbol_conf.guestmount) {
		items = scandir(symbol_conf.guestmount, &namelist, NULL, NULL);
		if (items <= 0)
			return -ENOENT;
		for (i = 0; i < items; i++) {
			if (!isdigit(namelist[i]->d_name[0])) {
				/* Filter out . and .. */
				continue;
			}
			pid = (pid_t)strtol(namelist[i]->d_name, &endp, 10);
			if ((*endp != '\0') ||
			    (endp == namelist[i]->d_name) ||
			    (errno == ERANGE)) {
				pr_debug("invalid directory (%s). Skipping.\n",
					 namelist[i]->d_name);
				continue;
			}
			sprintf(path, "%s/%s/proc/kallsyms",
				symbol_conf.guestmount,
				namelist[i]->d_name);
			ret = access(path, R_OK);
			if (ret) {
				pr_debug("Can't access file %s\n", path);
				goto failure;
			}
			machines__create_kernel_maps(machines, pid);
		}
failure:
		free(namelist);
	}

	return ret;
}

void machines__destroy_kernel_maps(struct machines *machines)
{
	struct rb_node *next = rb_first_cached(&machines->guests);

	machine__destroy_kernel_maps(&machines->host);

	while (next) {
		struct machine *pos = rb_entry(next, struct machine, rb_node);

		next = rb_next(&pos->rb_node);
		rb_erase_cached(&pos->rb_node, &machines->guests);
		machine__delete(pos);
	}
}

int machines__create_kernel_maps(struct machines *machines, pid_t pid)
{
	struct machine *machine = machines__findnew(machines, pid);

	if (machine == NULL)
		return -1;

	return machine__create_kernel_maps(machine);
}

int machine__load_kallsyms(struct machine *machine, const char *filename)
{
	struct map *map = machine__kernel_map(machine);
	int ret = __dso__load_kallsyms(map->dso, filename, map, true);

	if (ret > 0) {
		dso__set_loaded(map->dso);
		/*
		 * Since /proc/kallsyms will have multiple sessions for the
		 * kernel, with modules between them, fixup the end of all
		 * sections.
		 */
		map_groups__fixup_end(&machine->kmaps);
	}

	return ret;
}

int machine__load_vmlinux_path(struct machine *machine)
{
	struct map *map = machine__kernel_map(machine);
	int ret = dso__load_vmlinux_path(map->dso, map);

	if (ret > 0)
		dso__set_loaded(map->dso);

	return ret;
}

static char *get_kernel_version(const char *root_dir)
{
	char version[PATH_MAX];
	FILE *file;
	char *name, *tmp;
	const char *prefix = "Linux version ";

	sprintf(version, "%s/proc/version", root_dir);
	file = fopen(version, "r");
	if (!file)
		return NULL;

	version[0] = '\0';
	tmp = fgets(version, sizeof(version), file);
	fclose(file);

	name = strstr(version, prefix);
	if (!name)
		return NULL;
	name += strlen(prefix);
	tmp = strchr(name, ' ');
	if (tmp)
		*tmp = '\0';

	return strdup(name);
}

static bool is_kmod_dso(struct dso *dso)
{
	return dso->symtab_type == DSO_BINARY_TYPE__SYSTEM_PATH_KMODULE ||
	       dso->symtab_type == DSO_BINARY_TYPE__GUEST_KMODULE;
}

static int map_groups__set_module_path(struct map_groups *mg, const char *path,
				       struct kmod_path *m)
{
	char *long_name;
	struct map *map = map_groups__find_by_name(mg, m->name);

	if (map == NULL)
		return 0;

	long_name = strdup(path);
	if (long_name == NULL)
		return -ENOMEM;

	dso__set_long_name(map->dso, long_name, true);
	dso__kernel_module_get_build_id(map->dso, "");

	/*
	 * Full name could reveal us kmod compression, so
	 * we need to update the symtab_type if needed.
	 */
	if (m->comp && is_kmod_dso(map->dso)) {
		map->dso->symtab_type++;
		map->dso->comp = m->comp;
	}

	return 0;
}

static int map_groups__set_modules_path_dir(struct map_groups *mg,
				const char *dir_name, int depth)
{
	struct dirent *dent;
	DIR *dir = opendir(dir_name);
	int ret = 0;

	if (!dir) {
		pr_debug("%s: cannot open %s dir\n", __func__, dir_name);
		return -1;
	}

	while ((dent = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		struct stat st;

		/*sshfs might return bad dent->d_type, so we have to stat*/
		snprintf(path, sizeof(path), "%s/%s", dir_name, dent->d_name);
		if (stat(path, &st))
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (!strcmp(dent->d_name, ".") ||
			    !strcmp(dent->d_name, ".."))
				continue;

			/* Do not follow top-level source and build symlinks */
			if (depth == 0) {
				if (!strcmp(dent->d_name, "source") ||
				    !strcmp(dent->d_name, "build"))
					continue;
			}

			ret = map_groups__set_modules_path_dir(mg, path,
							       depth + 1);
			if (ret < 0)
				goto out;
		} else {
			struct kmod_path m;

			ret = kmod_path__parse_name(&m, dent->d_name);
			if (ret)
				goto out;

			if (m.kmod)
				ret = map_groups__set_module_path(mg, path, &m);

			free(m.name);

			if (ret)
				goto out;
		}
	}

out:
	closedir(dir);
	return ret;
}

static int machine__set_modules_path(struct machine *machine)
{
	char *version;
	char modules_path[PATH_MAX];

	version = get_kernel_version(machine->root_dir);
	if (!version)
		return -1;

	snprintf(modules_path, sizeof(modules_path), "%s/lib/modules/%s",
		 machine->root_dir, version);
	free(version);

	return map_groups__set_modules_path_dir(&machine->kmaps, modules_path, 0);
}
int __weak arch__fix_module_text_start(u64 *start __maybe_unused,
				const char *name __maybe_unused)
{
	return 0;
}

static int machine__create_module(void *arg, const char *name, u64 start,
				  u64 size)
{
	struct machine *machine = arg;
	struct map *map;

	if (arch__fix_module_text_start(&start, name) < 0)
		return -1;

	map = machine__findnew_module_map(machine, start, name);
	if (map == NULL)
		return -1;
	map->end = start + size;

	dso__kernel_module_get_build_id(map->dso, machine->root_dir);

	return 0;
}

static int machine__create_modules(struct machine *machine)
{
	const char *modules;
	char path[PATH_MAX];

	if (machine__is_default_guest(machine)) {
		modules = symbol_conf.default_guest_modules;
	} else {
		snprintf(path, PATH_MAX, "%s/proc/modules", machine->root_dir);
		modules = path;
	}

	if (symbol__restricted_filename(modules, "/proc/modules"))
		return -1;

	if (modules__parse(modules, machine, machine__create_module))
		return -1;

	if (!machine__set_modules_path(machine))
		return 0;

	pr_debug("Problems setting modules path maps, continuing anyway...\n");

	return 0;
}

static void machine__set_kernel_mmap(struct machine *machine,
				     u64 start, u64 end)
{
	machine->vmlinux_map->start = start;
	machine->vmlinux_map->end   = end;
	/*
	 * Be a bit paranoid here, some perf.data file came with
	 * a zero sized synthesized MMAP event for the kernel.
	 */
	if (start == 0 && end == 0)
		machine->vmlinux_map->end = ~0ULL;
}

int machine__create_kernel_maps(struct machine *machine)
{
	struct dso *kernel = machine__get_kernel(machine);
	const char *name = NULL;
	struct map *map;
	u64 addr = 0;
	int ret;

	if (kernel == NULL)
		return -1;

	ret = __machine__create_kernel_maps(machine, kernel);
	if (ret < 0)
		goto out_put;

	if (symbol_conf.use_modules && machine__create_modules(machine) < 0) {
		if (machine__is_host(machine))
			pr_debug("Problems creating module maps, "
				 "continuing anyway...\n");
		else
			pr_debug("Problems creating module maps for guest %d, "
				 "continuing anyway...\n", machine->pid);
	}

	if (!machine__get_running_kernel_start(machine, &name, &addr)) {
		if (name &&
		    map__set_kallsyms_ref_reloc_sym(machine->vmlinux_map, name, addr)) {
			machine__destroy_kernel_maps(machine);
			ret = -1;
			goto out_put;
		}

		/* we have a real start address now, so re-order the kmaps */
		map = machine__kernel_map(machine);

		map__get(map);
		map_groups__remove(&machine->kmaps, map);

		/* assume it's the last in the kmaps */
		machine__set_kernel_mmap(machine, addr, ~0ULL);

		map_groups__insert(&machine->kmaps, map);
		map__put(map);
	}

	if (machine__create_extra_kernel_maps(machine, kernel))
		pr_debug("Problems creating extra kernel maps, continuing anyway...\n");

	/* update end address of the kernel map using adjacent module address */
	map = map__next(machine__kernel_map(machine));
	if (map)
		machine__set_kernel_mmap(machine, addr, map->start);
out_put:
	dso__put(kernel);
	return ret;
}

static bool machine__uses_kcore(struct machine *machine)
{
	struct dso *dso;

	list_for_each_entry(dso, &machine->dsos.head, node) {
		if (dso__is_kcore(dso))
			return true;
	}

	return false;
}

static bool perf_event__is_extra_kernel_mmap(struct machine *machine,
					     union perf_event *event)
{
	return machine__is(machine, "x86_64") &&
	       is_entry_trampoline(event->mmap.filename);
}

static int machine__process_extra_kernel_map(struct machine *machine,
					     union perf_event *event)
{
	struct map *kernel_map = machine__kernel_map(machine);
	struct dso *kernel = kernel_map ? kernel_map->dso : NULL;
	struct extra_kernel_map xm = {
		.start = event->mmap.start,
		.end   = event->mmap.start + event->mmap.len,
		.pgoff = event->mmap.pgoff,
	};

	if (kernel == NULL)
		return -1;

	strlcpy(xm.name, event->mmap.filename, KMAP_NAME_LEN);

	return machine__create_extra_kernel_map(machine, kernel, &xm);
}

static int machine__process_kernel_mmap_event(struct machine *machine,
					      union perf_event *event)
{
	struct map *map;
	enum dso_kernel_type kernel_type;
	bool is_kernel_mmap;

	/* If we have maps from kcore then we do not need or want any others */
	if (machine__uses_kcore(machine))
		return 0;

	if (machine__is_host(machine))
		kernel_type = DSO_TYPE_KERNEL;
	else
		kernel_type = DSO_TYPE_GUEST_KERNEL;

	is_kernel_mmap = memcmp(event->mmap.filename,
				machine->mmap_name,
				strlen(machine->mmap_name) - 1) == 0;
	if (event->mmap.filename[0] == '/' ||
	    (!is_kernel_mmap && event->mmap.filename[0] == '[')) {
		map = machine__findnew_module_map(machine, event->mmap.start,
						  event->mmap.filename);
		if (map == NULL)
			goto out_problem;

		map->end = map->start + event->mmap.len;
	} else if (is_kernel_mmap) {
		const char *symbol_name = (event->mmap.filename +
				strlen(machine->mmap_name));
		/*
		 * Should be there already, from the build-id table in
		 * the header.
		 */
		struct dso *kernel = NULL;
		struct dso *dso;

		down_read(&machine->dsos.lock);

		list_for_each_entry(dso, &machine->dsos.head, node) {

			/*
			 * The cpumode passed to is_kernel_module is not the
			 * cpumode of *this* event. If we insist on passing
			 * correct cpumode to is_kernel_module, we should
			 * record the cpumode when we adding this dso to the
			 * linked list.
			 *
			 * However we don't really need passing correct
			 * cpumode.  We know the correct cpumode must be kernel
			 * mode (if not, we should not link it onto kernel_dsos
			 * list).
			 *
			 * Therefore, we pass PERF_RECORD_MISC_CPUMODE_UNKNOWN.
			 * is_kernel_module() treats it as a kernel cpumode.
			 */

			if (!dso->kernel ||
			    is_kernel_module(dso->long_name,
					     PERF_RECORD_MISC_CPUMODE_UNKNOWN))
				continue;


			kernel = dso;
			break;
		}

		up_read(&machine->dsos.lock);

		if (kernel == NULL)
			kernel = machine__findnew_dso(machine, machine->mmap_name);
		if (kernel == NULL)
			goto out_problem;

		kernel->kernel = kernel_type;
		if (__machine__create_kernel_maps(machine, kernel) < 0) {
			dso__put(kernel);
			goto out_problem;
		}

		if (strstr(kernel->long_name, "vmlinux"))
			dso__set_short_name(kernel, "[kernel.vmlinux]", false);

		machine__set_kernel_mmap(machine, event->mmap.start,
					 event->mmap.start + event->mmap.len);

		/*
		 * Avoid using a zero address (kptr_restrict) for the ref reloc
		 * symbol. Effectively having zero here means that at record
		 * time /proc/sys/kernel/kptr_restrict was non zero.
		 */
		if (event->mmap.pgoff != 0) {
			map__set_kallsyms_ref_reloc_sym(machine->vmlinux_map,
							symbol_name,
							event->mmap.pgoff);
		}

		if (machine__is_default_guest(machine)) {
			/*
			 * preload dso of guest kernel and modules
			 */
			dso__load(kernel, machine__kernel_map(machine));
		}
	} else if (perf_event__is_extra_kernel_mmap(machine, event)) {
		return machine__process_extra_kernel_map(machine, event);
	}
	return 0;
out_problem:
	return -1;
}

int machine__process_mmap2_event(struct machine *machine,
				 union perf_event *event,
				 struct perf_sample *sample)
{
	struct thread *thread;
	struct map *map;
	int ret = 0;

	if (dump_trace)
		perf_event__fprintf_mmap2(event, stdout);

	if (sample->cpumode == PERF_RECORD_MISC_GUEST_KERNEL ||
	    sample->cpumode == PERF_RECORD_MISC_KERNEL) {
		ret = machine__process_kernel_mmap_event(machine, event);
		if (ret < 0)
			goto out_problem;
		return 0;
	}

	thread = machine__findnew_thread(machine, event->mmap2.pid,
					event->mmap2.tid);
	if (thread == NULL)
		goto out_problem;

	map = map__new(machine, event->mmap2.start,
			event->mmap2.len, event->mmap2.pgoff,
			event->mmap2.maj,
			event->mmap2.min, event->mmap2.ino,
			event->mmap2.ino_generation,
			event->mmap2.prot,
			event->mmap2.flags,
			event->mmap2.filename, thread);

	if (map == NULL)
		goto out_problem_map;

	ret = thread__insert_map(thread, map);
	if (ret)
		goto out_problem_insert;

	thread__put(thread);
	map__put(map);
	return 0;

out_problem_insert:
	map__put(map);
out_problem_map:
	thread__put(thread);
out_problem:
	dump_printf("problem processing PERF_RECORD_MMAP2, skipping event.\n");
	return 0;
}

int machine__process_mmap_event(struct machine *machine, union perf_event *event,
				struct perf_sample *sample)
{
	struct thread *thread;
	struct map *map;
	u32 prot = 0;
	int ret = 0;

	if (dump_trace)
		perf_event__fprintf_mmap(event, stdout);

	if (sample->cpumode == PERF_RECORD_MISC_GUEST_KERNEL ||
	    sample->cpumode == PERF_RECORD_MISC_KERNEL) {
		ret = machine__process_kernel_mmap_event(machine, event);
		if (ret < 0)
			goto out_problem;
		return 0;
	}

	thread = machine__findnew_thread(machine, event->mmap.pid,
					 event->mmap.tid);
	if (thread == NULL)
		goto out_problem;

	if (!(event->header.misc & PERF_RECORD_MISC_MMAP_DATA))
		prot = PROT_EXEC;

	map = map__new(machine, event->mmap.start,
			event->mmap.len, event->mmap.pgoff,
			0, 0, 0, 0, prot, 0,
			event->mmap.filename,
			thread);

	if (map == NULL)
		goto out_problem_map;

	ret = thread__insert_map(thread, map);
	if (ret)
		goto out_problem_insert;

	thread__put(thread);
	map__put(map);
	return 0;

out_problem_insert:
	map__put(map);
out_problem_map:
	thread__put(thread);
out_problem:
	dump_printf("problem processing PERF_RECORD_MMAP, skipping event.\n");
	return 0;
}

static void __machine__remove_thread(struct machine *machine, struct thread *th, bool lock)
{
	struct threads *threads = machine__threads(machine, th->tid);

	if (threads->last_match == th)
		threads__set_last_match(threads, NULL);

	BUG_ON(refcount_read(&th->refcnt) == 0);
	if (lock)
		down_write(&threads->lock);
	rb_erase_cached(&th->rb_node, &threads->entries);
	RB_CLEAR_NODE(&th->rb_node);
	--threads->nr;
	/*
	 * Move it first to the dead_threads list, then drop the reference,
	 * if this is the last reference, then the thread__delete destructor
	 * will be called and we will remove it from the dead_threads list.
	 */
	list_add_tail(&th->node, &threads->dead);
	if (lock)
		up_write(&threads->lock);
	thread__put(th);
}

void machine__remove_thread(struct machine *machine, struct thread *th)
{
	return __machine__remove_thread(machine, th, true);
}

int machine__process_fork_event(struct machine *machine, union perf_event *event,
				struct perf_sample *sample)
{
	struct thread *thread = machine__find_thread(machine,
						     event->fork.pid,
						     event->fork.tid);
	struct thread *parent = machine__findnew_thread(machine,
							event->fork.ppid,
							event->fork.ptid);
	bool do_maps_clone = true;
	int err = 0;

	if (dump_trace)
		perf_event__fprintf_task(event, stdout);

	/*
	 * There may be an existing thread that is not actually the parent,
	 * either because we are processing events out of order, or because the
	 * (fork) event that would have removed the thread was lost. Assume the
	 * latter case and continue on as best we can.
	 */
	if (parent->pid_ != (pid_t)event->fork.ppid) {
		dump_printf("removing erroneous parent thread %d/%d\n",
			    parent->pid_, parent->tid);
		machine__remove_thread(machine, parent);
		thread__put(parent);
		parent = machine__findnew_thread(machine, event->fork.ppid,
						 event->fork.ptid);
	}

	/* if a thread currently exists for the thread id remove it */
	if (thread != NULL) {
		machine__remove_thread(machine, thread);
		thread__put(thread);
	}

	thread = machine__findnew_thread(machine, event->fork.pid,
					 event->fork.tid);
	/*
	 * When synthesizing FORK events, we are trying to create thread
	 * objects for the already running tasks on the machine.
	 *
	 * Normally, for a kernel FORK event, we want to clone the parent's
	 * maps because that is what the kernel just did.
	 *
	 * But when synthesizing, this should not be done.  If we do, we end up
	 * with overlapping maps as we process the sythesized MMAP2 events that
	 * get delivered shortly thereafter.
	 *
	 * Use the FORK event misc flags in an internal way to signal this
	 * situation, so we can elide the map clone when appropriate.
	 */
	if (event->fork.header.misc & PERF_RECORD_MISC_FORK_EXEC)
		do_maps_clone = false;

	if (thread == NULL || parent == NULL ||
	    thread__fork(thread, parent, sample->time, do_maps_clone) < 0) {
		dump_printf("problem processing PERF_RECORD_FORK, skipping event.\n");
		err = -1;
	}
	thread__put(thread);
	thread__put(parent);

	return err;
}

int machine__process_exit_event(struct machine *machine, union perf_event *event,
				struct perf_sample *sample __maybe_unused)
{
	struct thread *thread = machine__find_thread(machine,
						     event->fork.pid,
						     event->fork.tid);

	if (dump_trace)
		perf_event__fprintf_task(event, stdout);

	if (thread != NULL) {
		thread__exited(thread);
		thread__put(thread);
	}

	return 0;
}

int machine__process_event(struct machine *machine, union perf_event *event,
			   struct perf_sample *sample)
{
	int ret;

	switch (event->header.type) {
	case PERF_RECORD_COMM:
		ret = machine__process_comm_event(machine, event, sample); break;
	case PERF_RECORD_MMAP:
		ret = machine__process_mmap_event(machine, event, sample); break;
	case PERF_RECORD_NAMESPACES:
		ret = machine__process_namespaces_event(machine, event, sample); break;
	case PERF_RECORD_MMAP2:
		ret = machine__process_mmap2_event(machine, event, sample); break;
	case PERF_RECORD_FORK:
		ret = machine__process_fork_event(machine, event, sample); break;
	case PERF_RECORD_EXIT:
		ret = machine__process_exit_event(machine, event, sample); break;
	case PERF_RECORD_LOST:
		ret = machine__process_lost_event(machine, event, sample); break;
	case PERF_RECORD_AUX:
		ret = machine__process_aux_event(machine, event); break;
	case PERF_RECORD_ITRACE_START:
		ret = machine__process_itrace_start_event(machine, event); break;
	case PERF_RECORD_LOST_SAMPLES:
		ret = machine__process_lost_samples_event(machine, event, sample); break;
	case PERF_RECORD_SWITCH:
	case PERF_RECORD_SWITCH_CPU_WIDE:
		ret = machine__process_switch_event(machine, event); break;
	case PERF_RECORD_KSYMBOL:
		ret = machine__process_ksymbol(machine, event, sample); break;
	case PERF_RECORD_BPF_EVENT:
		ret = machine__process_bpf_event(machine, event, sample); break;
	default:
		ret = -1;
		break;
	}

	return ret;
}

static bool symbol__match_regex(struct symbol *sym, regex_t *regex)
{
	if (!regexec(regex, sym->name, 0, NULL, 0))
		return 1;
	return 0;
}

static void ip__resolve_ams(struct thread *thread,
			    struct addr_map_symbol *ams,
			    u64 ip)
{
	struct addr_location al;

	memset(&al, 0, sizeof(al));
	/*
	 * We cannot use the header.misc hint to determine whether a
	 * branch stack address is user, kernel, guest, hypervisor.
	 * Branches may straddle the kernel/user/hypervisor boundaries.
	 * Thus, we have to try consecutively until we find a match
	 * or else, the symbol is unknown
	 */
	thread__find_cpumode_addr_location(thread, ip, &al);

	ams->addr = ip;
	ams->al_addr = al.addr;
	ams->sym = al.sym;
	ams->map = al.map;
	ams->phys_addr = 0;
}

static void ip__resolve_data(struct thread *thread,
			     u8 m, struct addr_map_symbol *ams,
			     u64 addr, u64 phys_addr)
{
	struct addr_location al;

	memset(&al, 0, sizeof(al));

	thread__find_symbol(thread, m, addr, &al);

	ams->addr = addr;
	ams->al_addr = al.addr;
	ams->sym = al.sym;
	ams->map = al.map;
	ams->phys_addr = phys_addr;
}

struct mem_info *sample__resolve_mem(struct perf_sample *sample,
				     struct addr_location *al)
{
	struct mem_info *mi = mem_info__new();

	if (!mi)
		return NULL;

	ip__resolve_ams(al->thread, &mi->iaddr, sample->ip);
	ip__resolve_data(al->thread, al->cpumode, &mi->daddr,
			 sample->addr, sample->phys_addr);
	mi->data_src.val = sample->data_src;

	return mi;
}

static char *callchain_srcline(struct map *map, struct symbol *sym, u64 ip)
{
	char *srcline = NULL;

	if (!map || callchain_param.key == CCKEY_FUNCTION)
		return srcline;

	srcline = srcline__tree_find(&map->dso->srclines, ip);
	if (!srcline) {
		bool show_sym = false;
		bool show_addr = callchain_param.key == CCKEY_ADDRESS;

		srcline = get_srcline(map->dso, map__rip_2objdump(map, ip),
				      sym, show_sym, show_addr, ip);
		srcline__tree_insert(&map->dso->srclines, ip, srcline);
	}

	return srcline;
}

struct iterations {
	int nr_loop_iter;
	u64 cycles;
};

static int add_callchain_ip(struct thread *thread,
			    struct callchain_cursor *cursor,
			    struct symbol **parent,
			    struct addr_location *root_al,
			    u8 *cpumode,
			    u64 ip,
			    bool branch,
			    struct branch_flags *flags,
			    struct iterations *iter,
			    u64 branch_from)
{
	struct addr_location al;
	int nr_loop_iter = 0;
	u64 iter_cycles = 0;
	const char *srcline = NULL;

	al.filtered = 0;
	al.sym = NULL;
	if (!cpumode) {
		thread__find_cpumode_addr_location(thread, ip, &al);
	} else {
		if (ip >= PERF_CONTEXT_MAX) {
			switch (ip) {
			case PERF_CONTEXT_HV:
				*cpumode = PERF_RECORD_MISC_HYPERVISOR;
				break;
			case PERF_CONTEXT_KERNEL:
				*cpumode = PERF_RECORD_MISC_KERNEL;
				break;
			case PERF_CONTEXT_USER:
				*cpumode = PERF_RECORD_MISC_USER;
				break;
			default:
				pr_debug("invalid callchain context: "
					 "%"PRId64"\n", (s64) ip);
				/*
				 * It seems the callchain is corrupted.
				 * Discard all.
				 */
				callchain_cursor_reset(cursor);
				return 1;
			}
			return 0;
		}
		thread__find_symbol(thread, *cpumode, ip, &al);
	}

	if (al.sym != NULL) {
		if (perf_hpp_list.parent && !*parent &&
		    symbol__match_regex(al.sym, &parent_regex))
			*parent = al.sym;
		else if (have_ignore_callees && root_al &&
		  symbol__match_regex(al.sym, &ignore_callees_regex)) {
			/* Treat this symbol as the root,
			   forgetting its callees. */
			*root_al = al;
			callchain_cursor_reset(cursor);
		}
	}

	if (symbol_conf.hide_unresolved && al.sym == NULL)
		return 0;

	if (iter) {
		nr_loop_iter = iter->nr_loop_iter;
		iter_cycles = iter->cycles;
	}

	srcline = callchain_srcline(al.map, al.sym, al.addr);
	return callchain_cursor_append(cursor, ip, al.map, al.sym,
				       branch, flags, nr_loop_iter,
				       iter_cycles, branch_from, srcline);
}

struct branch_info *sample__resolve_bstack(struct perf_sample *sample,
					   struct addr_location *al)
{
	unsigned int i;
	const struct branch_stack *bs = sample->branch_stack;
	struct branch_info *bi = calloc(bs->nr, sizeof(struct branch_info));

	if (!bi)
		return NULL;

	for (i = 0; i < bs->nr; i++) {
		ip__resolve_ams(al->thread, &bi[i].to, bs->entries[i].to);
		ip__resolve_ams(al->thread, &bi[i].from, bs->entries[i].from);
		bi[i].flags = bs->entries[i].flags;
	}
	return bi;
}

static void save_iterations(struct iterations *iter,
			    struct branch_entry *be, int nr)
{
	int i;

	iter->nr_loop_iter++;
	iter->cycles = 0;

	for (i = 0; i < nr; i++)
		iter->cycles += be[i].flags.cycles;
}

#define CHASHSZ 127
#define CHASHBITS 7
#define NO_ENTRY 0xff

#define PERF_MAX_BRANCH_DEPTH 127

/* Remove loops. */
static int remove_loops(struct branch_entry *l, int nr,
			struct iterations *iter)
{
	int i, j, off;
	unsigned char chash[CHASHSZ];

	memset(chash, NO_ENTRY, sizeof(chash));

	BUG_ON(PERF_MAX_BRANCH_DEPTH > 255);

	for (i = 0; i < nr; i++) {
		int h = hash_64(l[i].from, CHASHBITS) % CHASHSZ;

		/* no collision handling for now */
		if (chash[h] == NO_ENTRY) {
			chash[h] = i;
		} else if (l[chash[h]].from == l[i].from) {
			bool is_loop = true;
			/* check if it is a real loop */
			off = 0;
			for (j = chash[h]; j < i && i + off < nr; j++, off++)
				if (l[j].from != l[i + off].from) {
					is_loop = false;
					break;
				}
			if (is_loop) {
				j = nr - (i + off);
				if (j > 0) {
					save_iterations(iter + i + off,
						l + i, off);

					memmove(iter + i, iter + i + off,
						j * sizeof(*iter));

					memmove(l + i, l + i + off,
						j * sizeof(*l));
				}

				nr -= off;
			}
		}
	}
	return nr;
}

/*
 * Recolve LBR callstack chain sample
 * Return:
 * 1 on success get LBR callchain information
 * 0 no available LBR callchain information, should try fp
 * negative error code on other errors.
 */
static int resolve_lbr_callchain_sample(struct thread *thread,
					struct callchain_cursor *cursor,
					struct perf_sample *sample,
					struct symbol **parent,
					struct addr_location *root_al,
					int max_stack)
{
	struct ip_callchain *chain = sample->callchain;
	int chain_nr = min(max_stack, (int)chain->nr), i;
	u8 cpumode = PERF_RECORD_MISC_USER;
	u64 ip, branch_from = 0;

	for (i = 0; i < chain_nr; i++) {
		if (chain->ips[i] == PERF_CONTEXT_USER)
			break;
	}

	/* LBR only affects the user callchain */
	if (i != chain_nr) {
		struct branch_stack *lbr_stack = sample->branch_stack;
		int lbr_nr = lbr_stack->nr, j, k;
		bool branch;
		struct branch_flags *flags;
		/*
		 * LBR callstack can only get user call chain.
		 * The mix_chain_nr is kernel call chain
		 * number plus LBR user call chain number.
		 * i is kernel call chain number,
		 * 1 is PERF_CONTEXT_USER,
		 * lbr_nr + 1 is the user call chain number.
		 * For details, please refer to the comments
		 * in callchain__printf
		 */
		int mix_chain_nr = i + 1 + lbr_nr + 1;

		for (j = 0; j < mix_chain_nr; j++) {
			int err;
			branch = false;
			flags = NULL;

			if (callchain_param.order == ORDER_CALLEE) {
				if (j < i + 1)
					ip = chain->ips[j];
				else if (j > i + 1) {
					k = j - i - 2;
					ip = lbr_stack->entries[k].from;
					branch = true;
					flags = &lbr_stack->entries[k].flags;
				} else {
					ip = lbr_stack->entries[0].to;
					branch = true;
					flags = &lbr_stack->entries[0].flags;
					branch_from =
						lbr_stack->entries[0].from;
				}
			} else {
				if (j < lbr_nr) {
					k = lbr_nr - j - 1;
					ip = lbr_stack->entries[k].from;
					branch = true;
					flags = &lbr_stack->entries[k].flags;
				}
				else if (j > lbr_nr)
					ip = chain->ips[i + 1 - (j - lbr_nr)];
				else {
					ip = lbr_stack->entries[0].to;
					branch = true;
					flags = &lbr_stack->entries[0].flags;
					branch_from =
						lbr_stack->entries[0].from;
				}
			}

			err = add_callchain_ip(thread, cursor, parent,
					       root_al, &cpumode, ip,
					       branch, flags, NULL,
					       branch_from);
			if (err)
				return (err < 0) ? err : 0;
		}
		return 1;
	}

	return 0;
}

static int find_prev_cpumode(struct ip_callchain *chain, struct thread *thread,
			     struct callchain_cursor *cursor,
			     struct symbol **parent,
			     struct addr_location *root_al,
			     u8 *cpumode, int ent)
{
	int err = 0;

	while (--ent >= 0) {
		u64 ip = chain->ips[ent];

		if (ip >= PERF_CONTEXT_MAX) {
			err = add_callchain_ip(thread, cursor, parent,
					       root_al, cpumode, ip,
					       false, NULL, NULL, 0);
			break;
		}
	}
	return err;
}

static int thread__resolve_callchain_sample(struct thread *thread,
					    struct callchain_cursor *cursor,
					    struct perf_evsel *evsel,
					    struct perf_sample *sample,
					    struct symbol **parent,
					    struct addr_location *root_al,
					    int max_stack)
{
	struct branch_stack *branch = sample->branch_stack;
	struct ip_callchain *chain = sample->callchain;
	int chain_nr = 0;
	u8 cpumode = PERF_RECORD_MISC_USER;
	int i, j, err, nr_entries;
	int skip_idx = -1;
	int first_call = 0;

	if (chain)
		chain_nr = chain->nr;

	if (perf_evsel__has_branch_callstack(evsel)) {
		err = resolve_lbr_callchain_sample(thread, cursor, sample, parent,
						   root_al, max_stack);
		if (err)
			return (err < 0) ? err : 0;
	}

	/*
	 * Based on DWARF debug information, some architectures skip
	 * a callchain entry saved by the kernel.
	 */
	skip_idx = arch_skip_callchain_idx(thread, chain);

	/*
	 * Add branches to call stack for easier browsing. This gives
	 * more context for a sample than just the callers.
	 *
	 * This uses individual histograms of paths compared to the
	 * aggregated histograms the normal LBR mode uses.
	 *
	 * Limitations for now:
	 * - No extra filters
	 * - No annotations (should annotate somehow)
	 */

	if (branch && callchain_param.branch_callstack) {
		int nr = min(max_stack, (int)branch->nr);
		struct branch_entry be[nr];
		struct iterations iter[nr];

		if (branch->nr > PERF_MAX_BRANCH_DEPTH) {
			pr_warning("corrupted branch chain. skipping...\n");
			goto check_calls;
		}

		for (i = 0; i < nr; i++) {
			if (callchain_param.order == ORDER_CALLEE) {
				be[i] = branch->entries[i];

				if (chain == NULL)
					continue;

				/*
				 * Check for overlap into the callchain.
				 * The return address is one off compared to
				 * the branch entry. To adjust for this
				 * assume the calling instruction is not longer
				 * than 8 bytes.
				 */
				if (i == skip_idx ||
				    chain->ips[first_call] >= PERF_CONTEXT_MAX)
					first_call++;
				else if (be[i].from < chain->ips[first_call] &&
				    be[i].from >= chain->ips[first_call] - 8)
					first_call++;
			} else
				be[i] = branch->entries[branch->nr - i - 1];
		}

		memset(iter, 0, sizeof(struct iterations) * nr);
		nr = remove_loops(be, nr, iter);

		for (i = 0; i < nr; i++) {
			err = add_callchain_ip(thread, cursor, parent,
					       root_al,
					       NULL, be[i].to,
					       true, &be[i].flags,
					       NULL, be[i].from);

			if (!err)
				err = add_callchain_ip(thread, cursor, parent, root_al,
						       NULL, be[i].from,
						       true, &be[i].flags,
						       &iter[i], 0);
			if (err == -EINVAL)
				break;
			if (err)
				return err;
		}

		if (chain_nr == 0)
			return 0;

		chain_nr -= nr;
	}

check_calls:
	if (callchain_param.order != ORDER_CALLEE) {
		err = find_prev_cpumode(chain, thread, cursor, parent, root_al,
					&cpumode, chain->nr - first_call);
		if (err)
			return (err < 0) ? err : 0;
	}
	for (i = first_call, nr_entries = 0;
	     i < chain_nr && nr_entries < max_stack; i++) {
		u64 ip;

		if (callchain_param.order == ORDER_CALLEE)
			j = i;
		else
			j = chain->nr - i - 1;

#ifdef HAVE_SKIP_CALLCHAIN_IDX
		if (j == skip_idx)
			continue;
#endif
		ip = chain->ips[j];
		if (ip < PERF_CONTEXT_MAX)
                       ++nr_entries;
		else if (callchain_param.order != ORDER_CALLEE) {
			err = find_prev_cpumode(chain, thread, cursor, parent,
						root_al, &cpumode, j);
			if (err)
				return (err < 0) ? err : 0;
			continue;
		}

		err = add_callchain_ip(thread, cursor, parent,
				       root_al, &cpumode, ip,
				       false, NULL, NULL, 0);

		if (err)
			return (err < 0) ? err : 0;
	}

	return 0;
}

static int append_inlines(struct callchain_cursor *cursor,
			  struct map *map, struct symbol *sym, u64 ip)
{
	struct inline_node *inline_node;
	struct inline_list *ilist;
	u64 addr;
	int ret = 1;

	if (!symbol_conf.inline_name || !map || !sym)
		return ret;

	addr = map__map_ip(map, ip);
	addr = map__rip_2objdump(map, addr);

	inline_node = inlines__tree_find(&map->dso->inlined_nodes, addr);
	if (!inline_node) {
		inline_node = dso__parse_addr_inlines(map->dso, addr, sym);
		if (!inline_node)
			return ret;
		inlines__tree_insert(&map->dso->inlined_nodes, inline_node);
	}

	list_for_each_entry(ilist, &inline_node->val, list) {
		ret = callchain_cursor_append(cursor, ip, map,
					      ilist->symbol, false,
					      NULL, 0, 0, 0, ilist->srcline);

		if (ret != 0)
			return ret;
	}

	return ret;
}

static int unwind_entry(struct unwind_entry *entry, void *arg)
{
	struct callchain_cursor *cursor = arg;
	const char *srcline = NULL;
	u64 addr = entry->ip;

	if (symbol_conf.hide_unresolved && entry->sym == NULL)
		return 0;

	if (append_inlines(cursor, entry->map, entry->sym, entry->ip) == 0)
		return 0;

	/*
	 * Convert entry->ip from a virtual address to an offset in
	 * its corresponding binary.
	 */
	if (entry->map)
		addr = map__map_ip(entry->map, entry->ip);

	srcline = callchain_srcline(entry->map, entry->sym, addr);
	return callchain_cursor_append(cursor, entry->ip,
				       entry->map, entry->sym,
				       false, NULL, 0, 0, 0, srcline);
}

static int thread__resolve_callchain_unwind(struct thread *thread,
					    struct callchain_cursor *cursor,
					    struct perf_evsel *evsel,
					    struct perf_sample *sample,
					    int max_stack)
{
	/* Can we do dwarf post unwind? */
	if (!((evsel->attr.sample_type & PERF_SAMPLE_REGS_USER) &&
	      (evsel->attr.sample_type & PERF_SAMPLE_STACK_USER)))
		return 0;

	/* Bail out if nothing was captured. */
	if ((!sample->user_regs.regs) ||
	    (!sample->user_stack.size))
		return 0;

	return unwind__get_entries(unwind_entry, cursor,
				   thread, sample, max_stack);
}

int thread__resolve_callchain(struct thread *thread,
			      struct callchain_cursor *cursor,
			      struct perf_evsel *evsel,
			      struct perf_sample *sample,
			      struct symbol **parent,
			      struct addr_location *root_al,
			      int max_stack)
{
	int ret = 0;

	callchain_cursor_reset(cursor);

	if (callchain_param.order == ORDER_CALLEE) {
		ret = thread__resolve_callchain_sample(thread, cursor,
						       evsel, sample,
						       parent, root_al,
						       max_stack);
		if (ret)
			return ret;
		ret = thread__resolve_callchain_unwind(thread, cursor,
						       evsel, sample,
						       max_stack);
	} else {
		ret = thread__resolve_callchain_unwind(thread, cursor,
						       evsel, sample,
						       max_stack);
		if (ret)
			return ret;
		ret = thread__resolve_callchain_sample(thread, cursor,
						       evsel, sample,
						       parent, root_al,
						       max_stack);
	}

	return ret;
}

int machine__for_each_thread(struct machine *machine,
			     int (*fn)(struct thread *thread, void *p),
			     void *priv)
{
	struct threads *threads;
	struct rb_node *nd;
	struct thread *thread;
	int rc = 0;
	int i;

	for (i = 0; i < THREADS__TABLE_SIZE; i++) {
		threads = &machine->threads[i];
		for (nd = rb_first_cached(&threads->entries); nd;
		     nd = rb_next(nd)) {
			thread = rb_entry(nd, struct thread, rb_node);
			rc = fn(thread, priv);
			if (rc != 0)
				return rc;
		}

		list_for_each_entry(thread, &threads->dead, node) {
			rc = fn(thread, priv);
			if (rc != 0)
				return rc;
		}
	}
	return rc;
}

int machines__for_each_thread(struct machines *machines,
			      int (*fn)(struct thread *thread, void *p),
			      void *priv)
{
	struct rb_node *nd;
	int rc = 0;

	rc = machine__for_each_thread(&machines->host, fn, priv);
	if (rc != 0)
		return rc;

	for (nd = rb_first_cached(&machines->guests); nd; nd = rb_next(nd)) {
		struct machine *machine = rb_entry(nd, struct machine, rb_node);

		rc = machine__for_each_thread(machine, fn, priv);
		if (rc != 0)
			return rc;
	}
	return rc;
}

int __machine__synthesize_threads(struct machine *machine, struct perf_tool *tool,
				  struct target *target, struct thread_map *threads,
				  perf_event__handler_t process, bool data_mmap,
				  unsigned int nr_threads_synthesize)
{
	if (target__has_task(target))
		return perf_event__synthesize_thread_map(tool, threads, process, machine, data_mmap);
	else if (target__has_cpu(target))
		return perf_event__synthesize_threads(tool, process,
						      machine, data_mmap,
						      nr_threads_synthesize);
	/* command specified */
	return 0;
}

pid_t machine__get_current_tid(struct machine *machine, int cpu)
{
	if (cpu < 0 || cpu >= MAX_NR_CPUS || !machine->current_tid)
		return -1;

	return machine->current_tid[cpu];
}

int machine__set_current_tid(struct machine *machine, int cpu, pid_t pid,
			     pid_t tid)
{
	struct thread *thread;

	if (cpu < 0)
		return -EINVAL;

	if (!machine->current_tid) {
		int i;

		machine->current_tid = calloc(MAX_NR_CPUS, sizeof(pid_t));
		if (!machine->current_tid)
			return -ENOMEM;
		for (i = 0; i < MAX_NR_CPUS; i++)
			machine->current_tid[i] = -1;
	}

	if (cpu >= MAX_NR_CPUS) {
		pr_err("Requested CPU %d too large. ", cpu);
		pr_err("Consider raising MAX_NR_CPUS\n");
		return -EINVAL;
	}

	machine->current_tid[cpu] = tid;

	thread = machine__findnew_thread(machine, pid, tid);
	if (!thread)
		return -ENOMEM;

	thread->cpu = cpu;
	thread__put(thread);

	return 0;
}

/*
 * Compares the raw arch string. N.B. see instead perf_env__arch() if a
 * normalized arch is needed.
 */
bool machine__is(struct machine *machine, const char *arch)
{
	return machine && !strcmp(perf_env__raw_arch(machine->env), arch);
}

int machine__nr_cpus_avail(struct machine *machine)
{
	return machine ? perf_env__nr_cpus_avail(machine->env) : 0;
}

int machine__get_kernel_start(struct machine *machine)
{
	struct map *map = machine__kernel_map(machine);
	int err = 0;

	/*
	 * The only addresses above 2^63 are kernel addresses of a 64-bit
	 * kernel.  Note that addresses are unsigned so that on a 32-bit system
	 * all addresses including kernel addresses are less than 2^32.  In
	 * that case (32-bit system), if the kernel mapping is unknown, all
	 * addresses will be assumed to be in user space - see
	 * machine__kernel_ip().
	 */
	machine->kernel_start = 1ULL << 63;
	if (map) {
		err = map__load(map);
		/*
		 * On x86_64, PTI entry trampolines are less than the
		 * start of kernel text, but still above 2^63. So leave
		 * kernel_start = 1ULL << 63 for x86_64.
		 */
		if (!err && !machine__is(machine, "x86_64"))
			machine->kernel_start = map->start;
	}
	return err;
}

u8 machine__addr_cpumode(struct machine *machine, u8 cpumode, u64 addr)
{
	u8 addr_cpumode = cpumode;
	bool kernel_ip;

	if (!machine->single_address_space)
		goto out;

	kernel_ip = machine__kernel_ip(machine, addr);
	switch (cpumode) {
	case PERF_RECORD_MISC_KERNEL:
	case PERF_RECORD_MISC_USER:
		addr_cpumode = kernel_ip ? PERF_RECORD_MISC_KERNEL :
					   PERF_RECORD_MISC_USER;
		break;
	case PERF_RECORD_MISC_GUEST_KERNEL:
	case PERF_RECORD_MISC_GUEST_USER:
		addr_cpumode = kernel_ip ? PERF_RECORD_MISC_GUEST_KERNEL :
					   PERF_RECORD_MISC_GUEST_USER;
		break;
	default:
		break;
	}
out:
	return addr_cpumode;
}

struct dso *machine__findnew_dso(struct machine *machine, const char *filename)
{
	return dsos__findnew(&machine->dsos, filename);
}

char *machine__resolve_kernel_addr(void *vmachine, unsigned long long *addrp, char **modp)
{
	struct machine *machine = vmachine;
	struct map *map;
	struct symbol *sym = machine__find_kernel_symbol(machine, *addrp, &map);

	if (sym == NULL)
		return NULL;

	*modp = __map__is_kmodule(map) ? (char *)map->dso->short_name : NULL;
	*addrp = map->unmap_ip(map, sym->start);
	return sym->name;
}
