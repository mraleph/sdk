#ifndef IR_TRANSLATOR_H
#define IR_TRANSLATOR_H
#include "vm/compiler/backend/llvm/llvm_headers.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include "vm/compiler/backend/il.h"

namespace dart {
class FlowGraph;
class BlockEntryInstr;
class BlockEntryWithInitialDefs;
namespace dart_llvm {

class IRTranslator : public FlowGraphVisitor {
 public:
  explicit IRTranslator(FlowGraph*);
  ~IRTranslator();
  void Translate();

 private:
  struct Impl;
  inline Impl& impl() { return *impl_; }

  void VisitBlockEntryWithInitialDefs(BlockEntryWithInitialDefs*);
  void VisitBlockEntry(BlockEntryInstr*);
#define DECLARE_VISIT_INSTRUCTION(ShortName, Attrs)                            \
  void Visit##ShortName(ShortName##Instr* instr) override;

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION
  DISALLOW_COPY_AND_ASSIGN(IRTranslator);
  std::unique_ptr<Impl> impl_;
};
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
#endif  // IR_TRANSLATOR_H
