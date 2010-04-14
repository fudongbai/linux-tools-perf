/*
 * probe-finder.c : C expression to kprobe event converter
 *
 * Written by Masami Hiramatsu <mhiramat@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "string.h"
#include "event.h"
#include "debug.h"
#include "util.h"
#include "probe-finder.h"


/*
 * Generic dwarf analysis helpers
 */

#define X86_32_MAX_REGS 8
const char *x86_32_regs_table[X86_32_MAX_REGS] = {
	"%ax",
	"%cx",
	"%dx",
	"%bx",
	"$stack",	/* Stack address instead of %sp */
	"%bp",
	"%si",
	"%di",
};

#define X86_64_MAX_REGS 16
const char *x86_64_regs_table[X86_64_MAX_REGS] = {
	"%ax",
	"%dx",
	"%cx",
	"%bx",
	"%si",
	"%di",
	"%bp",
	"%sp",
	"%r8",
	"%r9",
	"%r10",
	"%r11",
	"%r12",
	"%r13",
	"%r14",
	"%r15",
};

/* TODO: switching by dwarf address size */
#ifdef __x86_64__
#define ARCH_MAX_REGS X86_64_MAX_REGS
#define arch_regs_table x86_64_regs_table
#else
#define ARCH_MAX_REGS X86_32_MAX_REGS
#define arch_regs_table x86_32_regs_table
#endif

/* Kprobe tracer basic type is up to u64 */
#define MAX_BASIC_TYPE_BITS	64

/* Return architecture dependent register string (for kprobe-tracer) */
static const char *get_arch_regstr(unsigned int n)
{
	return (n <= ARCH_MAX_REGS) ? arch_regs_table[n] : NULL;
}

/*
 * Compare the tail of two strings.
 * Return 0 if whole of either string is same as another's tail part.
 */
static int strtailcmp(const char *s1, const char *s2)
{
	int i1 = strlen(s1);
	int i2 = strlen(s2);
	while (--i1 >= 0 && --i2 >= 0) {
		if (s1[i1] != s2[i2])
			return s1[i1] - s2[i2];
	}
	return 0;
}

/* Line number list operations */

/* Add a line to line number list */
static int line_list__add_line(struct list_head *head, int line)
{
	struct line_node *ln;
	struct list_head *p;

	/* Reverse search, because new line will be the last one */
	list_for_each_entry_reverse(ln, head, list) {
		if (ln->line < line) {
			p = &ln->list;
			goto found;
		} else if (ln->line == line)	/* Already exist */
			return 1;
	}
	/* List is empty, or the smallest entry */
	p = head;
found:
	pr_debug("line list: add a line %u\n", line);
	ln = zalloc(sizeof(struct line_node));
	if (ln == NULL)
		return -ENOMEM;
	ln->line = line;
	INIT_LIST_HEAD(&ln->list);
	list_add(&ln->list, p);
	return 0;
}

/* Check if the line in line number list */
static int line_list__has_line(struct list_head *head, int line)
{
	struct line_node *ln;

	/* Reverse search, because new line will be the last one */
	list_for_each_entry(ln, head, list)
		if (ln->line == line)
			return 1;

	return 0;
}

/* Init line number list */
static void line_list__init(struct list_head *head)
{
	INIT_LIST_HEAD(head);
}

/* Free line number list */
static void line_list__free(struct list_head *head)
{
	struct line_node *ln;
	while (!list_empty(head)) {
		ln = list_first_entry(head, struct line_node, list);
		list_del(&ln->list);
		free(ln);
	}
}

/* Dwarf wrappers */

/* Find the realpath of the target file. */
static const char *cu_find_realpath(Dwarf_Die *cu_die, const char *fname)
{
	Dwarf_Files *files;
	size_t nfiles, i;
	const char *src = NULL;
	int ret;

	if (!fname)
		return NULL;

	ret = dwarf_getsrcfiles(cu_die, &files, &nfiles);
	if (ret != 0)
		return NULL;

	for (i = 0; i < nfiles; i++) {
		src = dwarf_filesrc(files, i, NULL, NULL);
		if (strtailcmp(src, fname) == 0)
			break;
	}
	if (i == nfiles)
		return NULL;
	return src;
}

/* Compare diename and tname */
static bool die_compare_name(Dwarf_Die *dw_die, const char *tname)
{
	const char *name;
	name = dwarf_diename(dw_die);
	return name ? strcmp(tname, name) : -1;
}

/* Get type die, but skip qualifiers and typedef */
static Dwarf_Die *die_get_real_type(Dwarf_Die *vr_die, Dwarf_Die *die_mem)
{
	Dwarf_Attribute attr;
	int tag;

	do {
		if (dwarf_attr(vr_die, DW_AT_type, &attr) == NULL ||
		    dwarf_formref_die(&attr, die_mem) == NULL)
			return NULL;

		tag = dwarf_tag(die_mem);
		vr_die = die_mem;
	} while (tag == DW_TAG_const_type ||
		 tag == DW_TAG_restrict_type ||
		 tag == DW_TAG_volatile_type ||
		 tag == DW_TAG_shared_type ||
		 tag == DW_TAG_typedef);

	return die_mem;
}

static bool die_is_signed_type(Dwarf_Die *tp_die)
{
	Dwarf_Attribute attr;
	Dwarf_Word ret;

	if (dwarf_attr(tp_die, DW_AT_encoding, &attr) == NULL ||
	    dwarf_formudata(&attr, &ret) != 0)
		return false;

	return (ret == DW_ATE_signed_char || ret == DW_ATE_signed ||
		ret == DW_ATE_signed_fixed);
}

static int die_get_byte_size(Dwarf_Die *tp_die)
{
	Dwarf_Attribute attr;
	Dwarf_Word ret;

	if (dwarf_attr(tp_die, DW_AT_byte_size, &attr) == NULL ||
	    dwarf_formudata(&attr, &ret) != 0)
		return 0;

	return (int)ret;
}

/* Return values for die_find callbacks */
enum {
	DIE_FIND_CB_FOUND = 0,		/* End of Search */
	DIE_FIND_CB_CHILD = 1,		/* Search only children */
	DIE_FIND_CB_SIBLING = 2,	/* Search only siblings */
	DIE_FIND_CB_CONTINUE = 3,	/* Search children and siblings */
};

/* Search a child die */
static Dwarf_Die *die_find_child(Dwarf_Die *rt_die,
				 int (*callback)(Dwarf_Die *, void *),
				 void *data, Dwarf_Die *die_mem)
{
	Dwarf_Die child_die;
	int ret;

	ret = dwarf_child(rt_die, die_mem);
	if (ret != 0)
		return NULL;

	do {
		ret = callback(die_mem, data);
		if (ret == DIE_FIND_CB_FOUND)
			return die_mem;

		if ((ret & DIE_FIND_CB_CHILD) &&
		    die_find_child(die_mem, callback, data, &child_die)) {
			memcpy(die_mem, &child_die, sizeof(Dwarf_Die));
			return die_mem;
		}
	} while ((ret & DIE_FIND_CB_SIBLING) &&
		 dwarf_siblingof(die_mem, die_mem) == 0);

	return NULL;
}

struct __addr_die_search_param {
	Dwarf_Addr	addr;
	Dwarf_Die	*die_mem;
};

static int __die_search_func_cb(Dwarf_Die *fn_die, void *data)
{
	struct __addr_die_search_param *ad = data;

	if (dwarf_tag(fn_die) == DW_TAG_subprogram &&
	    dwarf_haspc(fn_die, ad->addr)) {
		memcpy(ad->die_mem, fn_die, sizeof(Dwarf_Die));
		return DWARF_CB_ABORT;
	}
	return DWARF_CB_OK;
}

/* Search a real subprogram including this line, */
static Dwarf_Die *die_find_real_subprogram(Dwarf_Die *cu_die, Dwarf_Addr addr,
					   Dwarf_Die *die_mem)
{
	struct __addr_die_search_param ad;
	ad.addr = addr;
	ad.die_mem = die_mem;
	/* dwarf_getscopes can't find subprogram. */
	if (!dwarf_getfuncs(cu_die, __die_search_func_cb, &ad, 0))
		return NULL;
	else
		return die_mem;
}

/* die_find callback for inline function search */
static int __die_find_inline_cb(Dwarf_Die *die_mem, void *data)
{
	Dwarf_Addr *addr = data;

	if (dwarf_tag(die_mem) == DW_TAG_inlined_subroutine &&
	    dwarf_haspc(die_mem, *addr))
		return DIE_FIND_CB_FOUND;

	return DIE_FIND_CB_CONTINUE;
}

/* Similar to dwarf_getfuncs, but returns inlined_subroutine if exists. */
static Dwarf_Die *die_find_inlinefunc(Dwarf_Die *sp_die, Dwarf_Addr addr,
				      Dwarf_Die *die_mem)
{
	return die_find_child(sp_die, __die_find_inline_cb, &addr, die_mem);
}

static int __die_find_variable_cb(Dwarf_Die *die_mem, void *data)
{
	const char *name = data;
	int tag;

	tag = dwarf_tag(die_mem);
	if ((tag == DW_TAG_formal_parameter ||
	     tag == DW_TAG_variable) &&
	    (die_compare_name(die_mem, name) == 0))
		return DIE_FIND_CB_FOUND;

	return DIE_FIND_CB_CONTINUE;
}

/* Find a variable called 'name' */
static Dwarf_Die *die_find_variable(Dwarf_Die *sp_die, const char *name,
				    Dwarf_Die *die_mem)
{
	return die_find_child(sp_die, __die_find_variable_cb, (void *)name,
			      die_mem);
}

static int __die_find_member_cb(Dwarf_Die *die_mem, void *data)
{
	const char *name = data;

	if ((dwarf_tag(die_mem) == DW_TAG_member) &&
	    (die_compare_name(die_mem, name) == 0))
		return DIE_FIND_CB_FOUND;

	return DIE_FIND_CB_SIBLING;
}

/* Find a member called 'name' */
static Dwarf_Die *die_find_member(Dwarf_Die *st_die, const char *name,
				  Dwarf_Die *die_mem)
{
	return die_find_child(st_die, __die_find_member_cb, (void *)name,
			      die_mem);
}

/*
 * Probe finder related functions
 */

/* Show a location */
static int convert_location(Dwarf_Op *op, struct probe_finder *pf)
{
	unsigned int regn;
	Dwarf_Word offs = 0;
	bool ref = false;
	const char *regs;
	struct kprobe_trace_arg *tvar = pf->tvar;

	/* If this is based on frame buffer, set the offset */
	if (op->atom == DW_OP_fbreg) {
		if (pf->fb_ops == NULL) {
			pr_warning("The attribute of frame base is not "
				   "supported.\n");
			return -ENOTSUP;
		}
		ref = true;
		offs = op->number;
		op = &pf->fb_ops[0];
	}

	if (op->atom >= DW_OP_breg0 && op->atom <= DW_OP_breg31) {
		regn = op->atom - DW_OP_breg0;
		offs += op->number;
		ref = true;
	} else if (op->atom >= DW_OP_reg0 && op->atom <= DW_OP_reg31) {
		regn = op->atom - DW_OP_reg0;
	} else if (op->atom == DW_OP_bregx) {
		regn = op->number;
		offs += op->number2;
		ref = true;
	} else if (op->atom == DW_OP_regx) {
		regn = op->number;
	} else {
		pr_warning("DW_OP %x is not supported.\n", op->atom);
		return -ENOTSUP;
	}

	regs = get_arch_regstr(regn);
	if (!regs) {
		pr_warning("%u exceeds max register number.\n", regn);
		return -ERANGE;
	}

	tvar->value = strdup(regs);
	if (tvar->value == NULL)
		return -ENOMEM;

	if (ref) {
		tvar->ref = zalloc(sizeof(struct kprobe_trace_arg_ref));
		if (tvar->ref == NULL)
			return -ENOMEM;
		tvar->ref->offset = (long)offs;
	}
	return 0;
}

static int convert_variable_type(Dwarf_Die *vr_die,
				 struct kprobe_trace_arg *targ)
{
	Dwarf_Die type;
	char buf[16];
	int ret;

	if (die_get_real_type(vr_die, &type) == NULL) {
		pr_warning("Failed to get a type information of %s.\n",
			   dwarf_diename(vr_die));
		return -ENOENT;
	}

	ret = die_get_byte_size(&type) * 8;
	if (ret) {
		/* Check the bitwidth */
		if (ret > MAX_BASIC_TYPE_BITS) {
			pr_info("%s exceeds max-bitwidth."
				" Cut down to %d bits.\n",
				dwarf_diename(&type), MAX_BASIC_TYPE_BITS);
			ret = MAX_BASIC_TYPE_BITS;
		}

		ret = snprintf(buf, 16, "%c%d",
			       die_is_signed_type(&type) ? 's' : 'u', ret);
		if (ret < 0 || ret >= 16) {
			if (ret >= 16)
				ret = -E2BIG;
			pr_warning("Failed to convert variable type: %s\n",
				   strerror(-ret));
			return ret;
		}
		targ->type = strdup(buf);
		if (targ->type == NULL)
			return -ENOMEM;
	}
	return 0;
}

static int convert_variable_fields(Dwarf_Die *vr_die, const char *varname,
				    struct perf_probe_arg_field *field,
				    struct kprobe_trace_arg_ref **ref_ptr,
				    Dwarf_Die *die_mem)
{
	struct kprobe_trace_arg_ref *ref = *ref_ptr;
	Dwarf_Attribute attr;
	Dwarf_Die type;
	Dwarf_Word offs;

	pr_debug("converting %s in %s\n", field->name, varname);
	if (die_get_real_type(vr_die, &type) == NULL) {
		pr_warning("Failed to get the type of %s.\n", varname);
		return -ENOENT;
	}

	/* Check the pointer and dereference */
	if (dwarf_tag(&type) == DW_TAG_pointer_type) {
		if (!field->ref) {
			pr_err("Semantic error: %s must be referred by '->'\n",
			       field->name);
			return -EINVAL;
		}
		/* Get the type pointed by this pointer */
		if (die_get_real_type(&type, &type) == NULL) {
			pr_warning("Failed to get the type of %s.\n", varname);
			return -ENOENT;
		}
		/* Verify it is a data structure  */
		if (dwarf_tag(&type) != DW_TAG_structure_type) {
			pr_warning("%s is not a data structure.\n", varname);
			return -EINVAL;
		}

		ref = zalloc(sizeof(struct kprobe_trace_arg_ref));
		if (ref == NULL)
			return -ENOMEM;
		if (*ref_ptr)
			(*ref_ptr)->next = ref;
		else
			*ref_ptr = ref;
	} else {
		/* Verify it is a data structure  */
		if (dwarf_tag(&type) != DW_TAG_structure_type) {
			pr_warning("%s is not a data structure.\n", varname);
			return -EINVAL;
		}
		if (field->ref) {
			pr_err("Semantic error: %s must be referred by '.'\n",
			       field->name);
			return -EINVAL;
		}
		if (!ref) {
			pr_warning("Structure on a register is not "
				   "supported yet.\n");
			return -ENOTSUP;
		}
	}

	if (die_find_member(&type, field->name, die_mem) == NULL) {
		pr_warning("%s(tyep:%s) has no member %s.\n", varname,
			   dwarf_diename(&type), field->name);
		return -EINVAL;
	}

	/* Get the offset of the field */
	if (dwarf_attr(die_mem, DW_AT_data_member_location, &attr) == NULL ||
	    dwarf_formudata(&attr, &offs) != 0) {
		pr_warning("Failed to get the offset of %s.\n", field->name);
		return -ENOENT;
	}
	ref->offset += (long)offs;

	/* Converting next field */
	if (field->next)
		return convert_variable_fields(die_mem, field->name,
					       field->next, &ref, die_mem);
	else
		return 0;
}

/* Show a variables in kprobe event format */
static int convert_variable(Dwarf_Die *vr_die, struct probe_finder *pf)
{
	Dwarf_Attribute attr;
	Dwarf_Die die_mem;
	Dwarf_Op *expr;
	size_t nexpr;
	int ret;

	if (dwarf_attr(vr_die, DW_AT_location, &attr) == NULL)
		goto error;
	/* TODO: handle more than 1 exprs */
	ret = dwarf_getlocation_addr(&attr, pf->addr, &expr, &nexpr, 1);
	if (ret <= 0 || nexpr == 0)
		goto error;

	ret = convert_location(expr, pf);
	if (ret == 0 && pf->pvar->field) {
		ret = convert_variable_fields(vr_die, pf->pvar->var,
					      pf->pvar->field, &pf->tvar->ref,
					      &die_mem);
		vr_die = &die_mem;
	}
	if (ret == 0) {
		if (pf->pvar->type) {
			pf->tvar->type = strdup(pf->pvar->type);
			if (pf->tvar->type == NULL)
				ret = -ENOMEM;
		} else
			ret = convert_variable_type(vr_die, pf->tvar);
	}
	/* *expr will be cached in libdw. Don't free it. */
	return ret;
error:
	/* TODO: Support const_value */
	pr_err("Failed to find the location of %s at this address.\n"
	       " Perhaps, it has been optimized out.\n", pf->pvar->var);
	return -ENOENT;
}

/* Find a variable in a subprogram die */
static int find_variable(Dwarf_Die *sp_die, struct probe_finder *pf)
{
	Dwarf_Die vr_die;
	char buf[32], *ptr;
	int ret;

	/* TODO: Support arrays */
	if (pf->pvar->name)
		pf->tvar->name = strdup(pf->pvar->name);
	else {
		ret = synthesize_perf_probe_arg(pf->pvar, buf, 32);
		if (ret < 0)
			return ret;
		ptr = strchr(buf, ':');	/* Change type separator to _ */
		if (ptr)
			*ptr = '_';
		pf->tvar->name = strdup(buf);
	}
	if (pf->tvar->name == NULL)
		return -ENOMEM;

	if (!is_c_varname(pf->pvar->var)) {
		/* Copy raw parameters */
		pf->tvar->value = strdup(pf->pvar->var);
		if (pf->tvar->value == NULL)
			return -ENOMEM;
		else
			return 0;
	}

	pr_debug("Searching '%s' variable in context.\n",
		 pf->pvar->var);
	/* Search child die for local variables and parameters. */
	if (!die_find_variable(sp_die, pf->pvar->var, &vr_die)) {
		pr_warning("Failed to find '%s' in this function.\n",
			   pf->pvar->var);
		return -ENOENT;
	}
	return convert_variable(&vr_die, pf);
}

/* Show a probe point to output buffer */
static int convert_probe_point(Dwarf_Die *sp_die, struct probe_finder *pf)
{
	struct kprobe_trace_event *tev;
	Dwarf_Addr eaddr;
	Dwarf_Die die_mem;
	const char *name;
	int ret, i;
	Dwarf_Attribute fb_attr;
	size_t nops;

	if (pf->ntevs == MAX_PROBES) {
		pr_warning("Too many( > %d) probe point found.\n", MAX_PROBES);
		return -ERANGE;
	}
	tev = &pf->tevs[pf->ntevs++];

	/* If no real subprogram, find a real one */
	if (!sp_die || dwarf_tag(sp_die) != DW_TAG_subprogram) {
		sp_die = die_find_real_subprogram(&pf->cu_die,
						 pf->addr, &die_mem);
		if (!sp_die) {
			pr_warning("Failed to find probe point in any "
				   "functions.\n");
			return -ENOENT;
		}
	}

	/* Copy the name of probe point */
	name = dwarf_diename(sp_die);
	if (name) {
		if (dwarf_entrypc(sp_die, &eaddr) != 0) {
			pr_warning("Failed to get entry pc of %s\n",
				   dwarf_diename(sp_die));
			return -ENOENT;
		}
		tev->point.symbol = strdup(name);
		if (tev->point.symbol == NULL)
			return -ENOMEM;
		tev->point.offset = (unsigned long)(pf->addr - eaddr);
	} else
		/* This function has no name. */
		tev->point.offset = (unsigned long)pf->addr;

	pr_debug("Probe point found: %s+%lu\n", tev->point.symbol,
		 tev->point.offset);

	/* Get the frame base attribute/ops */
	dwarf_attr(sp_die, DW_AT_frame_base, &fb_attr);
	ret = dwarf_getlocation_addr(&fb_attr, pf->addr, &pf->fb_ops, &nops, 1);
	if (ret <= 0 || nops == 0) {
		pf->fb_ops = NULL;
	} else if (nops == 1 && pf->fb_ops[0].atom == DW_OP_call_frame_cfa &&
		   pf->cfi != NULL) {
		Dwarf_Frame *frame;
		if (dwarf_cfi_addrframe(pf->cfi, pf->addr, &frame) != 0 ||
		    dwarf_frame_cfa(frame, &pf->fb_ops, &nops) != 0) {
			pr_warning("Failed to get CFA on 0x%jx\n",
				   (uintmax_t)pf->addr);
			return -ENOENT;
		}
	}

	/* Find each argument */
	tev->nargs = pf->pev->nargs;
	tev->args = zalloc(sizeof(struct kprobe_trace_arg) * tev->nargs);
	if (tev->args == NULL)
		return -ENOMEM;
	for (i = 0; i < pf->pev->nargs; i++) {
		pf->pvar = &pf->pev->args[i];
		pf->tvar = &tev->args[i];
		ret = find_variable(sp_die, pf);
		if (ret != 0)
			return ret;
	}

	/* *pf->fb_ops will be cached in libdw. Don't free it. */
	pf->fb_ops = NULL;
	return 0;
}

/* Find probe point from its line number */
static int find_probe_point_by_line(struct probe_finder *pf)
{
	Dwarf_Lines *lines;
	Dwarf_Line *line;
	size_t nlines, i;
	Dwarf_Addr addr;
	int lineno;
	int ret = 0;

	if (dwarf_getsrclines(&pf->cu_die, &lines, &nlines) != 0) {
		pr_warning("No source lines found in this CU.\n");
		return -ENOENT;
	}

	for (i = 0; i < nlines && ret == 0; i++) {
		line = dwarf_onesrcline(lines, i);
		if (dwarf_lineno(line, &lineno) != 0 ||
		    lineno != pf->lno)
			continue;

		/* TODO: Get fileno from line, but how? */
		if (strtailcmp(dwarf_linesrc(line, NULL, NULL), pf->fname) != 0)
			continue;

		if (dwarf_lineaddr(line, &addr) != 0) {
			pr_warning("Failed to get the address of the line.\n");
			return -ENOENT;
		}
		pr_debug("Probe line found: line[%d]:%d addr:0x%jx\n",
			 (int)i, lineno, (uintmax_t)addr);
		pf->addr = addr;

		ret = convert_probe_point(NULL, pf);
		/* Continuing, because target line might be inlined. */
	}
	return ret;
}

/* Find lines which match lazy pattern */
static int find_lazy_match_lines(struct list_head *head,
				 const char *fname, const char *pat)
{
	char *fbuf, *p1, *p2;
	int fd, ret, line, nlines = 0;
	struct stat st;

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		pr_warning("Failed to open %s: %s\n", fname, strerror(-fd));
		return fd;
	}

	ret = fstat(fd, &st);
	if (ret < 0) {
		pr_warning("Failed to get the size of %s: %s\n",
			   fname, strerror(errno));
		return ret;
	}
	fbuf = xmalloc(st.st_size + 2);
	ret = read(fd, fbuf, st.st_size);
	if (ret < 0) {
		pr_warning("Failed to read %s: %s\n", fname, strerror(errno));
		return ret;
	}
	close(fd);
	fbuf[st.st_size] = '\n';	/* Dummy line */
	fbuf[st.st_size + 1] = '\0';
	p1 = fbuf;
	line = 1;
	while ((p2 = strchr(p1, '\n')) != NULL) {
		*p2 = '\0';
		if (strlazymatch(p1, pat)) {
			line_list__add_line(head, line);
			nlines++;
		}
		line++;
		p1 = p2 + 1;
	}
	free(fbuf);
	return nlines;
}

/* Find probe points from lazy pattern  */
static int find_probe_point_lazy(Dwarf_Die *sp_die, struct probe_finder *pf)
{
	Dwarf_Lines *lines;
	Dwarf_Line *line;
	size_t nlines, i;
	Dwarf_Addr addr;
	Dwarf_Die die_mem;
	int lineno;
	int ret = 0;

	if (list_empty(&pf->lcache)) {
		/* Matching lazy line pattern */
		ret = find_lazy_match_lines(&pf->lcache, pf->fname,
					    pf->pev->point.lazy_line);
		if (ret == 0) {
			pr_debug("No matched lines found in %s.\n", pf->fname);
			return 0;
		} else if (ret < 0)
			return ret;
	}

	if (dwarf_getsrclines(&pf->cu_die, &lines, &nlines) != 0) {
		pr_warning("No source lines found in this CU.\n");
		return -ENOENT;
	}

	for (i = 0; i < nlines && ret >= 0; i++) {
		line = dwarf_onesrcline(lines, i);

		if (dwarf_lineno(line, &lineno) != 0 ||
		    !line_list__has_line(&pf->lcache, lineno))
			continue;

		/* TODO: Get fileno from line, but how? */
		if (strtailcmp(dwarf_linesrc(line, NULL, NULL), pf->fname) != 0)
			continue;

		if (dwarf_lineaddr(line, &addr) != 0) {
			pr_debug("Failed to get the address of line %d.\n",
				 lineno);
			continue;
		}
		if (sp_die) {
			/* Address filtering 1: does sp_die include addr? */
			if (!dwarf_haspc(sp_die, addr))
				continue;
			/* Address filtering 2: No child include addr? */
			if (die_find_inlinefunc(sp_die, addr, &die_mem))
				continue;
		}

		pr_debug("Probe line found: line[%d]:%d addr:0x%llx\n",
			 (int)i, lineno, (unsigned long long)addr);
		pf->addr = addr;

		ret = convert_probe_point(sp_die, pf);
		/* Continuing, because target line might be inlined. */
	}
	/* TODO: deallocate lines, but how? */
	return ret;
}

/* Callback parameter with return value */
struct dwarf_callback_param {
	void *data;
	int retval;
};

static int probe_point_inline_cb(Dwarf_Die *in_die, void *data)
{
	struct dwarf_callback_param *param = data;
	struct probe_finder *pf = param->data;
	struct perf_probe_point *pp = &pf->pev->point;
	Dwarf_Addr addr;

	if (pp->lazy_line)
		param->retval = find_probe_point_lazy(in_die, pf);
	else {
		/* Get probe address */
		if (dwarf_entrypc(in_die, &addr) != 0) {
			pr_warning("Failed to get entry pc of %s.\n",
				   dwarf_diename(in_die));
			param->retval = -ENOENT;
			return DWARF_CB_ABORT;
		}
		pf->addr = addr;
		pf->addr += pp->offset;
		pr_debug("found inline addr: 0x%jx\n",
			 (uintmax_t)pf->addr);

		param->retval = convert_probe_point(in_die, pf);
	}

	return DWARF_CB_OK;
}

/* Search function from function name */
static int probe_point_search_cb(Dwarf_Die *sp_die, void *data)
{
	struct dwarf_callback_param *param = data;
	struct probe_finder *pf = param->data;
	struct perf_probe_point *pp = &pf->pev->point;

	/* Check tag and diename */
	if (dwarf_tag(sp_die) != DW_TAG_subprogram ||
	    die_compare_name(sp_die, pp->function) != 0)
		return DWARF_CB_OK;

	pf->fname = dwarf_decl_file(sp_die);
	if (pp->line) { /* Function relative line */
		dwarf_decl_line(sp_die, &pf->lno);
		pf->lno += pp->line;
		param->retval = find_probe_point_by_line(pf);
	} else if (!dwarf_func_inline(sp_die)) {
		/* Real function */
		if (pp->lazy_line)
			param->retval = find_probe_point_lazy(sp_die, pf);
		else {
			if (dwarf_entrypc(sp_die, &pf->addr) != 0) {
				pr_warning("Failed to get entry pc of %s.\n",
					   dwarf_diename(sp_die));
				param->retval = -ENOENT;
				return DWARF_CB_ABORT;
			}
			pf->addr += pp->offset;
			/* TODO: Check the address in this function */
			param->retval = convert_probe_point(sp_die, pf);
		}
	} else {
		struct dwarf_callback_param _param = {.data = (void *)pf,
						      .retval = 0};
		/* Inlined function: search instances */
		dwarf_func_inline_instances(sp_die, probe_point_inline_cb,
					    &_param);
		param->retval = _param.retval;
	}

	return DWARF_CB_ABORT; /* Exit; no same symbol in this CU. */
}

static int find_probe_point_by_func(struct probe_finder *pf)
{
	struct dwarf_callback_param _param = {.data = (void *)pf,
					      .retval = 0};
	dwarf_getfuncs(&pf->cu_die, probe_point_search_cb, &_param, 0);
	return _param.retval;
}

/* Find kprobe_trace_events specified by perf_probe_event from debuginfo */
int find_kprobe_trace_events(int fd, struct perf_probe_event *pev,
			     struct kprobe_trace_event **tevs)
{
	struct probe_finder pf = {.pev = pev};
	struct perf_probe_point *pp = &pev->point;
	Dwarf_Off off, noff;
	size_t cuhl;
	Dwarf_Die *diep;
	Dwarf *dbg;
	int ret = 0;

	pf.tevs = zalloc(sizeof(struct kprobe_trace_event) * MAX_PROBES);
	if (pf.tevs == NULL)
		return -ENOMEM;
	*tevs = pf.tevs;
	pf.ntevs = 0;

	dbg = dwarf_begin(fd, DWARF_C_READ);
	if (!dbg) {
		pr_warning("No dwarf info found in the vmlinux - "
			"please rebuild with CONFIG_DEBUG_INFO=y.\n");
		return -EBADF;
	}

	/* Get the call frame information from this dwarf */
	pf.cfi = dwarf_getcfi(dbg);

	off = 0;
	line_list__init(&pf.lcache);
	/* Loop on CUs (Compilation Unit) */
	while (!dwarf_nextcu(dbg, off, &noff, &cuhl, NULL, NULL, NULL) &&
	       ret >= 0) {
		/* Get the DIE(Debugging Information Entry) of this CU */
		diep = dwarf_offdie(dbg, off + cuhl, &pf.cu_die);
		if (!diep)
			continue;

		/* Check if target file is included. */
		if (pp->file)
			pf.fname = cu_find_realpath(&pf.cu_die, pp->file);
		else
			pf.fname = NULL;

		if (!pp->file || pf.fname) {
			if (pp->function)
				ret = find_probe_point_by_func(&pf);
			else if (pp->lazy_line)
				ret = find_probe_point_lazy(NULL, &pf);
			else {
				pf.lno = pp->line;
				ret = find_probe_point_by_line(&pf);
			}
		}
		off = noff;
	}
	line_list__free(&pf.lcache);
	dwarf_end(dbg);

	return (ret < 0) ? ret : pf.ntevs;
}

/* Reverse search */
int find_perf_probe_point(int fd, unsigned long addr,
			  struct perf_probe_point *ppt)
{
	Dwarf_Die cudie, spdie, indie;
	Dwarf *dbg;
	Dwarf_Line *line;
	Dwarf_Addr laddr, eaddr;
	const char *tmp;
	int lineno, ret = 0;
	bool found = false;

	dbg = dwarf_begin(fd, DWARF_C_READ);
	if (!dbg)
		return -EBADF;

	/* Find cu die */
	if (!dwarf_addrdie(dbg, (Dwarf_Addr)addr, &cudie)) {
		ret = -EINVAL;
		goto end;
	}

	/* Find a corresponding line */
	line = dwarf_getsrc_die(&cudie, (Dwarf_Addr)addr);
	if (line) {
		if (dwarf_lineaddr(line, &laddr) == 0 &&
		    (Dwarf_Addr)addr == laddr &&
		    dwarf_lineno(line, &lineno) == 0) {
			tmp = dwarf_linesrc(line, NULL, NULL);
			if (tmp) {
				ppt->line = lineno;
				ppt->file = strdup(tmp);
				if (ppt->file == NULL) {
					ret = -ENOMEM;
					goto end;
				}
				found = true;
			}
		}
	}

	/* Find a corresponding function */
	if (die_find_real_subprogram(&cudie, (Dwarf_Addr)addr, &spdie)) {
		tmp = dwarf_diename(&spdie);
		if (!tmp || dwarf_entrypc(&spdie, &eaddr) != 0)
			goto end;

		if (ppt->line) {
			if (die_find_inlinefunc(&spdie, (Dwarf_Addr)addr,
						&indie)) {
				/* addr in an inline function */
				tmp = dwarf_diename(&indie);
				if (!tmp)
					goto end;
				ret = dwarf_decl_line(&indie, &lineno);
			} else {
				if (eaddr == addr) {	/* Function entry */
					lineno = ppt->line;
					ret = 0;
				} else
					ret = dwarf_decl_line(&spdie, &lineno);
			}
			if (ret == 0) {
				/* Make a relative line number */
				ppt->line -= lineno;
				goto found;
			}
		}
		/* We don't have a line number, let's use offset */
		ppt->offset = addr - (unsigned long)eaddr;
found:
		ppt->function = strdup(tmp);
		if (ppt->function == NULL) {
			ret = -ENOMEM;
			goto end;
		}
		found = true;
	}

end:
	dwarf_end(dbg);
	if (ret >= 0)
		ret = found ? 1 : 0;
	return ret;
}


/* Find line range from its line number */
static int find_line_range_by_line(Dwarf_Die *sp_die, struct line_finder *lf)
{
	Dwarf_Lines *lines;
	Dwarf_Line *line;
	size_t nlines, i;
	Dwarf_Addr addr;
	int lineno;
	const char *src;
	Dwarf_Die die_mem;

	line_list__init(&lf->lr->line_list);
	if (dwarf_getsrclines(&lf->cu_die, &lines, &nlines) != 0) {
		pr_warning("No source lines found in this CU.\n");
		return -ENOENT;
	}

	for (i = 0; i < nlines; i++) {
		line = dwarf_onesrcline(lines, i);
		if (dwarf_lineno(line, &lineno) != 0 ||
		    (lf->lno_s > lineno || lf->lno_e < lineno))
			continue;

		if (sp_die) {
			/* Address filtering 1: does sp_die include addr? */
			if (dwarf_lineaddr(line, &addr) != 0 ||
			    !dwarf_haspc(sp_die, addr))
				continue;

			/* Address filtering 2: No child include addr? */
			if (die_find_inlinefunc(sp_die, addr, &die_mem))
				continue;
		}

		/* TODO: Get fileno from line, but how? */
		src = dwarf_linesrc(line, NULL, NULL);
		if (strtailcmp(src, lf->fname) != 0)
			continue;

		/* Copy real path */
		if (!lf->lr->path) {
			lf->lr->path = strdup(src);
			if (lf->lr->path == NULL)
				return -ENOMEM;
		}
		line_list__add_line(&lf->lr->line_list, lineno);
	}
	/* Update status */
	if (!list_empty(&lf->lr->line_list))
		lf->found = 1;
	else {
		free(lf->lr->path);
		lf->lr->path = NULL;
	}
	return lf->found;
}

static int line_range_inline_cb(Dwarf_Die *in_die, void *data)
{
	struct dwarf_callback_param *param = data;

	param->retval = find_line_range_by_line(in_die, param->data);
	return DWARF_CB_ABORT;	/* No need to find other instances */
}

/* Search function from function name */
static int line_range_search_cb(Dwarf_Die *sp_die, void *data)
{
	struct dwarf_callback_param *param = data;
	struct line_finder *lf = param->data;
	struct line_range *lr = lf->lr;

	if (dwarf_tag(sp_die) == DW_TAG_subprogram &&
	    die_compare_name(sp_die, lr->function) == 0) {
		lf->fname = dwarf_decl_file(sp_die);
		dwarf_decl_line(sp_die, &lr->offset);
		pr_debug("fname: %s, lineno:%d\n", lf->fname, lr->offset);
		lf->lno_s = lr->offset + lr->start;
		if (lf->lno_s < 0)	/* Overflow */
			lf->lno_s = INT_MAX;
		lf->lno_e = lr->offset + lr->end;
		if (lf->lno_e < 0)	/* Overflow */
			lf->lno_e = INT_MAX;
		pr_debug("New line range: %d to %d\n", lf->lno_s, lf->lno_e);
		lr->start = lf->lno_s;
		lr->end = lf->lno_e;
		if (dwarf_func_inline(sp_die)) {
			struct dwarf_callback_param _param;
			_param.data = (void *)lf;
			_param.retval = 0;
			dwarf_func_inline_instances(sp_die,
						    line_range_inline_cb,
						    &_param);
			param->retval = _param.retval;
		} else
			param->retval = find_line_range_by_line(sp_die, lf);
		return DWARF_CB_ABORT;
	}
	return DWARF_CB_OK;
}

static int find_line_range_by_func(struct line_finder *lf)
{
	struct dwarf_callback_param param = {.data = (void *)lf, .retval = 0};
	dwarf_getfuncs(&lf->cu_die, line_range_search_cb, &param, 0);
	return param.retval;
}

int find_line_range(int fd, struct line_range *lr)
{
	struct line_finder lf = {.lr = lr, .found = 0};
	int ret = 0;
	Dwarf_Off off = 0, noff;
	size_t cuhl;
	Dwarf_Die *diep;
	Dwarf *dbg;

	dbg = dwarf_begin(fd, DWARF_C_READ);
	if (!dbg) {
		pr_warning("No dwarf info found in the vmlinux - "
			"please rebuild with CONFIG_DEBUG_INFO=y.\n");
		return -EBADF;
	}

	/* Loop on CUs (Compilation Unit) */
	while (!lf.found && ret >= 0) {
		if (dwarf_nextcu(dbg, off, &noff, &cuhl, NULL, NULL, NULL) != 0)
			break;

		/* Get the DIE(Debugging Information Entry) of this CU */
		diep = dwarf_offdie(dbg, off + cuhl, &lf.cu_die);
		if (!diep)
			continue;

		/* Check if target file is included. */
		if (lr->file)
			lf.fname = cu_find_realpath(&lf.cu_die, lr->file);
		else
			lf.fname = 0;

		if (!lr->file || lf.fname) {
			if (lr->function)
				ret = find_line_range_by_func(&lf);
			else {
				lf.lno_s = lr->start;
				lf.lno_e = lr->end;
				ret = find_line_range_by_line(NULL, &lf);
			}
		}
		off = noff;
	}
	pr_debug("path: %lx\n", (unsigned long)lr->path);
	dwarf_end(dbg);

	return (ret < 0) ? ret : lf.found;
}

