/*
 * Ir/IrSerialize.h
 *
 * Binary CPIR (de)serialization.
 */

#ifndef CXXE_IR_IR_SERIALIZE_H
#define CXXE_IR_IR_SERIALIZE_H

#include "Core/Common.h"
#include "Ir/Ir.h"

#define CPIR_MAGIC "CPIR"
#define CPIR_VERSION 11u

void save_ir(const IR *ir, const char *path);
void load_ir(IR *ir, const char *path);

#endif /* CXXE_IR_IR_SERIALIZE_H */
