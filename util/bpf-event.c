// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <stdlib.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>
#include <linux/btf.h>
#include <linux/err.h>
#include "bpf-event.h"
#include "debug.h"
#include "symbol.h"
#include "machine.h"
#include "env.h"
#include "session.h"
#include "map.h"

#define ptr_to_u64(ptr)    ((__u64)(unsigned long)(ptr))

static int snprintf_hex(char *buf, size_t size, unsigned char *data, size_t len)
{
	int ret = 0;
	size_t i;

	for (i = 0; i < len; i++)
		ret += snprintf(buf + ret, size - ret, "%02x", data[i]);
	return ret;
}

static int machine__process_bpf_event_load(struct machine *machine,
					   union perf_event *event,
					   struct perf_sample *sample __maybe_unused)
{
	struct bpf_prog_info_linear *info_linear;
	struct bpf_prog_info_node *info_node;
	struct perf_env *env = machine->env;
	int id = event->bpf_event.id;
	unsigned int i;

	/* perf-record, no need to handle bpf-event */
	if (env == NULL)
		return 0;

	info_node = perf_env__find_bpf_prog_info(env, id);
	if (!info_node)
		return 0;
	info_linear = info_node->info_linear;

	for (i = 0; i < info_linear->info.nr_jited_ksyms; i++) {
		u64 *addrs = (u64 *)(info_linear->info.jited_ksyms);
		u64 addr = addrs[i];
		struct map *map;

		map = map_groups__find(&machine->kmaps, addr);

		if (map) {
			map->dso->binary_type = DSO_BINARY_TYPE__BPF_PROG_INFO;
			map->dso->bpf_prog.id = id;
			map->dso->bpf_prog.sub_id = i;
			map->dso->bpf_prog.env = env;
		}
	}
	return 0;
}

int machine__process_bpf_event(struct machine *machine __maybe_unused,
			       union perf_event *event,
			       struct perf_sample *sample __maybe_unused)
{
	if (dump_trace)
		perf_event__fprintf_bpf_event(event, stdout);

	switch (event->bpf_event.type) {
	case PERF_BPF_EVENT_PROG_LOAD:
		return machine__process_bpf_event_load(machine, event, sample);

	case PERF_BPF_EVENT_PROG_UNLOAD:
		/*
		 * Do not free bpf_prog_info and btf of the program here,
		 * as annotation still need them. They will be freed at
		 * the end of the session.
		 */
		break;
	default:
		pr_debug("unexpected bpf_event type of %d\n",
			 event->bpf_event.type);
		break;
	}
	return 0;
}

static int perf_env__fetch_btf(struct perf_env *env,
			       u32 btf_id,
			       struct btf *btf)
{
	struct btf_node *node;
	u32 data_size;
	const void *data;

	data = btf__get_raw_data(btf, &data_size);

	node = malloc(data_size + sizeof(struct btf_node));
	if (!node)
		return -1;

	node->id = btf_id;
	node->data_size = data_size;
	memcpy(node->data, data, data_size);

	perf_env__insert_btf(env, node);
	return 0;
}

/*
 * Synthesize PERF_RECORD_KSYMBOL and PERF_RECORD_BPF_EVENT for one bpf
 * program. One PERF_RECORD_BPF_EVENT is generated for the program. And
 * one PERF_RECORD_KSYMBOL is generated for each sub program.
 *
 * Returns:
 *    0 for success;
 *   -1 for failures;
 *   -2 for lack of kernel support.
 */
static int perf_event__synthesize_one_bpf_prog(struct perf_session *session,
					       perf_event__handler_t process,
					       struct machine *machine,
					       int fd,
					       union perf_event *event,
					       struct record_opts *opts)
{
	struct ksymbol_event *ksymbol_event = &event->ksymbol_event;
	struct bpf_event *bpf_event = &event->bpf_event;
	struct bpf_prog_info_linear *info_linear;
	struct perf_tool *tool = session->tool;
	struct bpf_prog_info_node *info_node;
	struct bpf_prog_info *info;
	struct btf *btf = NULL;
	bool has_btf = false;
	struct perf_env *env;
	u32 sub_prog_cnt, i;
	int err = 0;
	u64 arrays;

	/*
	 * for perf-record and perf-report use header.env;
	 * otherwise, use global perf_env.
	 */
	env = session->data ? &session->header.env : &perf_env;

	arrays = 1UL << BPF_PROG_INFO_JITED_KSYMS;
	arrays |= 1UL << BPF_PROG_INFO_JITED_FUNC_LENS;
	arrays |= 1UL << BPF_PROG_INFO_FUNC_INFO;
	arrays |= 1UL << BPF_PROG_INFO_PROG_TAGS;
	arrays |= 1UL << BPF_PROG_INFO_JITED_INSNS;
	arrays |= 1UL << BPF_PROG_INFO_LINE_INFO;
	arrays |= 1UL << BPF_PROG_INFO_JITED_LINE_INFO;

	info_linear = bpf_program__get_prog_info_linear(fd, arrays);
	if (IS_ERR_OR_NULL(info_linear)) {
		info_linear = NULL;
		pr_debug("%s: failed to get BPF program info. aborting\n", __func__);
		return -1;
	}

	if (info_linear->info_len < offsetof(struct bpf_prog_info, prog_tags)) {
		pr_debug("%s: the kernel is too old, aborting\n", __func__);
		return -2;
	}

	info = &info_linear->info;

	/* number of ksyms, func_lengths, and tags should match */
	sub_prog_cnt = info->nr_jited_ksyms;
	if (sub_prog_cnt != info->nr_prog_tags ||
	    sub_prog_cnt != info->nr_jited_func_lens)
		return -1;

	/* check BTF func info support */
	if (info->btf_id && info->nr_func_info && info->func_info_rec_size) {
		/* btf func info number should be same as sub_prog_cnt */
		if (sub_prog_cnt != info->nr_func_info) {
			pr_debug("%s: mismatch in BPF sub program count and BTF function info count, aborting\n", __func__);
			err = -1;
			goto out;
		}
		if (btf__get_from_id(info->btf_id, &btf)) {
			pr_debug("%s: failed to get BTF of id %u, aborting\n", __func__, info->btf_id);
			err = -1;
			btf = NULL;
			goto out;
		}
		has_btf = true;
		perf_env__fetch_btf(env, info->btf_id, btf);
	}

	/* Synthesize PERF_RECORD_KSYMBOL */
	for (i = 0; i < sub_prog_cnt; i++) {
		u8 (*prog_tags)[BPF_TAG_SIZE] = (void *)(uintptr_t)(info->prog_tags);
		__u32 *prog_lens  = (__u32 *)(uintptr_t)(info->jited_func_lens);
		__u64 *prog_addrs = (__u64 *)(uintptr_t)(info->jited_ksyms);
		void *func_infos  = (void *)(uintptr_t)(info->func_info);
		const struct bpf_func_info *finfo;
		const char *short_name = NULL;
		const struct btf_type *t;
		int name_len;

		*ksymbol_event = (struct ksymbol_event){
			.header = {
				.type = PERF_RECORD_KSYMBOL,
				.size = offsetof(struct ksymbol_event, name),
			},
			.addr = prog_addrs[i],
			.len = prog_lens[i],
			.ksym_type = PERF_RECORD_KSYMBOL_TYPE_BPF,
			.flags = 0,
		};
		name_len = snprintf(ksymbol_event->name, KSYM_NAME_LEN,
				    "bpf_prog_");
		name_len += snprintf_hex(ksymbol_event->name + name_len,
					 KSYM_NAME_LEN - name_len,
					 prog_tags[i], BPF_TAG_SIZE);
		if (has_btf) {
			finfo = func_infos + i * info->func_info_rec_size;
			t = btf__type_by_id(btf, finfo->type_id);
			short_name = btf__name_by_offset(btf, t->name_off);
		} else if (i == 0 && sub_prog_cnt == 1) {
			/* no subprog */
			if (info->name[0])
				short_name = info->name;
		} else
			short_name = "F";
		if (short_name)
			name_len += snprintf(ksymbol_event->name + name_len,
					     KSYM_NAME_LEN - name_len,
					     "_%s", short_name);

		ksymbol_event->header.size += PERF_ALIGN(name_len + 1,
							 sizeof(u64));

		memset((void *)event + event->header.size, 0, machine->id_hdr_size);
		event->header.size += machine->id_hdr_size;
		err = perf_tool__process_synth_event(tool, event,
						     machine, process);
	}

	if (!opts->no_bpf_event) {
		/* Synthesize PERF_RECORD_BPF_EVENT */
		*bpf_event = (struct bpf_event){
			.header = {
				.type = PERF_RECORD_BPF_EVENT,
				.size = sizeof(struct bpf_event),
			},
			.type = PERF_BPF_EVENT_PROG_LOAD,
			.flags = 0,
			.id = info->id,
		};
		memcpy(bpf_event->tag, info->tag, BPF_TAG_SIZE);
		memset((void *)event + event->header.size, 0, machine->id_hdr_size);
		event->header.size += machine->id_hdr_size;

		/* save bpf_prog_info to env */
		info_node = malloc(sizeof(struct bpf_prog_info_node));
		if (!info_node) {
			err = -1;
			goto out;
		}

		info_node->info_linear = info_linear;
		perf_env__insert_bpf_prog_info(env, info_node);
		info_linear = NULL;

		/*
		 * process after saving bpf_prog_info to env, so that
		 * required information is ready for look up
		 */
		err = perf_tool__process_synth_event(tool, event,
						     machine, process);
	}

out:
	free(info_linear);
	free(btf);
	return err ? -1 : 0;
}

int perf_event__synthesize_bpf_events(struct perf_session *session,
				      perf_event__handler_t process,
				      struct machine *machine,
				      struct record_opts *opts)
{
	union perf_event *event;
	__u32 id = 0;
	int err;
	int fd;

	event = malloc(sizeof(event->bpf_event) + KSYM_NAME_LEN + machine->id_hdr_size);
	if (!event)
		return -1;
	while (true) {
		err = bpf_prog_get_next_id(id, &id);
		if (err) {
			if (errno == ENOENT) {
				err = 0;
				break;
			}
			pr_debug("%s: can't get next program: %s%s\n",
				 __func__, strerror(errno),
				 errno == EINVAL ? " -- kernel too old?" : "");
			/* don't report error on old kernel or EPERM  */
			err = (errno == EINVAL || errno == EPERM) ? 0 : -1;
			break;
		}
		fd = bpf_prog_get_fd_by_id(id);
		if (fd < 0) {
			pr_debug("%s: failed to get fd for prog_id %u\n",
				 __func__, id);
			continue;
		}

		err = perf_event__synthesize_one_bpf_prog(session, process,
							  machine, fd,
							  event, opts);
		close(fd);
		if (err) {
			/* do not return error for old kernel */
			if (err == -2)
				err = 0;
			break;
		}
	}
	free(event);
	return err;
}
