/*
 * Parser/Parser.h
 *
 * Public parser entry point: tokens -> CPIR tree.
 */

#ifndef CXXE_PARSER_PARSER_H
#define CXXE_PARSER_PARSER_H

#include "Core/Common.h"
#include "Ir/Ir.h"

void parse_source(IR *ir);

#endif /* CXXE_PARSER_PARSER_H */
