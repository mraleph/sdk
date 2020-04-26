#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <unordered_map>

#include "vm/compiler/aot/precompiler.h"
#include "vm/compiler/assembler/object_pool_builder.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/initialize_llvm.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#include "vm/compiler/backend/llvm/output.h"
#include "vm/compiler/backend/llvm/target_specific.h"
#include "vm/compiler/runtime_api.h"

namespace dart {
namespace dart_llvm {
namespace {
class AnonImpl;
struct NotMergedPhiDesc {
  BlockEntryInstr* pred;
  int value;
  LValue phi;
};

struct NotMergedArgDesc {
  BlockEntryInstr* pred;
  size_t index;
  LValue phi;
};

struct GCRelocateDesc {
  int value;
  int where;
  GCRelocateDesc(int v, int w) : value(v), where(w) {}
};

using GCRelocateDescList = std::vector<GCRelocateDesc>;

enum class ValueType { LLVMValue, RelativeCallTarget };

struct ValueDesc {
  ValueType type;
  Definition* def;
  union {
    LValue llvm_value;
    int64_t relative_call_target;
  };
};

struct IRTranslatorBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::vector<NotMergedArgDesc> not_merged_args;
  std::unordered_map<int, ValueDesc> values_;
  // values for exception block.
  std::unordered_map<int, ValueDesc> exception_values_;
  // Call supports
  // ssa values pushed by PushArgumentInstr
  std::vector<LValue> arguments_pushed_;

  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  // exception vals
  LValue landing_pad = nullptr;
  LValue exception_val = nullptr;
  LValue stacktrace_val = nullptr;
  AnonImpl* anon_impl;
  int try_index = kInvalidTryIndex;

  bool started = false;
  bool ended = false;
  bool need_merge = false;
  bool exception_block = false;

  inline void SetLLVMValue(int ssa_id, LValue value) {
    values_[ssa_id] = {ValueType::LLVMValue, nullptr, {value}};
  }

  inline void SetLLVMValue(Definition* d, LValue value) {
    values_[d->ssa_temp_index()] = {ValueType::LLVMValue, nullptr, {value}};
  }

  inline void SetValue(int nid, const ValueDesc& value) {
    values_[nid] = value;
  }

  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }

  bool IsDefined(Definition* def) {
    auto found = values_.find(def->ssa_temp_index());
    return found != values_.end();
  }

#if 0
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
#endif

  const ValueDesc& GetValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
  }

  const ValueDesc& GetValue(Value* v) const {
    return GetValue(v->definition()->ssa_temp_index());
  }

  inline std::unordered_map<int, ValueDesc>& values() { return values_; }

  inline void StartTranslate() {
    EMASSERT(!started);
    EMASSERT(!ended);
    started = true;
  }

  inline void EndTranslate() {
    EMASSERT(started);
    EMASSERT(!ended);
    ended = true;
  }
};

class AnonImpl {
 public:
  LBasicBlock GetNativeBB(BlockEntryInstr* bb);
  LBasicBlock EnsureNativeBB(BlockEntryInstr* bb);
  IRTranslatorBlockImpl* GetTranslatorBlockImpl(BlockEntryInstr* bb);
  LBasicBlock GetNativeBBContinuation(BlockEntryInstr* bb);
  bool IsBBStartedToTranslate(BlockEntryInstr* bb);
  bool IsBBEndedToTranslate(BlockEntryInstr* bb);
  void StartTranslate(BlockEntryInstr* bb);
  void MergePredecessors(BlockEntryInstr* bb);
  bool AllPredecessorStarted(BlockEntryInstr* bb, BlockEntryInstr** ref_pred);
  void End();
  void EndCurrentBlock();
  void ProcessPhiWorkList();
  void ProcessArgWorkList();
  void BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                 BlockEntryInstr* ref_pred);
  void BuildPushedArgumentsPhis(BlockEntryInstr* bb, BlockEntryInstr* ref_pred);
  // MaterializeDef
  void MaterializeDef(ValueDesc& desc);
  // Phis
  LValue EnsurePhiInput(BlockEntryInstr* pred, int index, LType type);
  LValue EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                   int index,
                                   LType type);
  // Exception handling
  LBasicBlock EnsureNativeCatchBlock(int try_index);
  void InitCatchBlock(CatchBlockEntryInstr* instr);
  // Debug & line info.
  void SetDebugLine(Instruction*);
  // Access to memory.
  LValue BuildAccessPointer(LValue base, LValue offset, Representation rep);
  LValue BuildAccessPointer(LValue base, LValue offset, LType type);
  // Calls
  void CheckPushArguments(BlockEntryInstr*);
  // Types
  LType GetMachineRepresentationType(Representation);
  LValue EnsureBoolean(LValue v);
  LValue EnsureIntPtr(LValue v);
  LValue EnsureInt32(LValue v);
  LValue SmiTag(LValue v);
  LValue SmiUntag(LValue v);
  LValue TstSmi(LValue v);

  // Value helpers for current bb
  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
  void SetLLVMValue(int ssa_id, LValue v);
  void SetLLVMValue(Definition* d, LValue v);
  void SetLazyValue(Definition*);

  // Load Store
  compiler::ObjectPoolBuilder& object_pool_builder() {
    return *precompiler_->global_object_pool_builder();
  }
  LValue LoadFieldFromOffset(LValue base, int offset, LType type);
  LValue LoadFieldFromOffset(LValue base, int offset);
  LValue LoadFromOffset(LValue base, int offset, LType type);
  LValue LoadObject(const Object& object, bool is_unique = false);

  void StoreToOffset(LValue base, int offset, LValue v);
  void StoreToOffset(LValue base, LValue offset, LValue v);

  // Basic blocks continuation
  LBasicBlock NewBasicBlock(const char* fmt, ...);

  inline CompilerState& compiler_state() { return *compiler_state_; }
  inline LivenessAnalysis& liveness() { return *liveness_analysis_; }
  inline Output& output() { return *output_; }
  inline IRTranslatorBlockImpl& current_bb_impl() { return *current_bb_impl_; }
  inline BlockEntryInstr* current_bb() { return current_bb_; }

  BlockEntryInstr* current_bb_;
  IRTranslatorBlockImpl* current_bb_impl_;
  FlowGraph* flow_graph_;
  Precompiler* precompiler_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
  std::unique_ptr<Output> output_;
  std::unordered_map<BlockEntryInstr*, IRTranslatorBlockImpl> block_map_;
  std::vector<BlockEntryInstr*> phi_rebuild_worklist_;
  std::vector<BlockEntryInstr*> arguments_pushed_rebuild_worklist_;
  std::vector<LBasicBlock> catch_blocks_;

  // debug lines
  std::vector<Instruction*> debug_instrs_;
  // Calls
};

class ContinuationResolver {
 protected:
  explicit ContinuationResolver(AnonImpl& impl);
  ~ContinuationResolver() = default;
  void CreateContination();
  inline AnonImpl& impl() { return impl_; }
  inline Output& output() { return impl().output(); }
  inline BlockEntryInstr* current_bb() { return impl().current_bb_; }
  inline int id() { return current_bb()->block_id(); }

 private:
  AnonImpl& impl_;
};

class DiamondContinuationResolver : public ContinuationResolver {
 public:
  explicit DiamondContinuationResolver(AnonImpl& impl, int ssa_id);
  ~DiamondContinuationResolver() = default;
  DiamondContinuationResolver& BuildCmp(std::function<LValue()>);
  DiamondContinuationResolver& BuildLeft(std::function<LValue()>);
  DiamondContinuationResolver& BuildRight(std::function<LValue()>);
  LValue End();

 private:
  int ssa_id_;
  LBasicBlock blocks_[3];
  LValue values_[2];
};

LValue IRTranslatorBlockImpl::GetLLVMValue(int ssa_id) {
  auto found = values_.find(ssa_id);
  EMASSERT(found != values_.end());
  EMASSERT(found->second.type == ValueType::LLVMValue);
  if (!found->second.llvm_value) {
    // materialize the llvm_value now!
    anon_impl->MaterializeDef(found->second);
  }
  EMASSERT(found->second.llvm_value);
  return found->second.llvm_value;
}

IRTranslatorBlockImpl* AnonImpl::GetTranslatorBlockImpl(BlockEntryInstr* bb) {
  auto& ref = block_map_[bb];
  return &ref;
}

LBasicBlock AnonImpl::EnsureNativeBB(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* impl = GetTranslatorBlockImpl(bb);
  if (impl->native_bb) return impl->native_bb;
  char buf[256];
  snprintf(buf, 256, "B%d", static_cast<int>(bb->block_id()));
  LBasicBlock native_bb = output_->appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
  return native_bb;
}

LBasicBlock AnonImpl::GetNativeBB(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->native_bb;
}

LBasicBlock AnonImpl::GetNativeBBContinuation(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->continuation;
}

bool AnonImpl::IsBBStartedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->started;
}

bool AnonImpl::IsBBEndedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->ended;
}

void AnonImpl::StartTranslate(BlockEntryInstr* bb) {
  current_bb_ = bb;
  current_bb_impl_ = GetTranslatorBlockImpl(bb);
  current_bb_impl().StartTranslate();
  EnsureNativeBB(bb);
  output_->positionToBBEnd(GetNativeBB(bb));
}

void AnonImpl::MergePredecessors(BlockEntryInstr* bb) {
  intptr_t predecessor_count = bb->PredecessorCount();
  if (predecessor_count == 0) return;
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  if (predecessor_count == 1) {
    // Don't use phi if only one predecessor.
    BlockEntryInstr* pred = bb->PredecessorAt(0);
    IRTranslatorBlockImpl* pred_block_impl = GetTranslatorBlockImpl(pred);
    EMASSERT(IsBBStartedToTranslate(pred));
    for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
         it.Advance()) {
      int live = it.Current();
      auto& value = pred_block_impl->GetValue(live);
      block_impl->SetValue(live, value);
    }
    // copy arguments_pushed_
    block_impl->arguments_pushed_ = pred_block_impl->arguments_pushed_;
    // FIXME: need to build landing pad after invoke.
    // The following should only merge the value with phi.
    if (block_impl->exception_block) {
#if 0
      LValue landing_pad = output().buildLandingPad();
      LValue call_value = landing_pad;
      PredecessorCallInfo& callinfo = block_impl->call_info;
      auto& values = block_impl->values();
      for (auto& gc_relocate : callinfo.gc_relocates) {
        auto found = values.find(gc_relocate.value);
        EMASSERT(found->second.type == ValueType::LLVMValue);
        LValue relocated = output().buildCall(
            output().repo().gcRelocateIntrinsic(), call_value,
            output().constInt32(gc_relocate.where),
            output().constInt32(gc_relocate.where));
        found->second.llvm_value = relocated;
      }
      block_impl->landing_pad = landing_pad;
#endif
    }
    return;
  }
  BlockEntryInstr* ref_pred = nullptr;
  if (!AllPredecessorStarted(bb, &ref_pred)) {
    EMASSERT(!!ref_pred);
    BuildPhiAndPushToWorkList(bb, ref_pred);
    BuildPushedArgumentsPhis(bb, ref_pred);
    return;
  }
  // FIXME: merge the following into BuildPhiAndPushToWorkList
  // Use phi.
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    auto& value = GetTranslatorBlockImpl(ref_pred)->GetValue(live);
    if (value.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value);
      continue;
    }
    if (value.def != nullptr) {
      ValueDesc new_value_desc = value;
      // reset the constant.
      new_value_desc.llvm_value = nullptr;
      block_impl->SetValue(live, new_value_desc);
      continue;
    }
    LValue ref_value = value.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      // FIXME: Should add EMASSERT that all values are the same.
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
    block_impl->SetLLVMValue(live, phi);
  }
  BuildPushedArgumentsPhis(bb, ref_pred);
}

bool AnonImpl::AllPredecessorStarted(BlockEntryInstr* bb,
                                     BlockEntryInstr** ref_pred) {
  bool ret_value = true;
  intptr_t predecessor_count = bb->PredecessorCount();
  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = bb->PredecessorAt(i);
    if (IsBBStartedToTranslate(pred)) {
      if (!*ref_pred) *ref_pred = pred;
    } else {
      ret_value = false;
    }
  }
  return ret_value;
}

void AnonImpl::BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                         BlockEntryInstr* ref_pred) {
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  IRTranslatorBlockImpl* ref_pred_impl = GetTranslatorBlockImpl(ref_pred);
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    const ValueDesc& value_desc = ref_pred_impl->GetValue(live);
    if (value_desc.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value_desc);
      continue;
    }
    if (value_desc.def != nullptr) {
      ValueDesc new_value_desc = value_desc;
      // reset the constant.
      new_value_desc.llvm_value = nullptr;
      block_impl->SetValue(live, new_value_desc);
      continue;
    }
    LValue ref_value = value_desc.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    block_impl->SetLLVMValue(live, phi);
    intptr_t predecessor_count = bb->PredecessorCount();
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      if (!IsBBStartedToTranslate(pred)) {
        block_impl->not_merged_phis.emplace_back();
        NotMergedPhiDesc& not_merged_phi = block_impl->not_merged_phis.back();
        not_merged_phi.phi = phi;
        not_merged_phi.value = live;
        not_merged_phi.pred = pred;
        continue;
      }
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
  }
  phi_rebuild_worklist_.push_back(bb);
}

void AnonImpl::BuildPushedArgumentsPhis(BlockEntryInstr* bb,
                                        BlockEntryInstr* ref_pred) {
  // pushed arguments to phi
  EMASSERT(IsBBStartedToTranslate(ref_pred));
  IRTranslatorBlockImpl* ref_pred_impl = GetTranslatorBlockImpl(ref_pred);
  if (ref_pred_impl->arguments_pushed_.empty())
    return;

  std::vector<LValue> work_list;
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  for (auto value : ref_pred_impl->arguments_pushed_) {
    LType type = typeOf(value);
    LValue phi = output().buildPhi(type);
    addIncoming(phi, &value, &ref_pred_impl->continuation, 1);
    work_list.emplace_back(phi);
  }
  int predecessor_count = bb->PredecessorCount();
  bool need_to_push = false;
  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = bb->PredecessorAt(i);
    // ref_pred has been handled.
    if (pred == ref_pred) continue;
    if (IsBBStartedToTranslate(pred)) {
      IRTranslatorBlockImpl* pred_impl = GetTranslatorBlockImpl(pred);
      EMASSERT(pred_impl->arguments_pushed_.size() == work_list.size());
      for (size_t i = 0; i != pred_impl->arguments_pushed_.size(); ++i) {
        LValue value = pred_impl->arguments_pushed_[i];
        EMASSERT(typeOf(value) == typeOf(work_list[i]));
        addIncoming(work_list[i], &value, &pred_impl->continuation, 1);
      }
    } else {
      need_to_push = true;
      for (size_t i = 0; i < work_list.size(); ++i) {
        block_impl->not_merged_args.emplace_back();
        LValue phi = work_list[i];
        NotMergedArgDesc& not_merged_arg = block_impl->not_merged_args.back();
        not_merged_arg.phi = phi;
        not_merged_arg.index = i;
        not_merged_arg.pred = pred;
      }
      continue;
    }
  }
  if (need_to_push) arguments_pushed_rebuild_worklist_.push_back(bb);
  // rebuild the arguments_pushed_ list for bb
  for (LValue phi : work_list) {
    block_impl->arguments_pushed_.emplace_back(phi);
  }
}

void AnonImpl::End() {
  EMASSERT(!!current_bb_);
  EndCurrentBlock();
  ProcessPhiWorkList();
  ProcessArgWorkList();
  output().positionToBBEnd(output().prologue());
  output().buildBr(GetNativeBB(flow_graph_->graph_entry()));
  output().finalize();
}

void AnonImpl::EndCurrentBlock() {
  GetTranslatorBlockImpl(current_bb_)->EndTranslate();
  current_bb_ = nullptr;
  current_bb_impl_ = nullptr;
}

void AnonImpl::ProcessPhiWorkList() {
  for (BlockEntryInstr* bb : phi_rebuild_worklist_) {
    auto impl = GetTranslatorBlockImpl(bb);
    for (auto& e : impl->not_merged_phis) {
      BlockEntryInstr* pred = e.pred;
      EMASSERT(IsBBStartedToTranslate(pred));
      LValue value = EnsurePhiInput(pred, e.value, typeOf(e.phi));
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(e.phi, &value, &native, 1);
    }
    impl->not_merged_phis.clear();
  }
  phi_rebuild_worklist_.clear();
}

void AnonImpl::ProcessArgWorkList() {
  for (BlockEntryInstr* bb : arguments_pushed_rebuild_worklist_) {
    auto impl = GetTranslatorBlockImpl(bb);
    for (auto& e : impl->not_merged_args) {
      BlockEntryInstr* pred = e.pred;
      IRTranslatorBlockImpl* pred_block_impl = GetTranslatorBlockImpl(pred);
      EMASSERT(IsBBStartedToTranslate(pred));
      EMASSERT(pred_block_impl->arguments_pushed_.size() > e.index);
      LValue value = pred_block_impl->arguments_pushed_[e.index];
      EMASSERT(typeOf(e.phi) == typeOf(value));
      LBasicBlock native = pred_block_impl->continuation;
      addIncoming(e.phi, &value, &native, 1);
    }
    impl->not_merged_args.clear();
  }
  arguments_pushed_rebuild_worklist_.clear();
}

LValue AnonImpl::EnsurePhiInput(BlockEntryInstr* pred, int index, LType type) {
  LValue val = GetTranslatorBlockImpl(pred)->GetLLVMValue(index);
  LType value_type = typeOf(val);
  if (value_type == type) return val;
  LValue terminator =
      LLVMGetBasicBlockTerminator(GetNativeBBContinuation(pred));
  if ((value_type == output().repo().intPtr) &&
      (type == output().tagged_type())) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMIntToPtr, val, output().tagged_type());
    return ret_val;
  }
  LLVMTypeKind value_type_kind = LLVMGetTypeKind(value_type);
  if ((LLVMPointerTypeKind == value_type_kind) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMPtrToInt, val, output().repo().intPtr);
    return ret_val;
  }
  if ((value_type == output().repo().boolean) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val = output().buildCast(LLVMZExt, val, output().repo().intPtr);
    return ret_val;
  }
  LLVMTypeKind type_kind = LLVMGetTypeKind(type);
  if ((LLVMIntegerTypeKind == value_type_kind) &&
      (value_type_kind == type_kind)) {
    // handle both integer
    output().positionBefore(terminator);
    LValue ret_val;
    if (LLVMGetIntTypeWidth(value_type) > LLVMGetIntTypeWidth(type)) {
      ret_val = output().buildCast(LLVMTrunc, val, type);
    } else {
      ret_val = output().buildCast(LLVMZExt, val, type);
    }
    return ret_val;
  }
  __builtin_trap();
}

LValue AnonImpl::EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                           int index,
                                           LType type) {
  LValue value = EnsurePhiInput(pred, index, type);
  output().positionToBBEnd(GetNativeBB(current_bb_));
  return value;
}

void AnonImpl::MaterializeDef(ValueDesc& desc) {
  EMASSERT(desc.def != nullptr);
  Definition* def = desc.def;
  LValue v;
  if (UnboxedConstantInstr* unboxed = def->AsUnboxedConstant()) {
    const Object& object = unboxed->value();
    if (object.IsDouble()) {
      const Double& n = Double::Cast(object);
      v = output().constDouble(n.value());
    } else if (object.IsSmi()) {
      v = output().constTagged(compiler::target::ToRawSmi(object));
    } else {
      LLVMLOGE(
          "MaterializeDef: UnboxedConstantInstr failed to interpret: unknown "
          "type: %s",
          object.ToCString());
      UNREACHABLE();
    }
  } else if (ConstantInstr* constant_object = def->AsConstant()) {
    v = LoadObject(constant_object->value());
  } else {
    LLVMLOGE("MaterializeDef: unknown def: %s\n", def->ToCString());
    UNREACHABLE();
  }
  desc.llvm_value = v;
}

LBasicBlock AnonImpl::EnsureNativeCatchBlock(int try_index) {
  char buf[256];
  LBasicBlock native_bb;
  if ((native_bb = catch_blocks_[try_index]) == nullptr) {
    snprintf(buf, 256, "B_catch_block_%d", try_index);
    native_bb = output().appendBasicBlock(buf);
    catch_blocks_[try_index] = native_bb;
  }
  return native_bb;
}

void AnonImpl::InitCatchBlock(CatchBlockEntryInstr* instr) {
  LBasicBlock native_bb = EnsureNativeCatchBlock(instr->catch_try_index());
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(instr);
  block_impl->native_bb = native_bb;
  block_impl->exception_block = true;
  output_->positionToBBEnd(native_bb);
}

void AnonImpl::SetDebugLine(Instruction* instr) {
  debug_instrs_.emplace_back(instr);
  output().setDebugInfo(debug_instrs_.size(), nullptr);
}

LValue AnonImpl::BuildAccessPointer(LValue base,
                                    LValue offset,
                                    Representation rep) {
  return BuildAccessPointer(base, offset,
                            pointerType(GetMachineRepresentationType(rep)));
}

LValue AnonImpl::BuildAccessPointer(LValue base, LValue offset, LType type) {
  LLVMTypeKind kind = LLVMGetTypeKind(typeOf(base));
  if (kind == LLVMIntegerTypeKind) {
    base = output().buildCast(LLVMIntToPtr, base, output().repo().ref8);
  }
  // For ElementOffsetFromIndex ignores BitcastTaggedToWord.
  if (typeOf(offset) == output().tagged_type()) {
    UNREACHABLE();
    // offset = output().buildCast(LLVMPtrToInt, offset, output().repo().intPtr);
  }
  LValue pointer = output().buildGEPWithByteOffset(base, offset, type);
  return pointer;
}

LType AnonImpl::GetMachineRepresentationType(Representation rep) {
  LType type;
  switch (rep) {
    case kTagged:
      type = output().tagged_type();
      break;
    case kUnboxedDouble:
      type = output().repo().doubleType;
      break;
    case kUnboxedFloat:
      type = output().repo().floatType;
      break;
    case kUnboxedInt32:
    case kUnboxedUint32:
      type = output().repo().int32;
      break;
    case kUnboxedInt64:
      type = output().repo().int64;
      break;
    default:
      UNREACHABLE();
  }
  return type;
}

LValue AnonImpl::EnsureBoolean(LValue v) {
  LType type = typeOf(v);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind)
    v = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  type = typeOf(v);
  if (LLVMGetIntTypeWidth(type) == 1) return v;
  v = output().buildICmp(LLVMIntNE, v, output().repo().intPtrZero);
  return v;
}

LValue AnonImpl::EnsureIntPtr(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().intPtr) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind)
    return output().buildCast(LLVMZExt, v, output().repo().intPtr);
  return v;
}

LValue AnonImpl::EnsureInt32(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().int32) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind) {
    return output().buildCast(LLVMPtrToInt, v, output().repo().int32);
  }
  if (type == output().repo().boolean) {
    return output().buildCast(LLVMZExt, v, output().repo().int32);
  }
  EMASSERT(type == output().repo().int32);
  return v;
}

LValue AnonImpl::SmiTag(LValue v) {
  EMASSERT(typeOf(v) == output().repo().intPtr);
  v = output().buildShl(v, output().constIntPtr(kSmiTagSize));
  return output().buildCast(LLVMIntToPtr, v, output().tagged_type());
}

LValue AnonImpl::SmiUntag(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  v = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  return output().buildShr(v, output().constIntPtr(kSmiTagSize));
}

LValue AnonImpl::TstSmi(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  LValue address = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  LValue and_value =
      output().buildAnd(address, output().constIntPtr(kSmiTagMask));
  return output().buildICmp(LLVMIntNE, and_value,
                            output().constIntPtr(kSmiTagMask));
}

void AnonImpl::CheckPushArguments(BlockEntryInstr* block) {
  // Must dominate
  if (!current_bb_impl().arguments_pushed_.empty()) {
    EMASSERT(current_bb_->Dominates(block));
    EMASSERT(current_bb_->postorder_number() > block->postorder_number());
  }
}

LValue AnonImpl::GetLLVMValue(int ssa_id) {
  return current_bb_impl().GetLLVMValue(ssa_id);
}

void AnonImpl::SetLLVMValue(int ssa_id, LValue val) {
  current_bb_impl().SetLLVMValue(ssa_id, val);
}

void AnonImpl::SetLLVMValue(Definition* d, LValue val) {
  current_bb_impl().SetLLVMValue(d, val);
}

void AnonImpl::SetLazyValue(Definition* d) {
  ValueDesc desc = {ValueType::LLVMValue, d, {nullptr}};
  current_bb_impl().SetValue(d->ssa_temp_index(), desc);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset, LType type) {
  LValue gep = output().buildGEPWithByteOffset(
      base, output().constIntPtr(offset - kHeapObjectTag), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset) {
  return LoadFieldFromOffset(base, offset, pointerType(output().tagged_type()));
}

LValue AnonImpl::LoadFromOffset(LValue base, int offset, LType type) {
  LValue gep =
      output().buildGEPWithByteOffset(base, output().constIntPtr(offset), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadObject(const Object& object, bool is_unique) {
  intptr_t offset = 0;
  if (compiler::target::CanLoadFromThread(object, &offset)) {
    // Load common VM constants from the thread. This works also in places where
    // no constant pool is set up (e.g. intrinsic code).
    LValue gep = output().buildGEPWithByteOffset(
        output().thread(), output().constIntPtr(offset),
        pointerType(output().tagged_type()));
    return output().buildLoad(gep);
  } else if (compiler::target::IsSmi(object)) {
    // Relocation doesn't apply to Smis.
    return output().constIntPtr(
        static_cast<intptr_t>(compiler::target::ToRawSmi(object)));
  } else {
    // Make sure that class CallPattern is able to decode this load from the
    // object pool.
    EMASSERT(FLAG_use_bare_instructions);
    const auto index = is_unique ? object_pool_builder().AddObject(object)
                                 : object_pool_builder().FindObject(object);
    const int32_t offset = compiler::target::ObjectPool::element_offset(index);
    LValue gep = output().buildGEPWithByteOffset(
        output().pp(), output().constIntPtr(offset - kHeapObjectTag),
        pointerType(output().tagged_type()));
    return output().buildLoad(gep);
  }
}

void AnonImpl::StoreToOffset(LValue base, int offset, LValue v) {
  StoreToOffset(base, output().constIntPtr(offset), v);
}

void AnonImpl::StoreToOffset(LValue base, LValue offset, LValue v) {
  LValue gep =
      output().buildGEPWithByteOffset(base, offset, pointerType(typeOf(v)));
  output().buildStore(v, gep);
}

LBasicBlock AnonImpl::NewBasicBlock(const char* fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char buf[256];
  vsnprintf(buf, 256, fmt, va);
  va_end(va);
  LBasicBlock bb = output().appendBasicBlock(buf);
  return bb;
}

ContinuationResolver::ContinuationResolver(AnonImpl& impl) : impl_(impl) {}

DiamondContinuationResolver::DiamondContinuationResolver(AnonImpl& _impl,
                                                         int ssa_id)
    : ContinuationResolver(_impl), ssa_id_(ssa_id) {
  blocks_[0] = impl().NewBasicBlock("left_for_%d", ssa_id);
  blocks_[1] = impl().NewBasicBlock("right_for_%d", ssa_id);
  blocks_[2] = impl().NewBasicBlock("continuation_for_%d", ssa_id);
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildCmp(
    std::function<LValue()> f) {
  LValue cmp_value = f();
  output().buildCondBr(cmp_value, blocks_[0], blocks_[1]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildLeft(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[0]);
  values_[0] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildRight(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[1]);
  values_[1] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

LValue DiamondContinuationResolver::End() {
  output().positionToBBEnd(blocks_[2]);
  LValue phi = output().buildPhi(typeOf(values_[0]));
  addIncoming(phi, values_, blocks_, 2);
  impl().current_bb_impl().continuation = blocks_[2];
  return phi;
}
}  // namespace

struct IRTranslator::Impl : public AnonImpl {};

IRTranslator::IRTranslator(FlowGraph* flow_graph, Precompiler* precompiler)
    : FlowGraphVisitor(flow_graph->reverse_postorder()) {
  InitLLVM();
  impl_.reset(new Impl);
  impl().flow_graph_ = flow_graph;
  impl().precompiler_ = precompiler;
  impl().compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function().function().UserVisibleName())
          .ToCString()));
  impl().liveness_analysis_.reset(new LivenessAnalysis(flow_graph));
  impl().output_.reset(new Output(impl().compiler_state()));
  // init parameter desc
  RegisterParameterDesc parameter_desc;
  LType tagged_type = output().repo().tagged_type;
  for (int i = flow_graph->num_direct_parameters(); i > 0; --i) {
    parameter_desc.emplace_back(-i, tagged_type);
  }
  // init output().
  output().initializeBuild(parameter_desc);
}

IRTranslator::~IRTranslator() {}

void IRTranslator::Translate() {
#if 0
  impl().liveness().Analyze();
  VisitBlocks();
#endif

  FlowGraphPrinter::PrintGraph("IRTranslator", impl().flow_graph_);
}

Output& IRTranslator::output() {
  return impl().output();
}

void IRTranslator::VisitBlockEntry(BlockEntryInstr* block) {
  impl().SetDebugLine(block);
  impl().StartTranslate(block);
  IRTranslatorBlockImpl* block_impl = impl().GetTranslatorBlockImpl(block);
  block_impl->try_index = block->try_index();
  impl().MergePredecessors(block);
}

void IRTranslator::VisitBlockEntryWithInitialDefs(
    BlockEntryWithInitialDefs* block) {
  VisitBlockEntry(block);
  for (Definition* def : *block->initial_definitions()) {
    def->Accept(this);
  }
}

void IRTranslator::VisitGraphEntry(GraphEntryInstr* instr) {
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitJoinEntry(JoinEntryInstr* instr) {
  VisitBlockEntry(instr);
  for (PhiInstr* phi : *instr->phis()) {
    VisitPhi(phi);
  }
}

void IRTranslator::VisitTargetEntry(TargetEntryInstr* instr) {
  VisitBlockEntry(instr);
}

void IRTranslator::VisitFunctionEntry(FunctionEntryInstr* instr) {
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitNativeEntry(NativeEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOsrEntry(OsrEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitIndirectEntry(IndirectEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCatchBlockEntry(CatchBlockEntryInstr* instr) {
  impl().InitCatchBlock(instr);
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitPhi(PhiInstr* instr) {
  impl().SetDebugLine(instr);
  LType phi_type = impl().GetMachineRepresentationType(instr->representation());

  LValue phi = output().buildPhi(phi_type);
  bool should_add_to_tf_phi_worklist = false;
  intptr_t predecessor_count = impl().current_bb_->PredecessorCount();
  IRTranslatorBlockImpl& block_impl = impl().current_bb_impl();

  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = impl().current_bb()->PredecessorAt(i);
    int ssa_value = instr->InputAt(i)->definition()->ssa_temp_index();
    if (impl().IsBBStartedToTranslate(pred)) {
      LValue value =
          impl().EnsurePhiInputAndPosition(pred, ssa_value, phi_type);
      LBasicBlock llvmbb = impl().GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &llvmbb, 1);
    } else {
      should_add_to_tf_phi_worklist = true;
      block_impl.not_merged_phis.emplace_back();
      auto& not_merged_phi = block_impl.not_merged_phis.back();
      not_merged_phi.phi = phi;
      not_merged_phi.pred = pred;
      not_merged_phi.value = ssa_value;
    }
  }
  if (should_add_to_tf_phi_worklist)
    impl().phi_rebuild_worklist_.push_back(impl().current_bb_);
  block_impl.SetLLVMValue(instr->ssa_temp_index(), phi);
}

void IRTranslator::VisitRedefinition(RedefinitionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitParameter(ParameterInstr* instr) {
  int param_index = instr->index();
  // only stack parameters
  output().parameter(kV8CCRegisterParameterCount + param_index);
}

void IRTranslator::VisitNativeParameter(NativeParameterInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexedUnsafe(LoadIndexedUnsafeInstr* instr) {
  EMASSERT(instr->RequiredInputRepresentation(0) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue access =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());
  LValue value = output().buildLoad(access);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexedUnsafe(StoreIndexedUnsafeInstr* instr) {
  ASSERT(instr->RequiredInputRepresentation(
             StoreIndexedUnsafeInstr::kIndexPos) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue gep =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());

  LValue value = impl().GetLLVMValue(instr->value());
  output().buildStore(value, gep);
}

void IRTranslator::VisitTailCall(TailCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitParallelMove(ParallelMoveInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitPushArgument(PushArgumentInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  impl().current_bb_impl().arguments_pushed_.emplace_back(value);
}

void IRTranslator::VisitReturn(ReturnInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  output().buildRet(value);
}

void IRTranslator::VisitNativeReturn(NativeReturnInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitThrow(ThrowInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitReThrow(ReThrowInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStop(StopInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGoto(GotoInstr* instr) {
  impl().SetDebugLine(instr);
  JoinEntryInstr* successor = instr->successor();
  impl().CheckPushArguments(successor);
  LBasicBlock bb = impl().EnsureNativeBB(successor);
  output().buildBr(bb);
  impl().EndCurrentBlock();
}

void IRTranslator::VisitIndirectGoto(IndirectGotoInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBranch(BranchInstr* instr) {
  impl().SetDebugLine(instr);
  TargetEntryInstr* true_successor = instr->true_successor();
  TargetEntryInstr* false_successor = instr->false_successor();
  impl().CheckPushArguments(true_successor);
  impl().CheckPushArguments(false_successor);
  impl().EnsureNativeBB(true_successor);
  impl().EnsureNativeBB(false_successor);
  LValue cmp_val = impl().GetLLVMValue(instr->comparison());
  cmp_val = impl().EnsureBoolean(cmp_val);
  output().buildCondBr(cmp_val, impl().GetNativeBB(true_successor),
                       impl().GetNativeBB(false_successor));
  impl().EndCurrentBlock();
}

// FIXME: implement assertions.
void IRTranslator::VisitAssertAssignable(AssertAssignableInstr* instr) {}

void IRTranslator::VisitAssertSubtype(AssertSubtypeInstr* instr) {}

void IRTranslator::VisitAssertBoolean(AssertBooleanInstr* instr) {}

void IRTranslator::VisitSpecialParameter(SpecialParameterInstr* instr) {
  LValue val = nullptr;
  auto kind = instr->kind();
  if (kind == SpecialParameterInstr::kArgDescriptor) {
    val = output().args_desc();
  } else {
    EMASSERT(impl().current_bb_impl().exception_block);
    switch (kind) {
      case SpecialParameterInstr::kException:
        val = impl().current_bb_impl().exception_val;
        break;
      case SpecialParameterInstr::kStackTrace:
        val = impl().current_bb_impl().stacktrace_val;
        break;
      default:
        UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitClosureCall(ClosureCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitFfiCall(FfiCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInstanceCall(InstanceCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStaticCall(StaticCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadLocal(LoadLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDropTemps(DropTempsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitMakeTemp(MakeTempInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStoreLocal(StoreLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStrictCompare(StrictCompareInstr* instr) {
  impl().SetDebugLine(instr);
  if (instr->needs_number_check()) {
    // FIXME: do it after call supports
    UNREACHABLE();
  }
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  LValue result = output().buildICmp(
      instr->kind() == Token::kEQ_STRICT ? LLVMIntEQ : LLVMIntNE, left, right);
  impl().SetLLVMValue(instr, result);
}

static LLVMIntPredicate TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMIntEQ;
    case Token::kNE:
      return LLVMIntNE;
    case Token::kLT:
      return LLVMIntSLT;
    case Token::kGT:
      return LLVMIntSGT;
    case Token::kLTE:
      return LLVMIntSLE;
    case Token::kGTE:
      return LLVMIntSGE;
    default:
      UNREACHABLE();
  }
}

static LLVMRealPredicate TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMRealOEQ;
    case Token::kNE:
      return LLVMRealONE;
    case Token::kLT:
      return LLVMRealOLT;
    case Token::kGT:
      return LLVMRealOGT;
    case Token::kLTE:
      return LLVMRealOLE;
    case Token::kGTE:
      return LLVMRealOGE;
    default:
      UNREACHABLE();
  }
}

void IRTranslator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  impl().SetDebugLine(instr);
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitRelationalOp(RelationalOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitNativeCall(NativeCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDebugStepCheck(DebugStepCheckInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());

  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue gep =
      impl().BuildAccessPointer(array, offset_value, instr->representation());
  LValue val = output().buildLoad(gep);
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitLoadCodeUnits(LoadCodeUnitsInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue value;
#if !defined(TARGET_ARCH_IS_64_BIT)
  if (instr->representation() == kUnboxedInt64) {
    LValue gep = impl().BuildAccessPointer(array, offset_value, kUnboxedUint32);
    LValue value_32 = output().buildLoad(gep);
    value = output().buildCast(LLVMZExt, value_32, output().repo().int64);
  }
#endif
  if (instr->representation() == kTagged) {
    EMASSERT(instr->representation() == kTagged);
    LType type;
    switch (instr->class_id()) {
      case kOneByteStringCid:
      case kExternalOneByteStringCid:
        switch (instr->element_count()) {
          case 1:
            type = output().repo().int8;
            break;
          case 2:
            type = output().repo().int16;
            break;
          case 4:
            type = output().repo().int32;
            break;
          default:
            UNREACHABLE();
        }
        break;
      case kTwoByteStringCid:
      case kExternalTwoByteStringCid:
        switch (instr->element_count()) {
          case 1:
            type = output().repo().int16;
            break;
          case 2:
            type = output().repo().int32;
            break;
          default:
            UNREACHABLE();
        }
        break;
      default:
        UNREACHABLE();
        break;
    }
    LValue gep =
        impl().BuildAccessPointer(array, offset_value, pointerType(type));
    LValue int_value = impl().EnsureIntPtr(output().buildLoad(gep));

    if (instr->can_pack_into_smi()) {
      value = impl().SmiTag(int_value);
    } else {
      // FIXME: implement after call support
      UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  LValue value = impl().GetLLVMValue(instr->value());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  switch (instr->class_id()) {
    case kArrayCid:
      if (instr->ShouldEmitStoreBarrier()) {
        // FIXME: implment after call support
        UNREACHABLE();
      } else {
        LValue gep = impl().BuildAccessPointer(
            array, offset_value, pointerType(output().tagged_type()));
        output().buildStore(value, gep);
      }
      break;
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kOneByteStringCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      value = impl().EnsureIntPtr(value);
      LValue constant = output().constIntPtr(0xFF);
      LValue cmp_1 = output().buildICmp(LLVMIntSLE, value, constant);
      LValue cmp_2 = output().buildICmp(LLVMIntULE, value, constant);
      LValue value_select =
          output().buildSelect(cmp_1, output().constIntPtr(0), constant);
      value = output().buildSelect(cmp_2, value, value_select);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref16);
      if (typeOf(value) != output().repo().int16)
        value = output().buildCast(LLVMTrunc, value, output().repo().int16);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref32);
      if (typeOf(value) != output().repo().int32)
        value = output().buildCast(LLVMTrunc, value, output().repo().int32);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt64ArrayCid:
    case kTypedDataUint64ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref64);
      EMASSERT(typeOf(value) == output().repo().int64)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat32ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refFloat);
      EMASSERT(typeOf(value) == output().repo().floatType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refDouble);
      EMASSERT(typeOf(value) == output().repo().doubleType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64x2ArrayCid:
    case kTypedDataInt32x4ArrayCid:
    case kTypedDataFloat32x4ArrayCid: {
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
  }
}

void IRTranslator::VisitStoreInstanceField(StoreInstanceFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInitInstanceField(InitInstanceFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInitStaticField(InitStaticFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue field = impl().GetLLVMValue(instr->field_value());
  LValue v = impl().LoadFieldFromOffset(
      field, compiler::target::Field::static_value_offset());
  impl().SetLLVMValue(instr, v);
}

void IRTranslator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  impl().SetDebugLine(instr);

  LValue true_or_false = impl().GetLLVMValue(instr->value());
  LValue true_value = impl().LoadObject(Bool::True());
  LValue false_value = impl().LoadObject(Bool::False());
  LValue cmp_value = output().buildICmp(LLVMIntEQ, true_value, true_or_false);
  LValue value = output().buildSelect(cmp_value, false_value, true_value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstanceOf(InstanceOfInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCreateArray(CreateArrayInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateObject(AllocateObjectInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadField(LoadFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instance = impl().GetLLVMValue(instr->instance());
  LValue boxed =
      impl().LoadFieldFromOffset(instance, instr->slot().offset_in_bytes());
  if (instr->IsUnboxedLoad()) {
    const intptr_t cid = instr->slot().field().UnboxedFieldCid();
    LValue value;
    switch (cid) {
      case kDoubleCid:
        value = impl().LoadFieldFromOffset(
            boxed, compiler::target::Double::value_offset(),
            output().repo().refDouble);
        break;
      case kFloat32x4Cid:
        EMASSERT("kFloat32x4Cid not supported" && false);
        UNREACHABLE();
      case kFloat64x2Cid:
        EMASSERT("kFloat64x2Cid not supported" && false);
        UNREACHABLE();
      default:
        UNREACHABLE();
    }
    impl().SetLLVMValue(instr, value);
    return;
  }
  if (instr->IsPotentialUnboxedLoad()) {
    // FIXME: need for continuation resolver
    // support after call implementation.
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, boxed);
}

void IRTranslator::VisitLoadUntagged(LoadUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  if (instr->object()->definition()->representation() == kUntagged) {
    value =
        impl().LoadFromOffset(object, instr->offset(), output().repo().refPtr);
  } else {
    value = impl().LoadFieldFromOffset(object, instr->offset());
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreUntagged(StoreUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value = impl().GetLLVMValue(instr->value());
  impl().StoreToOffset(object, instr->offset_from_tagged(), value);
}

void IRTranslator::VisitLoadClassId(LoadClassIdInstr* instr) {
  impl().SetDebugLine(instr);
  const AbstractType& value_type = *instr->object()->Type()->ToAbstractType();
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  const intptr_t class_id_offset =
      compiler::target::Object::tags_offset() +
      compiler::target::RawObject::kClassIdTagPos / kBitsPerByte;
  if (CompileType::Smi().IsAssignableTo(value_type) ||
      value_type.IsTypeParameter()) {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(object); })
        .BuildLeft([&]() { return output().constInt16(kSmiCid); })
        .BuildRight([&]() {
          return impl().LoadFieldFromOffset(object, class_id_offset,
                                            pointerType(output().repo().int16));
        });

    value = diamond.End();
  } else {
    value = impl().LoadFromOffset(object, class_id_offset - kHeapObjectTag,
                                  pointerType(output().repo().int16));
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstantiateType(InstantiateTypeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateContext(AllocateContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateUninitializedContext(
    AllocateUninitializedContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCloneContext(CloneContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().SmiUntag(impl().GetLLVMValue(instr->left()));
  LValue right = impl().SmiUntag(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) {
  impl().SetDebugLine(instr);
  // FIXME: implment slow path code after call supported.
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  LValue cmp_value =
      output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                         impl().SmiUntag(left), impl().SmiUntag(right));
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitCheckedSmiOp(CheckedSmiOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt32Op(BinaryInt32OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().EnsureInt32(impl().GetLLVMValue(instr->left()));
  LValue right = impl().EnsureInt32(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryDoubleOp(UnaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(v) == output().repo().doubleType);
  impl().SetLLVMValue(instr, output().buildFNeg(v));
}

void IRTranslator::VisitCheckStackOverflow(CheckStackOverflowInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  LValue num = impl().SmiUntag(v);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt32ToDouble(Int32ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().EnsureInt32(impl().GetLLVMValue(instr->value()));
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt64ToDouble(Int64ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(num) == output().repo().int64);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToFloat(DoubleToFloatInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitFloatToDouble(FloatToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue fnum = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(fnum) == output().repo().floatType);
  LValue dnum = output().buildCast(LLVMFPExt, fnum, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitCheckClass(CheckClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckClassId(CheckClassIdInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckSmi(CheckSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckNull(CheckNullInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckCondition(CheckConditionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitConstant(ConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitUnboxedConstant(UnboxedConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitCheckEitherNonSmi(CheckEitherNonSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryDoubleOp(BinaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().doubleType);
  EMASSERT(typeOf(right) == output().repo().doubleType);
  LValue value;
  switch (instr->op_kind()) {
    case Token::kADD:
      value = output().buildFAdd(left, right);
      break;
    case Token::kSUB:
      value = output().buildFSub(left, right);
      break;
    case Token::kMUL:
      value = output().buildFMul(left, right);
      break;
    case Token::kDIV:
      value = output().buildFDiv(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitDoubleTestOp(DoubleTestOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  LValue cmp_value;
  if (instr->op_kind() == MethodRecognizer::kDouble_getIsNaN) {
    cmp_value = output().buildFCmp(LLVMRealUNO, value, value);
  } else {
    ASSERT(instr->op_kind() == MethodRecognizer::kDouble_getIsInfinite);
    value = output().buildCall(output().repo().doubleAbsIntrinsic(), value);
    cmp_value =
        output().buildFCmp(LLVMRealOEQ, value, output().constDouble(INFINITY));
  }
  const bool is_negated = instr->kind() != Token::kEQ;
  if (is_negated) {
    cmp_value =
        output().buildICmp(LLVMIntEQ, cmp_value, output().repo().booleanFalse);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitMathUnary(MathUnaryInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  if (instr->kind() == MathUnaryInstr::kSqrt) {
    value = output().buildCall(output().repo().doubleSqrtIntrinsic(), value);
  } else if (instr->kind() == MathUnaryInstr::kDoubleSquare) {
    value = output().buildFMul(value, value);
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitMathMinMax(MathMinMaxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  LValue value;
  LValue cmp_value;
  const intptr_t is_min = (instr->op_kind() == MethodRecognizer::kMathMin);
  if (instr->result_cid() == kDoubleCid) {
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(LLVMRealOGT, left, right);
  } else {
    cmp_value = output().buildICmp(LLVMIntSGT, left, right);
  }
  if (is_min)
    value = output().buildSelect(cmp_value, right, left);
  else
    value = output().buildSelect(cmp_value, left, right);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBox(BoxInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnbox(UnboxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;
  auto EmitLoadFromBox = [&]() {
    switch (instr->representation()) {
      case kUnboxedInt64: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().int64));
        return output().buildLoad(gep);
      }

      case kUnboxedDouble: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        return output().buildLoad(gep);
      }

      case kUnboxedFloat: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        LValue result = output().buildLoad(gep);
        return output().buildCast(LLVMFPTrunc, result,
                                  output().repo().floatType);
      }

      case kUnboxedFloat32x4:
      case kUnboxedFloat64x2:
      case kUnboxedInt32x4: {
        UNREACHABLE();
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
  };

  auto EmitLoadInt32FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() { return impl().SmiUntag(value); })
        .BuildRight([&]() {
          return impl().LoadFieldFromOffset(
              value, compiler::target::Mint::value_offset());
        });
    return diamond.End();
  };
  auto EmitSmiConversion = [&]() {
    LValue result;
    switch (instr->representation()) {
      case kUnboxedInt64: {
        result = impl().SmiUntag(value);
        result = output().buildCast(LLVMSExt, result, output().repo().int64);
        break;
      }

      case kUnboxedDouble: {
        result = impl().SmiUntag(value);
        result =
            output().buildCast(LLVMSIToFP, result, output().repo().doubleType);
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
    return result;
  };
  auto EmitLoadInt64FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() {
          LValue result = impl().SmiUntag(value);
          return output().buildCast(LLVMSExt, result, output().repo().int64);
        })
        .BuildRight([&]() { return EmitLoadFromBox(); });
    return diamond.End();
  };
  if (instr->speculative_mode() == UnboxInstr::kNotSpeculative) {
    switch (instr->representation()) {
      case kUnboxedDouble:
      case kUnboxedFloat: {
        result = EmitLoadFromBox();
      } break;

      case kUnboxedInt32:
        result = EmitLoadInt32FromBoxOrSmi();
        break;

      case kUnboxedInt64: {
        if (instr->value()->Type()->ToCid() == kSmiCid) {
          // Smi -> int64 conversion is more efficient than
          // handling arbitrary smi/mint.
          result = EmitSmiConversion();
        } else {
          result = EmitLoadInt64FromBoxOrSmi();
        }
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
  } else {
    ASSERT(instr->speculative_mode() == UnboxInstr::kGuardInputs);
    const intptr_t value_cid = instr->value()->Type()->ToCid();
    const intptr_t box_cid = Boxing::BoxCid(instr->representation());

    if (value_cid == box_cid) {
      result = EmitLoadFromBox();
    } else if (instr->CanConvertSmi() && (value_cid == kSmiCid)) {
      result = EmitSmiConversion();
    } else if (instr->representation() == kUnboxedInt32 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt32FromBoxOrSmi();
    } else if (instr->representation() == kUnboxedInt64 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt64FromBoxOrSmi();
    } else {
      UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBoxInt64(BoxInt64Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt64(UnboxInt64Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt64Op(BinaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int64);
  EMASSERT(typeOf(right) == output().repo().int64);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND: {
      result = output().buildAnd(left, right);
      break;
    }
    case Token::kBIT_OR: {
      result = output().buildOr(left, right);
      break;
    }
    case Token::kBIT_XOR: {
      result = output().buildXor(left, right);
      break;
    }
    case Token::kADD: {
      result = output().buildAdd(left, right);
      break;
    }
    case Token::kSUB: {
      result = output().buildSub(left, right);
      break;
    }
    case Token::kMUL: {
      result = output().buildMul(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftInt64Op(ShiftInt64OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftInt64Op(
    SpeculativeShiftInt64OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryInt64Op(UnaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());

  switch (instr->op_kind()) {
    case Token::kBIT_NOT:
      value = output().buildNot(value);
      break;
    case Token::kNEGATE:
      value = output().buildNeg(value);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGenericCheckBound(GenericCheckBoundInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitConstraint(ConstraintInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringToCharCode(StringToCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOneByteStringFromCharCode(
    OneByteStringFromCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringInterpolate(StringInterpolateInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInvokeMathCFunction(InvokeMathCFunctionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTruncDivMod(TruncDivModInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldClass(GuardFieldClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldLength(GuardFieldLengthInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldType(GuardFieldTypeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

static bool IsPowerOfTwoKind(intptr_t v1, intptr_t v2) {
  return (Utils::IsPowerOfTwo(v1) && (v2 == 0)) ||
         (Utils::IsPowerOfTwo(v2) && (v1 == 0));
}

void IRTranslator::VisitIfThenElse(IfThenElseInstr* instr) {
  ComparisonInstr* compare = instr->comparison();
  if (!impl().current_bb_impl().IsDefined(compare)) {
    compare->Accept(this);
  }
  EMASSERT(impl().current_bb_impl().IsDefined(compare));
  impl().SetDebugLine(instr);
  LValue cmp_value = impl().GetLLVMValue(compare->ssa_temp_index());

  intptr_t true_value = instr->if_true();
  intptr_t false_value = instr->if_false();

  const bool is_power_of_two_kind = IsPowerOfTwoKind(true_value, false_value);

  if (is_power_of_two_kind) {
    if (true_value == 0) {
      // We need to have zero in result on true_condition.
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  } else {
    if (true_value == 0) {
      // Swap values so that false_value is zero.
      intptr_t temp = true_value;
      true_value = false_value;
      false_value = temp;
    } else {
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  }
  LValue result = output().buildSelect(cmp_value, output().constIntPtr(1),
                                       output().constIntPtr(0));

  if (is_power_of_two_kind) {
    const intptr_t shift =
        Utils::ShiftForPowerOfTwo(Utils::Maximum(true_value, false_value));
    result =
        output().buildShl(result, output().constIntPtr(shift + kSmiTagSize));
  } else {
    result = output().buildSub(result, output().constIntPtr(1));
    const int32_t val = compiler::target::ToRawSmi(true_value) -
                        compiler::target::ToRawSmi(false_value);
    result = output().buildAnd(result, output().constIntPtr(val));
    if (false_value != 0) {
      result = output().buildAdd(
          result,
          output().constIntPtr(compiler::target::ToRawSmi(false_value)));
    }
  }
  result = output().buildCast(LLVMIntToPtr, result, output().tagged_type());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitMaterializeObject(MaterializeObjectInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestSmi(TestSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestCids(TestCidsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitExtractNthOutput(ExtractNthOutputInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryUint32Op(BinaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int32);
  EMASSERT(typeOf(right) == output().repo().int32);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND:
      result = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      result = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      result = output().buildXor(left, right);
      break;
    case Token::kADD:
      result = output().buildAdd(left, right);
      break;
    case Token::kSUB:
      result = output().buildSub(left, right);
      break;
    case Token::kMUL:
      result = output().buildMul(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftUint32Op(ShiftUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftUint32Op(
    SpeculativeShiftUint32OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryUint32Op(UnaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(instr->op_kind() == Token::kBIT_NOT);
  LValue value = impl().GetLLVMValue(instr->value());

  value = output().buildNot(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBoxUint32(BoxUint32Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxUint32(UnboxUint32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitBoxInt32(BoxInt32Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt32(UnboxInt32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitIntConverter(IntConverterInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;

  if (instr->from() == kUnboxedInt32 && instr->to() == kUnboxedUint32) {
    result = value;
  } else if (instr->from() == kUnboxedUint32 && instr->to() == kUnboxedInt32) {
    result = value;
  } else if (instr->from() == kUnboxedInt64) {
    if (instr->to() == kUnboxedInt32) {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    } else {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    }
  } else if (instr->to() == kUnboxedInt64) {
    if (instr->from() == kUnboxedUint32) {
      result = output().buildCast(LLVMZExt, value, output().repo().int64);
    } else {
      result = output().buildCast(LLVMSExt, value, output().repo().int64);
    }
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBitCast(BitCastInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedWidthExtender(UnboxedWidthExtenderInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDeoptimize(DeoptimizeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitSimdOp(SimdOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
