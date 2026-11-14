/*
 * Sema/TypeLookup.h
 *
 * IR queries: enclosing callables, local declaration types, property resolution.
 */

#ifndef CXXE_SEMA_TYPE_LOOKUP_H
#define CXXE_SEMA_TYPE_LOOKUP_H

#include "Core/Common.h"
#include "Ir/Ir.h"

int resolve_property_for_member_ir(const IR *ir, size_t sep_tok, const char *receiver, const char *member, int64_t *out_id);

#endif /* CXXE_SEMA_TYPE_LOOKUP_H */
