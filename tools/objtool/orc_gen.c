/*
 * Copyright (C) 2017 Josh Poimboeuf <jpoimboe@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "orc.h"
#include "check.h"
#include "warn.h"

int create_orc(struct objtool_file *file)
{
	struct instruction *insn;

	for_each_insn(file, insn) {
		struct orc_entry *orc = &insn->orc;
		struct cfi_reg *cfa = &insn->state.cfa;
		struct cfi_reg *bp = &insn->state.regs[CFI_BP];

		if (cfa->base == CFI_UNDEFINED) {
			orc->sp_reg = ORC_REG_UNDEFINED;
			continue;
		}

		switch (cfa->base) {
		case CFI_SP:
			orc->sp_reg = ORC_REG_SP;
			break;
		case CFI_SP_INDIRECT:
			orc->sp_reg = ORC_REG_SP_INDIRECT;
			break;
		case CFI_BP:
			orc->sp_reg = ORC_REG_BP;
			break;
		case CFI_BP_INDIRECT:
			orc->sp_reg = ORC_REG_BP_INDIRECT;
			break;
		case CFI_R10:
			orc->sp_reg = ORC_REG_R10;
			break;
		case CFI_R13:
			orc->sp_reg = ORC_REG_R13;
			break;
		case CFI_DI:
			orc->sp_reg = ORC_REG_DI;
			break;
		case CFI_DX:
			orc->sp_reg = ORC_REG_DX;
			break;
		default:
			WARN_FUNC("unknown CFA base reg %d",
				  insn->sec, insn->offset, cfa->base);
			return -1;
		}

		switch(bp->base) {
		case CFI_UNDEFINED:
			orc->bp_reg = ORC_REG_UNDEFINED;
			break;
		case CFI_CFA:
			orc->bp_reg = ORC_REG_PREV_SP;
			break;
		case CFI_BP:
			orc->bp_reg = ORC_REG_BP;
			break;
		default:
			WARN_FUNC("unknown BP base reg %d",
				  insn->sec, insn->offset, bp->base);
			return -1;
		}

		orc->sp_offset = cfa->offset;
		orc->bp_offset = bp->offset;
		orc->type = insn->state.type;
	}

	return 0;
}

static int create_orc_entry(struct elf *elf, struct section *u_sec, struct section *ip_relasec,
				unsigned int idx, struct section *insn_sec,
				unsigned long insn_off, struct orc_entry *o)
{
	struct orc_entry *orc;
	struct rela *rela;

	if (!insn_sec->sym) {
		WARN("missing symbol for section %s", insn_sec->name);
		return -1;
	}

	/* populate ORC data */
	orc = (struct orc_entry *)u_sec->data->d_buf + idx;
	memcpy(orc, o, sizeof(*orc));

	/* populate rela for ip */
	rela = malloc(sizeof(*rela));
	if (!rela) {
		perror("malloc");
		return -1;
	}
	memset(rela, 0, sizeof(*rela));

	rela->sym = insn_sec->sym;
	rela->addend = insn_off;
	rela->type = R_X86_64_PC32;
	rela->offset = idx * sizeof(int);
	rela->rela_sec = ip_relasec;

	list_add_tail(&rela->list, &ip_relasec->rela_list);
	hash_add(elf->rela_hash, &rela->hash, rela_hash(rela));

	return 0;
}

int create_orc_sections(struct objtool_file *file)
{
	struct instruction *insn, *prev_insn;
	struct section *sec, *u_sec, *ip_relasec;
	unsigned int idx;

	struct orc_entry empty = {
		.sp_reg = ORC_REG_UNDEFINED,
		.bp_reg  = ORC_REG_UNDEFINED,
		.type    = ORC_TYPE_CALL,
	};

	sec = find_section_by_name(file->elf, ".orc_unwind");
	if (sec) {
		WARN("file already has .orc_unwind section, skipping");
		return -1;
	}

	/* count the number of needed orcs */
	idx = 0;
	for_each_sec(file, sec) {
		if (!sec->text)
			continue;

		prev_insn = NULL;
		sec_for_each_insn(file, sec, insn) {
			if (!prev_insn ||
			    memcmp(&insn->orc, &prev_insn->orc,
				   sizeof(struct orc_entry))) {
				idx++;
			}
			prev_insn = insn;
		}

		/* section terminator */
		if (prev_insn)
			idx++;
	}
	if (!idx)
		return -1;


	/* create .orc_unwind_ip and .rela.orc_unwind_ip sections */
	sec = elf_create_section(file->elf, ".orc_unwind_ip", sizeof(int), idx);
	if (!sec)
		return -1;

	ip_relasec = elf_create_rela_section(file->elf, sec);
	if (!ip_relasec)
		return -1;

	/* create .orc_unwind section */
	u_sec = elf_create_section(file->elf, ".orc_unwind",
				   sizeof(struct orc_entry), idx);

	/* populate sections */
	idx = 0;
	for_each_sec(file, sec) {
		if (!sec->text)
			continue;

		prev_insn = NULL;
		sec_for_each_insn(file, sec, insn) {
			if (!prev_insn || memcmp(&insn->orc, &prev_insn->orc,
						 sizeof(struct orc_entry))) {

				if (create_orc_entry(file->elf, u_sec, ip_relasec, idx,
						     insn->sec, insn->offset,
						     &insn->orc))
					return -1;

				idx++;
			}
			prev_insn = insn;
		}

		/* section terminator */
		if (prev_insn) {
			if (create_orc_entry(file->elf, u_sec, ip_relasec, idx,
					     prev_insn->sec,
					     prev_insn->offset + prev_insn->len,
					     &empty))
				return -1;

			idx++;
		}
	}

	if (elf_rebuild_rela_section(ip_relasec))
		return -1;

	return 0;
}
