//===-- irfunction.cpp ----------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/irstate.h"
#include "gen/runtime.h"
#include "gen/tollvm.h"
#include "gen/ms-cxx-helper.h"
#include "ir/irdsymbol.h"
#include "ir/irfunction.h"
#include <sstream>

JumpTarget::JumpTarget(llvm::BasicBlock *targetBlock,
                       CleanupCursor cleanupScope, Statement *targetStatement)
    : targetBlock(targetBlock), cleanupScope(cleanupScope),
      targetStatement(targetStatement) {}

GotoJump::GotoJump(Loc loc, llvm::BasicBlock *sourceBlock,
                   llvm::BasicBlock *tentativeTarget, Identifier *targetLabel)
    : sourceLoc(std::move(loc)), sourceBlock(sourceBlock),
      tentativeTarget(tentativeTarget), targetLabel(targetLabel) {}

CatchScope::CatchScope(llvm::Constant *classInfoPtr,
                       llvm::BasicBlock *bodyBlock, CleanupCursor cleanupScope)
    : classInfoPtr(classInfoPtr), bodyBlock(bodyBlock),
      cleanupScope(cleanupScope) {}

bool useMSVCEH() {
#if LDC_LLVM_VER >= 308
  return global.params.targetTriple->isWindowsMSVCEnvironment();
#else
  return false;
#endif
}

namespace {

#if LDC_LLVM_VER >= 308

// MSVC/x86 uses C++ exception handling that puts cleanup blocks into funclets.
// This means that we cannot use a branch selector and conditional branches
// at cleanup exit to continue with different targets.
// Instead we make a full copy of the cleanup code for every target
//
// Return the beginning basic block of the cleanup code
llvm::BasicBlock *executeCleanupCopying(IRState *irs, CleanupScope &scope,
                                        llvm::BasicBlock *sourceBlock,
                                        llvm::BasicBlock *continueWith,
                                        llvm::BasicBlock *unwindTo,
                                        llvm::Value* funclet) {
  if (isCatchSwitchBlock(scope.beginBlock))
    return continueWith;
  if (scope.cleanupBlocks.empty()) {
    // figure out the list of blocks used by this cleanup step
    findSuccessors(scope.cleanupBlocks, scope.beginBlock, scope.endBlock);
    if (!scope.endBlock->getTerminator())
      // Set up the unconditional branch at the end of the cleanup
      llvm::BranchInst::Create(continueWith, scope.endBlock);
  } else {
    // check whether we have an exit target with the same continuation
    for (CleanupExitTarget &tgt : scope.exitTargets)
      if (tgt.branchTarget == continueWith) {
        tgt.sourceBlocks.push_back(sourceBlock);
        return tgt.cleanupBlocks.front();
      }
  }

  // reuse the original IR if not unwinding and not already used
  bool useOriginal = unwindTo == nullptr && funclet == nullptr;
  for (CleanupExitTarget &tgt : scope.exitTargets)
    useOriginal = useOriginal && tgt.cleanupBlocks.front() != scope.beginBlock;

  // append new target
  scope.exitTargets.push_back(CleanupExitTarget(continueWith));
  scope.exitTargets.back().sourceBlocks.push_back(sourceBlock);

  if (useOriginal) {
    // change the continuation target if the initial branch was created
    // by another instance with unwinding
    if (continueWith)
      if (auto term = scope.endBlock->getTerminator())
        if (auto succ = term->getSuccessor(0))
          if (succ != continueWith) {
            remapBlocksValue(scope.cleanupBlocks, succ, continueWith);
          }
    scope.exitTargets.back().cleanupBlocks = scope.cleanupBlocks;
  } else {
    // clone the code
    cloneBlocks(scope.cleanupBlocks, scope.exitTargets.back().cleanupBlocks,
                continueWith, unwindTo, funclet);
  }
  return scope.exitTargets.back().cleanupBlocks.front();
}

#endif // LDC_LLVM_VER >= 308

void executeCleanup(IRState *irs, CleanupScope &scope,
                    llvm::BasicBlock *sourceBlock,
                    llvm::BasicBlock *continueWith) {
  assert(!useMSVCEH()); // should always use executeCleanupCopying

  if (scope.exitTargets.empty() ||
      (scope.exitTargets.size() == 1 &&
       scope.exitTargets[0].branchTarget == continueWith)) {
    // We didn't need a branch selector before and still don't need one.
    assert(!scope.branchSelector);

    // Set up the unconditional branch at the end of the cleanup if we have
    // not done so already.
    if (scope.exitTargets.empty()) {
      scope.exitTargets.push_back(CleanupExitTarget(continueWith));
      llvm::BranchInst::Create(continueWith, scope.endBlock);
    }
    scope.exitTargets.front().sourceBlocks.push_back(sourceBlock);
    return;
  }

  // We need a branch selector if we are here...
  if (!scope.branchSelector) {
    // ... and have not created one yet, so do so now.
    scope.branchSelector = new llvm::AllocaInst(
        llvm::Type::getInt32Ty(gIR->context()),
        llvm::Twine("branchsel.") + scope.beginBlock->getName(),
        irs->topallocapoint());

    // Now we also need to store 0 to it to keep the paths that go to the
    // only existing branch target the same.
    auto &v = scope.exitTargets.front().sourceBlocks;
    for (auto bb : v) {
      new llvm::StoreInst(DtoConstUint(0), scope.branchSelector,
                          bb->getTerminator());
    }

    // And convert the BranchInst to the existing branch target to a
    // SelectInst so we can append the other cases to it.
    scope.endBlock->getTerminator()->eraseFromParent();
    llvm::Value *sel =
        new llvm::LoadInst(scope.branchSelector, "", scope.endBlock);
    llvm::SwitchInst::Create(
        sel, scope.exitTargets[0].branchTarget,
        1, // Expected number of branches, only for pre-allocating.
        scope.endBlock);
  }

  // If we already know this branch target, figure out the branch selector
  // value and simply insert the store into the source block (prior to the
  // last instruction, which is the branch to the first cleanup).
  for (unsigned i = 0; i < scope.exitTargets.size(); ++i) {
    CleanupExitTarget &t = scope.exitTargets[i];
    if (t.branchTarget == continueWith) {
      new llvm::StoreInst(DtoConstUint(i), scope.branchSelector,
                          sourceBlock->getTerminator());

      // Note: Strictly speaking, keeping this up to date would not be
      // needed right now, because we never to any optimizations that
      // require changes to the source blocks after the initial conversion
      // from one to two branch targets. Keeping this around for now to
      // ease future development, but may be removed to save some work.
      t.sourceBlocks.push_back(sourceBlock);

      return;
    }
  }

  // We don't know this branch target yet, so add it to the SwitchInst...
  llvm::ConstantInt *const selectorVal = DtoConstUint(scope.exitTargets.size());
  llvm::cast<llvm::SwitchInst>(scope.endBlock->getTerminator())
      ->addCase(selectorVal, continueWith);

  // ... insert the store into the source block...
  new llvm::StoreInst(selectorVal, scope.branchSelector,
                      sourceBlock->getTerminator());

  // ... and keep track of it (again, this is unnecessary right now as
  // discussed in the above note).
  scope.exitTargets.push_back(CleanupExitTarget(continueWith));
  scope.exitTargets.back().sourceBlocks.push_back(sourceBlock);
}
}

ScopeStack::~ScopeStack() {
  // If there are still unresolved gotos left, it means that they were either
  // down or "sideways" (i.e. down another branch) of the tree of all
  // cleanup scopes, both of which are not allowed in D.
  if (!topLevelUnresolvedGotos.empty()) {
    for (const auto &i : topLevelUnresolvedGotos) {
      error(i.sourceLoc, "goto into try/finally scope is not allowed");
    }
    fatal();
  }
}

void ScopeStack::pushCleanup(llvm::BasicBlock *beginBlock,
                             llvm::BasicBlock *endBlock) {
  cleanupScopes.push_back(CleanupScope(beginBlock, endBlock));
}

void ScopeStack::runCleanups(CleanupCursor sourceScope,
                             CleanupCursor targetScope,
                             llvm::BasicBlock *continueWith) {
#if LDC_LLVM_VER >= 308
  if (useMSVCEH()) {
    runCleanupCopies(sourceScope, targetScope, continueWith);
    return;
  }
#endif
  assert(targetScope <= sourceScope);

  if (targetScope == sourceScope) {
    // No cleanups to run, just branch to the next block.
    irs->ir->CreateBr(continueWith);
    return;
  }

  // Insert the unconditional branch to the first cleanup block.
  irs->ir->CreateBr(cleanupScopes[sourceScope - 1].beginBlock);

  // Update all the control flow in the cleanups to make sure we end up where
  // we want.
  for (CleanupCursor i = sourceScope; i-- > targetScope;) {
    llvm::BasicBlock *nextBlock =
        (i > targetScope) ? cleanupScopes[i - 1].beginBlock : continueWith;
    executeCleanup(irs, cleanupScopes[i], irs->scopebb(), nextBlock);
  }
}

#if LDC_LLVM_VER >= 308
void ScopeStack::runCleanupCopies(CleanupCursor sourceScope,
                                  CleanupCursor targetScope,
                                  llvm::BasicBlock *continueWith) {
  assert(targetScope <= sourceScope);

  // work through the blocks in reverse execution order, so we
  // can merge cleanups that end up at the same continuation target
  for (CleanupCursor i = targetScope; i < sourceScope; ++i)
    continueWith = executeCleanupCopying(irs, cleanupScopes[i], irs->scopebb(),
                                         continueWith, nullptr, nullptr);

  // Insert the unconditional branch to the first cleanup block.
  irs->ir->CreateBr(continueWith);
}

llvm::BasicBlock *ScopeStack::runCleanupPad(CleanupCursor scope,
                                            llvm::BasicBlock *unwindTo) {
  // a catch switch never needs to be cloned and is an unwind target itself
  if (isCatchSwitchBlock(cleanupScopes[scope].beginBlock))
    return cleanupScopes[scope].beginBlock;


  // each cleanup block is bracketed by a pair of cleanuppad/cleanupret
  // instructions, any unwinding should also just continue at the next
  // cleanup block, e.g.:
  //
  // cleanuppad:
  //   %0 = cleanuppad within %funclet[]
  //   %frame = nullptr
  //   if (!_d_enter_cleanup(%frame)) br label %cleanupret 
  //                                  else br label %copy
  //
  // copy:
  //   invoke _dtor to %cleanupret unwind %unwindTo [ "funclet"(token %0) ]
  //
  // cleanupret:
  //   _d_leave_cleanup(%frame)
  //   cleanupret %0 unwind %unwindTo
  //
  llvm::BasicBlock *cleanupbb =
      llvm::BasicBlock::Create(irs->context(), "cleanuppad", irs->topfunc());
  auto cleanuppad =
      llvm::CleanupPadInst::Create(getFuncletToken(), {}, "", cleanupbb);

  llvm::BasicBlock *cleanupret =
      llvm::BasicBlock::Create(irs->context(), "cleanupret", irs->topfunc());

  // preparation to allocate some space on the stack where _d_enter_cleanup 
  //  can place an exception frame (but not done here)
  auto frame = getNullPtr(getVoidPtrType());

  auto endFn = getRuntimeFunction(Loc(), irs->module, "_d_leave_cleanup");
  llvm::CallInst::Create(endFn, frame,
                         {llvm::OperandBundleDef("funclet", cleanuppad)}, "",
                         cleanupret);
  llvm::CleanupReturnInst::Create(cleanuppad, unwindTo, cleanupret);

  auto copybb = executeCleanupCopying(irs, cleanupScopes[scope], cleanupbb,
                                      cleanupret, unwindTo, cleanuppad);

  auto beginFn = getRuntimeFunction(Loc(), irs->module, "_d_enter_cleanup");
  auto exec = llvm::CallInst::Create(
      beginFn, frame, {llvm::OperandBundleDef("funclet", cleanuppad)}, "",
      cleanupbb);
  llvm::BranchInst::Create(copybb, cleanupret, exec, cleanupbb);

  return cleanupbb;
}
#endif

void ScopeStack::runAllCleanups(llvm::BasicBlock *continueWith) {
  runCleanups(0, continueWith);
}

void ScopeStack::popCleanups(CleanupCursor targetScope) {
  assert(targetScope <= currentCleanupScope());
  if (targetScope == currentCleanupScope()) {
    return;
  }

  for (CleanupCursor i = currentCleanupScope(); i-- > targetScope;) {
    // Any gotos that are still unresolved necessarily leave this scope.
    // Thus, the cleanup needs to be executed.
    for (const auto &gotoJump : currentUnresolvedGotos()) {
      // Make the source resp. last cleanup branch to this one.
      llvm::BasicBlock *tentative = gotoJump.tentativeTarget;
#if LDC_LLVM_VER >= 308
      if (useMSVCEH()) {
        llvm::BasicBlock *continueWith =
          llvm::BasicBlock::Create(irs->context(), "jumpcleanup", irs->topfunc());
        auto startCleanup =
            executeCleanupCopying(irs, cleanupScopes[i], gotoJump.sourceBlock,
                                  continueWith, nullptr, nullptr);
        tentative->replaceAllUsesWith(startCleanup);
        llvm::BranchInst::Create(tentative, continueWith);
      } else
#endif
      {
        tentative->replaceAllUsesWith(cleanupScopes[i].beginBlock);

        // And continue execution with the tentative target (we simply reuse
        // it because there is no reason not to).
        executeCleanup(irs, cleanupScopes[i], gotoJump.sourceBlock, tentative);
      }
    }

    std::vector<GotoJump> &nextUnresolved =
        (i == 0) ? topLevelUnresolvedGotos
                 : cleanupScopes[i - 1].unresolvedGotos;
    nextUnresolved.insert(nextUnresolved.end(),
                          currentUnresolvedGotos().begin(),
                          currentUnresolvedGotos().end());

    cleanupScopes.pop_back();
  }
}

void ScopeStack::pushCatch(llvm::Constant *classInfoPtr,
                           llvm::BasicBlock *bodyBlock) {
  if (useMSVCEH()) {
#if LDC_LLVM_VER >= 308
    assert(isCatchSwitchBlock(bodyBlock));
    pushCleanup(bodyBlock, bodyBlock);
#endif
  } else {
    catchScopes.emplace_back(classInfoPtr, bodyBlock, currentCleanupScope());
    currentLandingPads().push_back(nullptr);
  }
}

void ScopeStack::popCatch() {
  if (useMSVCEH()) {
#if LDC_LLVM_VER >= 308
    assert(isCatchSwitchBlock(cleanupScopes.back().beginBlock));
    popCleanups(currentCleanupScope() - 1);
#endif
  } else {
    catchScopes.pop_back();
    currentLandingPads().pop_back();
  }
}

void ScopeStack::pushLoopTarget(Statement *loopStatement,
                                llvm::BasicBlock *continueTarget,
                                llvm::BasicBlock *breakTarget) {
  continueTargets.emplace_back(continueTarget, currentCleanupScope(),
                               loopStatement);
  breakTargets.emplace_back(breakTarget, currentCleanupScope(), loopStatement);
}

void ScopeStack::popLoopTarget() {
  continueTargets.pop_back();
  breakTargets.pop_back();
}

void ScopeStack::pushBreakTarget(Statement *switchStatement,
                                 llvm::BasicBlock *targetBlock) {
  breakTargets.push_back({targetBlock, currentCleanupScope(), switchStatement});
}

void ScopeStack::popBreakTarget() { breakTargets.pop_back(); }

void ScopeStack::addLabelTarget(Identifier *labelName,
                                llvm::BasicBlock *targetBlock) {
  labelTargets[labelName] = {targetBlock, currentCleanupScope(), nullptr};

  // See whether any of the unresolved gotos target this label, and resolve
  // those that do.
  std::vector<GotoJump> &unresolved = currentUnresolvedGotos();
  size_t i = 0;
  while (i < unresolved.size()) {
    if (unresolved[i].targetLabel != labelName) {
      ++i;
      continue;
    }

    unresolved[i].tentativeTarget->replaceAllUsesWith(targetBlock);
    unresolved[i].tentativeTarget->eraseFromParent();
    unresolved.erase(unresolved.begin() + i);
  }
}

void ScopeStack::jumpToLabel(Loc loc, Identifier *labelName) {
  // If we have already seen that label, branch to it, executing any cleanups
  // as necessary.
  auto it = labelTargets.find(labelName);
  if (it != labelTargets.end()) {
    runCleanups(it->second.cleanupScope, it->second.targetBlock);
    return;
  }

  llvm::BasicBlock *target = llvm::BasicBlock::Create(
      irs->context(), "goto.unresolved", irs->topfunc());
  irs->ir->CreateBr(target);
  currentUnresolvedGotos().emplace_back(loc, irs->scopebb(), target, labelName);
}

void ScopeStack::jumpToStatement(std::vector<JumpTarget> &targets,
                                 Statement *loopOrSwitchStatement) {
  for (auto it = targets.rbegin(), end = targets.rend(); it != end; ++it) {
    if (it->targetStatement == loopOrSwitchStatement) {
      runCleanups(it->cleanupScope, it->targetBlock);
      return;
    }
  }
  assert(false && "Target for labeled break not found.");
}

void ScopeStack::jumpToClosest(std::vector<JumpTarget> &targets) {
  assert(!targets.empty() &&
         "Encountered break/continue but no loop in scope.");
  JumpTarget &t = targets.back();
  runCleanups(t.cleanupScope, t.targetBlock);
}

std::vector<GotoJump> &ScopeStack::currentUnresolvedGotos() {
  return cleanupScopes.empty() ? topLevelUnresolvedGotos
                               : cleanupScopes.back().unresolvedGotos;
}

std::vector<llvm::BasicBlock *> &ScopeStack::currentLandingPads() {
  return cleanupScopes.empty() ? topLevelLandingPads
                               : cleanupScopes.back().landingPads;
}

llvm::BasicBlock *&ScopeStack::getLandingPadRef(CleanupCursor scope) {
  auto &pads = cleanupScopes.empty() ? topLevelLandingPads
                                     : cleanupScopes[scope].landingPads;
  if (pads.empty()) {
    // Have not encountered any catches (for which we would push a scope) or
    // calls to throwing functions (where we would have already executed
    // this if) in this cleanup scope yet.
    pads.push_back(nullptr);
  }
  return pads.back();
}

llvm::BasicBlock *ScopeStack::getLandingPad() {
  llvm::BasicBlock *&landingPad = getLandingPadRef(currentCleanupScope() - 1);
  if (!landingPad) {
#if LDC_LLVM_VER >= 308
    if (useMSVCEH()) {
      assert(currentCleanupScope() > 0);
      landingPad = emitLandingPadMSVCEH(currentCleanupScope() - 1);
    } else
#endif
      landingPad = emitLandingPad();
  }
  return landingPad;
}

namespace {
llvm::LandingPadInst *createLandingPadInst(IRState *irs) {
  LLType *retType =
      LLStructType::get(LLType::getInt8PtrTy(irs->context()),
                        LLType::getInt32Ty(irs->context()), nullptr);
#if LDC_LLVM_VER >= 307
  LLFunction *currentFunction = irs->func()->func;
  if (!currentFunction->hasPersonalityFn()) {
    LLFunction *personalityFn =
        getRuntimeFunction(Loc(), irs->module, "_d_eh_personality");
    currentFunction->setPersonalityFn(personalityFn);
  }
  return irs->ir->CreateLandingPad(retType, 0);
#else
  LLFunction *personalityFn =
      getRuntimeFunction(Loc(), irs->module, "_d_eh_personality");
  return irs->ir->CreateLandingPad(retType, personalityFn, 0);
#endif
}
}

#if LDC_LLVM_VER >= 308
llvm::BasicBlock *ScopeStack::emitLandingPadMSVCEH(CleanupCursor scope) {

  LLFunction *currentFunction = irs->func()->func;
  if (!currentFunction->hasPersonalityFn()) {
    const char *personality = "__CxxFrameHandler3";
    LLFunction *personalityFn =
        getRuntimeFunction(Loc(), irs->module, personality);
    currentFunction->setPersonalityFn(personalityFn);
  }

  if (scope == 0)
    return runCleanupPad(scope, nullptr);

  llvm::BasicBlock *&pad = getLandingPadRef(scope - 1);
  if (!pad)
    pad = emitLandingPadMSVCEH(scope - 1);

  return runCleanupPad(scope, pad);
}
#endif

llvm::BasicBlock *ScopeStack::emitLandingPad() {
  // save and rewrite scope
  IRScope savedIRScope = irs->scope();

  llvm::BasicBlock *beginBB =
      llvm::BasicBlock::Create(irs->context(), "landingPad", irs->topfunc());
  irs->scope() = IRScope(beginBB);

  llvm::LandingPadInst *landingPad = createLandingPadInst(irs);

  // Stash away the exception object pointer and selector value into their
  // stack slots.
  llvm::Value *ehPtr = DtoExtractValue(landingPad, 0);
  irs->ir->CreateStore(ehPtr, irs->func()->getOrCreateEhPtrSlot());

  llvm::Value *ehSelector = DtoExtractValue(landingPad, 1);
  if (!irs->func()->ehSelectorSlot) {
    irs->func()->ehSelectorSlot =
        DtoRawAlloca(ehSelector->getType(), 0, "eh.selector");
  }
  irs->ir->CreateStore(ehSelector, irs->func()->ehSelectorSlot);

  // Add landingpad clauses, emit finallys and 'if' chain to catch the
  // exception.
  CleanupCursor lastCleanup = currentCleanupScope();
  for (auto it = catchScopes.rbegin(), end = catchScopes.rend(); it != end;
       ++it) {
    // Insert any cleanups in between the last catch we ran (i.e. tested for
    // and found that the type does not match) and this one.
    assert(lastCleanup >= it->cleanupScope);
    if (lastCleanup > it->cleanupScope) {
      landingPad->setCleanup(true);
      llvm::BasicBlock *afterCleanupBB = llvm::BasicBlock::Create(
          irs->context(), beginBB->getName() + llvm::Twine(".after.cleanup"),
          irs->topfunc());
      runCleanups(lastCleanup, it->cleanupScope, afterCleanupBB);
      irs->scope() = IRScope(afterCleanupBB);
      lastCleanup = it->cleanupScope;
    }

    // Add the ClassInfo reference to the landingpad instruction so it is
    // emitted to the EH tables.
    landingPad->addClause(it->classInfoPtr);

    llvm::BasicBlock *mismatchBB = llvm::BasicBlock::Create(
        irs->context(), beginBB->getName() + llvm::Twine(".mismatch"),
        irs->topfunc());

    // "Call" llvm.eh.typeid.for, which gives us the eh selector value to
    // compare the landing pad selector value with.
    llvm::Value *ehTypeId =
        irs->ir->CreateCall(GET_INTRINSIC_DECL(eh_typeid_for),
                            DtoBitCast(it->classInfoPtr, getVoidPtrType()));

    // Compare the selector value from the unwinder against the expected
    // one and branch accordingly.
    irs->ir->CreateCondBr(
        irs->ir->CreateICmpEQ(irs->ir->CreateLoad(irs->func()->ehSelectorSlot),
                              ehTypeId),
        it->bodyBlock, mismatchBB);
    irs->scope() = IRScope(mismatchBB);
  }

  // No catch matched. Execute all finallys and resume unwinding.
  if (lastCleanup > 0) {
    landingPad->setCleanup(true);
    runCleanups(lastCleanup, 0, irs->func()->getOrCreateResumeUnwindBlock());
  } else if (!catchScopes.empty()) {
    // Directly convert the last mismatch branch into a branch to the
    // unwind resume block.
    irs->scopebb()->replaceAllUsesWith(
        irs->func()->getOrCreateResumeUnwindBlock());
    irs->scopebb()->eraseFromParent();
  } else {
    irs->ir->CreateBr(irs->func()->getOrCreateResumeUnwindBlock());
  }

  irs->scope() = savedIRScope;
  return beginBB;
}

IrFunction::IrFunction(FuncDeclaration *fd) : FMF() {
  decl = fd;

  Type *t = fd->type->toBasetype();
  assert(t->ty == Tfunction);
  type = static_cast<TypeFunction *>(t);
}

void IrFunction::setNeverInline() {
  assert(!func->getAttributes().hasAttribute(llvm::AttributeSet::FunctionIndex,
                                             llvm::Attribute::AlwaysInline) &&
         "function can't be never- and always-inline at the same time");
  func->addFnAttr(llvm::Attribute::NoInline);
}

void IrFunction::setAlwaysInline() {
  assert(!func->getAttributes().hasAttribute(llvm::AttributeSet::FunctionIndex,
                                             llvm::Attribute::NoInline) &&
         "function can't be never- and always-inline at the same time");
  func->addFnAttr(llvm::Attribute::AlwaysInline);
}

llvm::AllocaInst *IrFunction::getOrCreateEhPtrSlot() {
  if (!ehPtrSlot) {
    ehPtrSlot = DtoRawAlloca(getVoidPtrType(), 0, "eh.ptr");
  }
  return ehPtrSlot;
}

llvm::BasicBlock *IrFunction::getOrCreateResumeUnwindBlock() {
  assert(func == gIR->topfunc() &&
         "Should only access unwind resume block while emitting function.");
  if (!resumeUnwindBlock) {
    resumeUnwindBlock =
        llvm::BasicBlock::Create(gIR->context(), "eh.resume", func);

    llvm::BasicBlock *oldBB = gIR->scopebb();
    gIR->scope() = IRScope(resumeUnwindBlock);

    llvm::Function *resumeFn =
        getRuntimeFunction(Loc(), gIR->module, "_d_eh_resume_unwind");
    gIR->ir->CreateCall(resumeFn, DtoLoad(getOrCreateEhPtrSlot()));
    gIR->ir->CreateUnreachable();

    gIR->scope() = IRScope(oldBB);
  }
  return resumeUnwindBlock;
}

IrFunction *getIrFunc(FuncDeclaration *decl, bool create) {
  if (!isIrFuncCreated(decl) && create) {
    assert(decl->ir->irFunc == NULL);
    decl->ir->irFunc = new IrFunction(decl);
    decl->ir->m_type = IrDsymbol::FuncType;
  }
  assert(decl->ir->irFunc != NULL);
  return decl->ir->irFunc;
}

bool isIrFuncCreated(FuncDeclaration *decl) {
  assert(decl);
  assert(decl->ir);
  IrDsymbol::Type t = decl->ir->type();
  assert(t == IrDsymbol::FuncType || t == IrDsymbol::NotSet);
  return t == IrDsymbol::FuncType;
}
