//===-- LoopUnrollAggressive.cpp - General Loop unroller pass -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements a general loop unroller.
// It works best when loops have been canonicalized by the -indvars pass.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "loop-unroll-aggressive"
#include "llvm/Transforms/Scalar.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include <climits>

using namespace llvm;

STATISTIC(NumUnrolledAggressive, "Number of loops unrolled aggressive");

static cl::opt<bool>
DisableLoopUnrollAggressive("disable-loop-unroll-aggressive",cl::init(false), cl::Hidden,
  cl::desc(""));

static cl::opt<bool>
TraceLoopUnrollAggressive("trace-loop-unroll-aggressive", cl::init(false), cl::Hidden,
  cl::desc(""));

static cl::opt<unsigned>
UnrollThreshold("unroll-aggressive-threshold", cl::init(150), cl::Hidden,
  cl::desc("The cut-off point for automatic loop unrolling"));

static cl::opt<unsigned>
UnrollCount("unroll-aggressive-count", cl::init(0), cl::Hidden,
  cl::desc("Use this unroll count for all loops, for testing purposes"));

namespace {
  class LoopUnrollAggressive : public LoopPass {
  public:
    static char ID; // Pass ID, replacement for typeid
    LoopUnrollAggressive(int T = -1, int C = -1,  int P = -1) : LoopPass(ID) {
      CurrentThreshold = (T == -1) ? UnrollThreshold : unsigned(T);
      CurrentCount = (C == -1) ? UnrollCount : unsigned(C);
      UserThreshold = (T != -1) || (UnrollThreshold.getNumOccurrences() > 0);
      initializeLoopUnrollAggressivePass(*PassRegistry::getPassRegistry());
    }

    /// A magic value for use with the Threshold parameter to indicate
    /// that the loop unroll should be performed regardless of how much
    /// code expansion would result.
    static const unsigned NoThreshold = UINT_MAX;

    // Threshold to use when optsize is specified (and there is no
    // explicit -unroll-threshold).
    static const unsigned OptSizeUnrollThreshold = 50;

    unsigned CurrentCount;
    unsigned CurrentThreshold;
    bool     UserThreshold;        // CurrentThreshold is user-specified.

    bool runOnLoop(Loop *L, LPPassManager &LPM);

    /// This transformation requires natural loop information & requires that
    /// loop preheaders be inserted into the CFG...
    ///
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<LoopInfo>();
      AU.addPreserved<LoopInfo>();
      AU.addRequiredID(LoopSimplifyID);
      AU.addPreservedID(LoopSimplifyID);
      AU.addRequiredID(LCSSAID);
      AU.addPreservedID(LCSSAID);
      AU.addRequired<TargetTransformInfo>();
      // FIXME: Loop unroll requires LCSSA. And LCSSA requires dom info.
      // If loop unroll does not preserve dom info then LCSSA pass on next
      // loop will receive invalid dom info.
      // For now, recreate dom info, if loop is unrolled.
      AU.addPreserved<DominatorTree>();
    }
  };
}

char LoopUnrollAggressive::ID = 0;
INITIALIZE_PASS_BEGIN(LoopUnrollAggressive,
                      "loop-unroll-aggressive", "Unroll general loops", false, false)
INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSA)
INITIALIZE_PASS_END(LoopUnrollAggressive,
                    "loop-unroll-aggressive", "Unroll general loops", false, false)

Pass *llvm::createLoopUnrollAggressivePass(int Threshold, int Count) {
  return new LoopUnrollAggressive(Threshold, Count);
}

/// ApproximateLoopSize - Approximate the size of the loop.
static unsigned ApproximateLoopSize(const Loop *L, unsigned &NumCalls,
                                    bool &NotDuplicatable,
                                    const TargetTransformInfo &TTI) {
  CodeMetrics Metrics;
  for (Loop::block_iterator I = L->block_begin(), E = L->block_end();
       I != E; ++I)
    Metrics.analyzeBasicBlock(*I, TTI);
  NumCalls = Metrics.NumInlineCandidates;
  NotDuplicatable = Metrics.notDuplicatable;

  unsigned LoopSize = Metrics.NumInsts;

  // Don't allow an estimate of size zero.  This would allows unrolling of loops
  // with huge iteration counts, which is a compile time problem even if it's
  // not a problem for code quality.
  if (LoopSize == 0) LoopSize = 1;

  return LoopSize;
}

bool LoopUnrollAggressive::runOnLoop(Loop *L, LPPassManager &LPM) {
  if (DisableLoopUnrollAggressive) {
    return false;
  }
  LoopInfo *LI = &getAnalysis<LoopInfo>();
  const TargetTransformInfo &TTI = getAnalysis<TargetTransformInfo>();

  BasicBlock *Header = L->getHeader();
  DEBUG(dbgs() << "LUA: F[" << Header->getParent()->getName()
        << "] Loop %" << Header->getName() << "\n");
  Function *F = Header->getParent();
  DEBUG(Header->dump());

  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader) {
    DEBUG(dbgs() << "LUA: Can't unroll: loop preheader-insertion failed.\n");
    return false;
  }

  // try loop unroll with single body
  BasicBlock *LatchBlock = L->getLoopLatch();
  if (Header != LatchBlock) {
    DEBUG(dbgs() << "LUA: Can't unroll: loop is not single bb.\n");
    return false;
  }

  BranchInst *BI = dyn_cast_or_null<BranchInst>(LatchBlock->getTerminator());
  if (!BI || BI->isUnconditional()) {
    DEBUG(dbgs() << "LUA: Can't unroll: loop is not terminated by a conditional branch.\n");
    return false;
  }

  if (Header->hasAddressTaken()) {
    DEBUG(dbgs() << "LUA: Can't unroll: address of header block is taken.\n");
    return false;
  }

  ICmpInst *Cond = dyn_cast_or_null<ICmpInst>(BI->getCondition());
  if (!Cond) {
    DEBUG(dbgs() << "LUA: Can't unroll: loop is not ICmpInst branch.\n");
    return false;
  }

  // canonicalize indexes.
  BasicBlock *ExitBlock = BI->getSuccessor(0);
  int ExitIndex = 0;
  int LatchIndex = 1;
  if (ExitBlock == LatchBlock) {
    ExitBlock = BI->getSuccessor(1);
    ExitIndex = 1;
    LatchIndex = 0;
  }

  // Determine the current unrolling threshold.  While this is normally set
  // from UnrollThreshold, it is overridden to a smaller value if the current
  // function is marked as optimize-for-size, and the unroll threshold was
  // not user specified.
  unsigned Threshold = CurrentThreshold;
  if (!UserThreshold &&
      Header->getParent()->getAttributes().
        hasAttribute(AttributeSet::FunctionIndex,
                     Attribute::OptimizeForSize))
    Threshold = OptSizeUnrollThreshold;

  // Use a default unroll-count if the user doesn't specify a value
  // and the trip count is a run-time value.  The default is different
  // for run-time or compile-time trip count loops.
  unsigned Count = 4;

  // Enforce the threshold.
  if (Threshold != NoThreshold) {
    unsigned NumInlineCandidates;
    bool notDuplicatable;
    unsigned LoopSize = ApproximateLoopSize(L, NumInlineCandidates,
                                            notDuplicatable, TTI);
    DEBUG(dbgs() << "LUA: Loop Size = " << LoopSize << "\n");
    if (notDuplicatable) {
      DEBUG(dbgs() << "LUA: Not unrolling loop which contains non duplicatable"
            << " instructions.\n");
      return false;
    }
    if (NumInlineCandidates != 0) {
      DEBUG(dbgs() << "LUA: Not unrolling loop with inlinable calls.\n");
      return false;
    }
    uint64_t Size = (uint64_t)LoopSize*Count;
    if (Size > Threshold) {
      DEBUG(dbgs() << "LUA: Too large to fully unroll with count: " << Count
            << " because size: " << Size << ">" << Threshold << "\n");
      Count = Threshold / LoopSize;
      if (Count < 2) {
        DEBUG(dbgs() << "LUA: could not unroll partially\n");
        return false;
      }
      DEBUG(dbgs() << "LUA: partially unrolling with count: " << Count << "\n");
    }
  }

  int unrollNum = Count;
  // Unroll the loop.
  // after IndVarSimplify
  // cond = icmp predicate inc upper
  {
    Value *op0 = Cond->getOperand(0);
    Instruction *Inc = dyn_cast_or_null<Instruction>(op0);
    if (!Inc || Inc->getOpcode() != Instruction::Add) {
      DEBUG(dbgs() << "LUA: Can't unroll: increment expression is not Add inst.\n");
      return false;
    }

    ICmpInst::Predicate Pred = Cond->getPredicate();
    if (LatchIndex == 0) {
      // require
      // cond = icmp NE inc upper
      // branch cond latch , exit
      if (Pred != ICmpInst::ICMP_NE) {
        DEBUG(dbgs() << "LUA: Can't unroll: Predicate is not NE.\n");
        return false;
      }
    } else if (LatchIndex == 1) {
      // require
      // cond = icmp EQ inc upper
      // branch cond exit, latch
      if (Pred != ICmpInst::ICMP_EQ) {
        DEBUG(dbgs() << "LUA: Can't unroll: Predicate is not EQ.\n");
        return false;
      }
    } else {
      assert(0 && "LUA: LatchIndex is not (0 or 1) ???");
    }

    Value *condOp1 = Cond->getOperand(1);
    IntegerType *NType = dyn_cast_or_null<IntegerType>(condOp1->getType());
    if (!NType) {
      DEBUG(dbgs() << "LUA: Can't unroll: upper expression is not IntegerType\n");
      return false;
    }

    Instruction *N = dyn_cast_or_null<Instruction>(condOp1);
    Argument *ArgN = dyn_cast_or_null<Argument>(condOp1);
    if (ArgN) {
      //ok
    } else if (!N || !L->isLoopInvariant(N) ) {
      DEBUG(dbgs() << "LUA: Can't unroll: upper expression is not LoopInvariant\n");
      return false;
    }

    Value *incOp1 = Inc->getOperand(1);
    ConstantInt *ci = dyn_cast_or_null<ConstantInt>(incOp1);
    if (!ci || !ci->isOne()) {
      DEBUG(dbgs() << "LUA: Can't unroll: increment const is not one.\n");
      return false;
    }
  }

  // try loop unrolling
  if (TraceLoopUnrollAggressive) {
    DEBUG(dbgs() << "LUA: (1) dump before transform\n");
    DEBUG(F->dump());
  }

  BasicBlock *AllocaBB = &F->getEntryBlock();
  // reg2mem transforms a PHINode to load/store inst.
  // notice sideeffect
  {
    BasicBlock::iterator I = AllocaBB->begin();
    while (isa<AllocaInst>(I)) ++I;

    CastInst *AllocaInsertionPoint =
    new BitCastInst(Constant::getNullValue(Type::getInt32Ty(F->getContext())),
                    Type::getInt32Ty(F->getContext()),
                    "loop unrolling aggressive alloca point", I);

    std::list<Instruction*> WorkList;
    WorkList.clear();
    for (Function::iterator ibb = F->begin(), ibe = F->end();
         ibb != ibe; ++ibb) {
      for (BasicBlock::iterator iib = ibb->begin(), iie = ibb->end();
           iib != iie; ++iib) {
        if (isa<PHINode>(iib)) {
          WorkList.push_front(&*iib);
        }
      }
    }

    for (std::list<Instruction*>::iterator ilb = WorkList.begin(),
         ile = WorkList.end(); ilb != ile; ++ilb) {
      DemotePHIToStack(cast<PHINode>(*ilb), AllocaInsertionPoint);
    }
  }

  if (TraceLoopUnrollAggressive) {
    DEBUG(dbgs() << "LUA: (2) dump after reg2mem\n");
    DEBUG(F->dump());
  }

  Value *Upper = Cond->getOperand(1);
  IntegerType *UpperType = dyn_cast<IntegerType>(Upper->getType());
  assert(UpperType && "LUA: not IntegerType ... what cause DemotePHIToStack sideeffect?");
  BasicBlock *switchBB = Preheader;
  Preheader = SplitBlock(Preheader, Preheader->getTerminator(), this);

  // generate guarantee case body
  {
    IRBuilder<> Builder(switchBB, switchBB->getTerminator());

    // Maybe InstCombine trans URem to And
    Value *guardexpr = (Builder.CreateURem(Upper, ConstantInt::get(UpperType, unrollNum), ".LUA.Guard"));

    SwitchInst *unrollSwitch =
      Builder.CreateSwitch(guardexpr, Preheader, unrollNum-1);
    switchBB->getTerminator()->eraseFromParent();

    BasicBlock *nextBB = Preheader;
    for (int i=1; i<unrollNum; i++) {
      // generate bb from case i
      ValueToValueMapTy VMap;

      BasicBlock *newBB = CloneBasicBlock(Header, VMap, ".LUA.Guard");
      F->getBasicBlockList().push_back(newBB);
      // if nested loop, add loopbody to parent
      if (Loop* parent = L->getParentLoop()) {
        parent->addBasicBlockToLoop(newBB, LI->getBase());
      }

      // notice that CloneBasicBlock is not update user's def-use chain.
      for (BasicBlock::iterator I = newBB->begin(), E = newBB->end();
           I != E; ++I) {
        RemapInstruction(I, VMap, RF_NoModuleLevelChanges|RF_IgnoreMissingEntries);
      }
      newBB->getTerminator()->eraseFromParent();
      BranchInst::Create(nextBB, newBB);

      nextBB = newBB;
      unrollSwitch->addCase(ConstantInt::get(UpperType, i), nextBB);
    }
  }

  if (TraceLoopUnrollAggressive) {
    DEBUG(dbgs() << "LUA: (3) dump after guarantee switch case\n");
    DEBUG(F->dump());
  }

  // unroll loop body
  {
    BasicBlock *prevBB = Header;
    for (int i=1; i<unrollNum; i++) {
      ValueToValueMapTy VMap;

      BasicBlock *newBB = CloneBasicBlock(prevBB, VMap, ".LUA");
      F->getBasicBlockList().push_back(newBB);
      L->addBasicBlockToLoop(newBB, LI->getBase());

      // notice that CloneBasicBlock is not update user's def-use chain.
      for (BasicBlock::iterator I = newBB->begin(), E = newBB->end();
           I != E; ++I) {
        RemapInstruction(I, VMap, RF_NoModuleLevelChanges|RF_IgnoreMissingEntries);
      }
      prevBB->getTerminator()->eraseFromParent();
      BranchInst::Create(newBB, prevBB);

      prevBB = newBB;
    }
    BranchInst *currbi = dyn_cast<BranchInst>(prevBB->getTerminator());
    currbi->setSuccessor(LatchIndex, Header);
    currbi->setSuccessor(ExitIndex, ExitBlock);
  }

  if (TraceLoopUnrollAggressive) {
    DEBUG(dbgs() << "LUA: (4) dump after unroll body\n");
    DEBUG(F->dump());
  }

  // promote mem2reg
  // load/store to PHINode
  {
    if (DominatorTree *DT = LPM.getAnalysisIfAvailable<DominatorTree>()) {
      DT->runOnFunction(*F);

      std::vector<AllocaInst*> Allocas;
      Allocas.clear();

      for (BasicBlock::iterator I = AllocaBB->begin(), E = --AllocaBB->end(); I != E; ++I) {
        if (AllocaInst *AI = dyn_cast<AllocaInst>(I)) {
          if (isAllocaPromotable(AI)) {
            Allocas.push_back(AI);
          }
        }
      }
      if (!Allocas.empty()) {
        PromoteMemToReg(Allocas, *DT);
      }
    }
  }

  if (TraceLoopUnrollAggressive) {
    DEBUG(dbgs() << "LUA: (5) dump after transform\n");
    DEBUG(F->dump());
  }

  ++NumUnrolledAggressive;
  return true;
}
