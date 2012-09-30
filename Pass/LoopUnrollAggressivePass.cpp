//===-- LoopUnrollAggressive.cpp - Loop unroller pass -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements a general loop unroller with switch guard.
// It works best when loops have been canonicalized by the -indvars pass.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "loop-unroll-aggressive"
#include "llvm/IntrinsicInst.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include <climits>
#include <list>

using namespace llvm;

STATISTIC(NumUnrolledAggressive, "Number of loops unrolled aggressive");

static cl::opt<bool>
DisableLoopUnrollAggressive("disable-loop-unroll-aggressive", cl::init(false), cl::Hidden,
  cl::desc(""));

static cl::opt<bool>
UnrollAggressiveAndGuard("unroll-aggressive-and-guard", cl::init(false), cl::Hidden,
  cl::desc(""));

static cl::opt<unsigned>
UnrollAggressiveThreshold("unroll-aggressive-threshold", cl::init(150), cl::Hidden,
  cl::desc("The cut-off point for automatic loop unrolling"));

static cl::opt<unsigned>
UnrollAggressiveCount("unroll-aggressive-count", cl::init(2), cl::Hidden,
  cl::desc("Use this unroll count for all loops, for testing purposes"));

static cl::opt<bool>
DisableUnrollAggressiveMem2Reg("disable-unroll-aggressive-mem2reg",
                               cl::init(false), cl::Hidden, cl::desc(""));

static cl::opt<bool>
DisableUnrollAggressiveReg2Mem("disable-unroll-aggressive-reg2mem",
                               cl::init(false), cl::Hidden, cl::desc(""));

static cl::opt<bool>
UnrollAggressiveAllowPartial("unroll-aggressive-allow-partial", cl::init(false), cl::Hidden,
  cl::desc("Allows loops to be partially unrolled until "
           "-unroll-threshold loop size is reached."));

namespace {
  class LoopUnrollAggressive : public LoopPass {
  public:
    static char ID; // Pass ID, replacement for typeid
    LoopUnrollAggressive(int T = -1, int C = -1,  int P = -1) : LoopPass(ID) {
      CurrentThreshold = (T == -1) ? UnrollAggressiveThreshold : unsigned(T);
      CurrentCount = (C == -1) ? UnrollAggressiveCount : unsigned(C);
      CurrentAllowPartial = (P == -1) ? UnrollAggressiveAllowPartial : (bool)P;

      UserThreshold = (T != -1) || (UnrollAggressiveThreshold.getNumOccurrences() > 0);

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
    bool     CurrentAllowPartial;
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
//      AU.addPreservedID(LCSSAID);
      // FIXME: Loop unroll requires LCSSA. And LCSSA requires dom info.
      // If loop unroll does not preserve dom info then LCSSA pass on next
      // loop will receive invalid dom info.
      // For now, recreate dom info, if loop is unrolled.
      AU.addPreserved<DominatorTree>();
    }
  };
}

char LoopUnrollAggressive::ID = 0;
INITIALIZE_PASS_BEGIN(LoopUnrollAggressive, "loop-unroll-aggressive", "Unroll loops", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSA)
INITIALIZE_PASS_END(LoopUnrollAggressive, "loop-unroll-aggressive", "Unroll loops", false, false)

Pass *llvm::createLoopUnrollAggressivePass(int Threshold, int Count, int AllowPartial) {
  return new LoopUnrollAggressive(Threshold, Count, AllowPartial);
}

static void dumpBB(BasicBlock *bb) {
  Function *F = bb->getParent();
  errs() << "### F[" << F->getName() << "] ###\n";
  bb->dump();
}
bool LoopUnrollAggressive::runOnLoop(Loop *L, LPPassManager &LPM) {
  if (DisableLoopUnrollAggressive) {
    return false;
  }
  LoopInfo *LI = &getAnalysis<LoopInfo>();

  int unrollNum = UnrollAggressiveCount;

  BasicBlock *Header = L->getHeader();
  DEBUG(dbgs() << "Loop Unroll Aggressive: F[" << Header->getParent()->getName()
        << "] Loop %" << Header->getName() << "\n");
  (void)Header;
  DEBUG(dumpBB(Header));

  Function *F = Header->getParent();
  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader) {
    DEBUG(dbgs() << "  Can't unroll: loop preheader-insertion failed.\n");
    return false;
  }

  BasicBlock *LatchBlock = L->getLoopLatch();
  if (Header != LatchBlock) {
    DEBUG(dbgs() << "  Can't unroll: loop exit-block-insertion failed.\n");
    return false;
  }

  BranchInst *BI = dyn_cast_or_null<BranchInst>(LatchBlock->getTerminator());
  if (!BI || BI->isUnconditional()) {
    DEBUG(dbgs() << "  Can't unroll: loop not terminated by a conditional branch.\n");
    return false;
  }

  BasicBlock *ExitBlock = BI->getSuccessor(0);
  int ExitIndex = 0;
  int LatchIndex = 1;
  if (ExitBlock == LatchBlock) {
    ExitBlock = BI->getSuccessor(1);
    ExitIndex = 1;
    LatchIndex = 0;
  }

  if (Header->hasAddressTaken()) {
    DEBUG(dbgs() << "  Can't unroll: address of header block is taken.\n");
    return false;
  }

  ICmpInst *Cond = dyn_cast_or_null<ICmpInst>(BI->getCondition());
  if (!Cond) {
    DEBUG(dbgs() << "  Can't unroll: loop is not ICmpInst branch.\n");
    return false;
  }

  // after IndVarSimplify
  // cond = icmp predicate inc upper
  {
    Value *op0 = Cond->getOperand(0);
    Instruction *Inc = dyn_cast_or_null<Instruction>(op0);
    if (!Inc || Inc->getOpcode() != Instruction::Add) {
      DEBUG(dbgs() << "  Can't unroll: increment expression is not Add inst.\n");
      return false;
    }

    ICmpInst::Predicate Pred = Cond->getPredicate();
    if (Pred != ICmpInst::ICMP_NE) {
      DEBUG(dbgs() << "  Can't unroll: Predicate is not NE.\n");
      return false;
    }

    Value *condOp1 = Cond->getOperand(1);
    IntegerType *NType = dyn_cast_or_null<IntegerType>(condOp1->getType());
    if (!NType) {
      DEBUG(dbgs() << "  Can't unroll: upper expression is not IntegerType\n");
      return false;
    }

    Instruction *N = dyn_cast_or_null<Instruction>(condOp1);
    Argument *ArgN = dyn_cast_or_null<Argument>(condOp1);
    if (ArgN) {
      //ok
    } else if (!N || !L->isLoopInvariant(N) ) {
      DEBUG(dbgs() << "  Can't unroll: upper expression is not LoopInvariant\n");
      return false;
    }

    Value *incOp1 = Inc->getOperand(1);
    ConstantInt *ci = dyn_cast_or_null<ConstantInt>(incOp1);
    if (!ci || !ci->isOne()) {
      DEBUG(dbgs() << "  Can't unroll: increment const is not one.\n");
      return false;
    }
  }

  // try loop unrolling
  DEBUG(dbgs() << "Loop Unroll Aggressive: dump before transform\n");
  DEBUG(F->dump());

  BasicBlock *AllocaBB = &F->getEntryBlock();
  // reg2mem
  // ここではPHINodeをload/storeに変換するため、副作用に注意すること。
  if (!DisableUnrollAggressiveReg2Mem) {
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

  Value *Upper = Cond->getOperand(1);
  IntegerType *UpperType = dyn_cast<IntegerType>(Upper->getType());
  assert(UpperType && "not IntegerType ... what cause DemotePHIToStack sideeffect?");
  BasicBlock *switchBB = Preheader;
  Preheader = SplitBlock(Preheader, Preheader->getTerminator(), this);

  // guarantee switchする部分作成
  {
    IRBuilder<> Builder(switchBB, switchBB->getTerminator());

    // URemは、特定条件化でInstcombineがAndに変換してくれるはず
    Value *guardexpr =
      (!UnrollAggressiveAndGuard)
      ? (Builder.CreateURem(Upper, ConstantInt::get(UpperType, unrollNum), "Guard"))
      : (Builder.CreateAnd(Upper, ConstantInt::get(UpperType, unrollNum-1), "Guard"));

    SwitchInst *unrollSwitch =
      Builder.CreateSwitch(guardexpr, Preheader, unrollNum-1);
    switchBB->getTerminator()->eraseFromParent();
    // switch文でfall throghしたほうがパフォーマンスが出るのか??
    // gotoで飛ぶ場合、x86は飛び先の命令をプリフェッチしてくれるはずなので、
    // ペナルティ等を気にする必要はないと予想
    BasicBlock *nextBB = Preheader;
    for (int i=1; i<unrollNum; i++) {

      // 後ろのBBから作っていく
      ValueToValueMapTy VMap;

      BasicBlock *newBB = CloneBasicBlock(Header, VMap, ".guard");
      F->getBasicBlockList().push_back(newBB);
      // nest loopの場合、guardを親ループに登録する必要がある
      if (Loop* parent = L->getParentLoop()) {
        parent->addBasicBlockToLoop(newBB, LI->getBase());
      }

      // CloneBasicBlockだけでは、userは更新されないため
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

  // メインループをn倍展開
  {
    BasicBlock *prevBB = Header;
    for (int i=1; i<unrollNum; i++) {
      ValueToValueMapTy VMap;

      BasicBlock *newBB = CloneBasicBlock(prevBB, VMap, "");
      F->getBasicBlockList().push_back(newBB);
      L->addBasicBlockToLoop(newBB, LI->getBase());

      // CloneBasicBlockだけでは、userは更新されないため
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
  ++NumUnrolledAggressive;

  // promote mem2reg
  if (!DisableUnrollAggressiveMem2Reg) {
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

  DEBUG(dbgs() << "Loop Unroll Aggressive: dump after transform\n");
  DEBUG(F->dump());

  return true;
}
