#ifndef KSYMS_H
#define KSYMS_H

#include "include/types.h"

/* Embedded kernel symbol table. Produced by tools/gen_ksyms.sh at
 * link time: stage1 of the kernel is linked without ksyms_data.o,
 * nm walks the resulting binary, and stage2 relinks with the C
 * source the tool emitted. Because .text precedes .rodata in
 * linker.ld, adding ksyms_data.o to the link only grows .rodata's
 * tail — function RIPs captured in the table stay valid.
 *
 * ksyms.c provides weak fallbacks for the three data symbols so
 * stage1 links cleanly; stage2's generated file overrides them
 * with the strong definitions. */

struct ksym { uint64_t addr; uint32_t name_off; };

extern const struct ksym ksyms[];
extern const uint32_t    ksym_count;
extern const char        ksym_strs[];

/* Look up the function whose start address is the greatest value
 * not exceeding `rip`. Returns the name (pointing into ksym_strs)
 * or NULL if rip is below the first symbol. If offset_out is
 * non-NULL, writes (rip - symbol_addr) into it. */
const char *ksym_lookup(uint64_t rip, uint64_t *offset_out);

#endif
