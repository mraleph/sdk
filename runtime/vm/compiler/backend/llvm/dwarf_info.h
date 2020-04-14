#ifndef DWARF_INFO_H
#define DWARF_INFO_H

#include "vm/compiler/backend/llvm/llvm_config.h"
#include "vm/compiler/backend/llvm/llvm_log.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
// FIXME: move data view out.
#include <map>

#include "vm/compiler/backend/llvm/stack_maps.h"

namespace dart {
namespace dart_llvm {
class DwarfLineMapper {
 public:
  DwarfLineMapper();
  ~DwarfLineMapper();
  bool Process(const uint8_t* data);
  // offset, line map
  inline const std::map<unsigned, unsigned>& GetMap() const { return map_; }
  void Dump();

 private:
  bool ReadHeader();
  bool ProcessInstrs();
  bool ProcessExtendedLineOp();
  void ResetStateMachine(int is_stmt);
  void RecordLine(bool end_of_seqence);

  typedef struct State_Machine_Registers {
    unsigned long address;
    unsigned int view;
    unsigned int file;
    unsigned int line;
    unsigned int column;
    int is_stmt;
    int basic_block;
    unsigned char op_index;
    unsigned char end_sequence;
    /* This variable hold the number of the last entry seen
       in the File Table.  */
    unsigned int last_file_entry;
  } SMR;

  SMR state_machine_regs_;
  std::map<unsigned, unsigned> map_;
  const uint8_t* start_ = nullptr;
  uint64_t cu_length_ = 0;
  uint64_t prologue_length_ = 0;
  unsigned offset_length_ = 0;
  unsigned min_instr_len_ = 0;
  unsigned max_ops_per_insn_ = 0;
  int line_base_ = 0;
  unsigned line_range_ = 0;
  unsigned opcode_base_ = 0;
  unsigned instr_start_ = 0;
  unsigned instr_end_ = 0;
  unsigned offset_current_ = 0;

  uint16_t cu_version_ = 0;
  bool is_statment_ = false;
};
}  // namespace dart_llvm
}  // namespace dart

#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // DWARF_INFO_H
