#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <unordered_map>

#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/initialize_llvm.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#include "vm/compiler/backend/llvm/output.h"

namespace dart {
namespace dart_llvm {
namespace {
struct NotMergedPhiDesc {
  BlockEntryInstr* pred;
  int value;
  LValue phi;
};

struct GCRelocateDesc {
  int value;
  int where;
  GCRelocateDesc(int v, int w) : value(v), where(w) {}
};

using GCRelocateDescList = std::vector<GCRelocateDesc>;

struct PredecessorCallInfo {
  GCRelocateDescList gc_relocates;
};

enum class ValueType { LLVMValue, RelativeCallTarget };

struct ValueDesc {
  ValueType type;
  Definition* constant_def;
  union {
    LValue llvm_value;
    int64_t relative_call_target;
  };
};

struct IRTranslatorBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::unordered_map<int, ValueDesc> values_;
  PredecessorCallInfo call_info;

  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  LValue landing_pad = nullptr;

  bool started = false;
  bool ended = false;
  bool need_merge = false;
  bool exception_block = false;

  inline void SetLLVMValue(int nid, LValue value) {
    values_[nid] = {ValueType::LLVMValue, nullptr, {value}};
  }

  inline void SetValue(int nid, const ValueDesc& value) {
    values_[nid] = value;
  }

  inline LValue GetLLVMValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    EMASSERT(found->second.type == ValueType::LLVMValue);
    return found->second.llvm_value;
  }

  const ValueDesc& GetValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
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
  void EnsureNativeBB(BlockEntryInstr* bb);
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
  LValue EnsurePhiInput(BlockEntryInstr* pred, int index, LType type);
  void BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                 BlockEntryInstr* ref_pred);

  inline CompilerState& compiler_state() { return *compiler_state_; }
  inline LivenessAnalysis& liveness() { return *liveness_analysis_; }
  inline Output& output() { return *output_; }

  BlockEntryInstr* current_bb_;
  FlowGraph* flow_graph_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
  std::unique_ptr<Output> output_;
  std::unordered_map<BlockEntryInstr*, IRTranslatorBlockImpl> block_map_;
  std::vector<BlockEntryInstr*> phi_rebuild_worklist_;
};

IRTranslatorBlockImpl* AnonImpl::GetTranslatorBlockImpl(BlockEntryInstr* bb) {
  auto& ref = block_map_[bb];
  return &ref;
}

void AnonImpl::EnsureNativeBB(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* impl = GetTranslatorBlockImpl(bb);
  if (impl->native_bb) return;
  char buf[256];
  snprintf(buf, 256, "B%d", static_cast<int>(bb->block_id()));
  LBasicBlock native_bb = output_->appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
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
  EnsureNativeBB(bb);
  GetTranslatorBlockImpl(bb)->StartTranslate();
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
    if (block_impl->exception_block) {
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
    }
    return;
  }
  BlockEntryInstr* ref_pred = nullptr;
  if (!AllPredecessorStarted(bb, &ref_pred)) {
    EMASSERT(!!ref_pred);
    BuildPhiAndPushToWorkList(bb, ref_pred);
    return;
  }
  // Use phi.
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    auto& value = GetTranslatorBlockImpl(ref_pred)->GetValue(live);
    if (value.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value);
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

void AnonImpl::End() {
  EMASSERT(!!current_bb_);
  EndCurrentBlock();
  ProcessPhiWorkList();
  output().positionToBBEnd(output().prologue());
  output().buildBr(GetNativeBB(flow_graph_->graph_entry()));
  output().finalize();
}

void AnonImpl::EndCurrentBlock() {
  GetTranslatorBlockImpl(current_bb_)->EndTranslate();
  current_bb_ = nullptr;
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
}  // namespace

struct IRTranslator::Impl : public AnonImpl {};

IRTranslator::IRTranslator(FlowGraph* flow_graph)
    : FlowGraphVisitor(flow_graph->reverse_postorder()) {
  InitLLVM();
  impl_.reset(new Impl);
  impl().flow_graph_ = flow_graph;
  impl().compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function().function().UserVisibleName())
          .ToCString()));
  impl().liveness_analysis_.reset(new LivenessAnalysis(flow_graph));
  impl().output_.reset(new Output(impl().compiler_state()));
  // init parameter desc
  RegisterParameterDesc parameter_desc;
  LType tagged_type = impl().output().repo().tagged_type;
  for (int i = 1; i <= flow_graph->num_direct_parameters(); ++i) {
    parameter_desc.emplace_back(-i, tagged_type);
  }
  // init output.
  impl().output().initializeBuild(parameter_desc);
}

IRTranslator::~IRTranslator() {}

void IRTranslator::Translate() {
  impl().liveness().Analyze();
  VisitBlocks();
}

void IRTranslator::VisitBlockEntry(BlockEntryInstr* block) {
  impl().current_bb_ = block;
  impl().StartTranslate(block);
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
  UNREACHABLE();
}

void IRTranslator::VisitTargetEntry(TargetEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFunctionEntry(FunctionEntryInstr* instr) {
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitNativeEntry(NativeEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitOsrEntry(OsrEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIndirectEntry(IndirectEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCatchBlockEntry(CatchBlockEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPhi(PhiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitRedefinition(RedefinitionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitParameter(ParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeParameter(NativeParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexedUnsafe(LoadIndexedUnsafeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreIndexedUnsafe(StoreIndexedUnsafeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTailCall(TailCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitParallelMove(ParallelMoveInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPushArgument(PushArgumentInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitReturn(ReturnInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeReturn(NativeReturnInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitThrow(ThrowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitReThrow(ReThrowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStop(StopInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGoto(GotoInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIndirectGoto(IndirectGotoInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBranch(BranchInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertAssignable(AssertAssignableInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertSubtype(AssertSubtypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertBoolean(AssertBooleanInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpecialParameter(SpecialParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitClosureCall(ClosureCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFfiCall(FfiCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstanceCall(InstanceCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStaticCall(StaticCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadLocal(LoadLocalInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDropTemps(DropTempsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMakeTemp(MakeTempInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreLocal(StoreLocalInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStrictCompare(StrictCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitRelationalOp(RelationalOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeCall(NativeCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDebugStepCheck(DebugStepCheckInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadCodeUnits(LoadCodeUnitsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreInstanceField(StoreInstanceFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInitInstanceField(InitInstanceFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInitStaticField(InitStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstanceOf(InstanceOfInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCreateArray(CreateArrayInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateObject(AllocateObjectInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadField(LoadFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadUntagged(LoadUntaggedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreUntagged(StoreUntaggedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadClassId(LoadClassIdInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateType(InstantiateTypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateContext(AllocateContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateUninitializedContext(
    AllocateUninitializedContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCloneContext(CloneContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckedSmiOp(CheckedSmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt32Op(BinaryInt32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryDoubleOp(UnaryDoubleOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckStackOverflow(CheckStackOverflowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInt32ToDouble(Int32ToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInt64ToDouble(Int64ToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToFloat(DoubleToFloatInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFloatToDouble(FloatToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckClass(CheckClassInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckClassId(CheckClassIdInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckSmi(CheckSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckNull(CheckNullInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckCondition(CheckConditionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitConstant(ConstantInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedConstant(UnboxedConstantInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckEitherNonSmi(CheckEitherNonSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryDoubleOp(BinaryDoubleOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleTestOp(DoubleTestOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMathUnary(MathUnaryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMathMinMax(MathMinMaxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBox(BoxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnbox(UnboxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxInt64(BoxInt64Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt64(UnboxInt64Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt64Op(BinaryInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitShiftInt64Op(ShiftInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpeculativeShiftInt64Op(
    SpeculativeShiftInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryInt64Op(UnaryInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGenericCheckBound(GenericCheckBoundInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitConstraint(ConstraintInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStringToCharCode(StringToCharCodeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitOneByteStringFromCharCode(
    OneByteStringFromCharCodeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStringInterpolate(StringInterpolateInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInvokeMathCFunction(InvokeMathCFunctionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTruncDivMod(TruncDivModInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldClass(GuardFieldClassInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldLength(GuardFieldLengthInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldType(GuardFieldTypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIfThenElse(IfThenElseInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMaterializeObject(MaterializeObjectInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTestSmi(TestSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTestCids(TestCidsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitExtractNthOutput(ExtractNthOutputInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryUint32Op(BinaryUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitShiftUint32Op(ShiftUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpeculativeShiftUint32Op(
    SpeculativeShiftUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryUint32Op(UnaryUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxUint32(BoxUint32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxUint32(UnboxUint32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxInt32(BoxInt32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt32(UnboxInt32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIntConverter(IntConverterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBitCast(BitCastInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedWidthExtender(UnboxedWidthExtenderInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDeoptimize(DeoptimizeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSimdOp(SimdOpInstr* instr) {
  UNREACHABLE();
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
