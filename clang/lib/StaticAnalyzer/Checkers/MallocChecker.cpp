//=== MallocChecker.cpp - A malloc/free checker -------------------*- C++ -*--//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines malloc/free checker, which checks for potential memory
// leaks, double free, and use-after-free problems.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
using namespace clang;
using namespace ento;

namespace {

class RefState {
  enum Kind { AllocateUnchecked, AllocateFailed, Released, Escaped,
              Relinquished } K;
  const Stmt *S;

public:
  RefState(Kind k, const Stmt *s) : K(k), S(s) {}

  bool isAllocated() const { return K == AllocateUnchecked; }
  //bool isFailed() const { return K == AllocateFailed; }
  bool isReleased() const { return K == Released; }
  //bool isEscaped() const { return K == Escaped; }
  //bool isRelinquished() const { return K == Relinquished; }

  bool operator==(const RefState &X) const {
    return K == X.K && S == X.S;
  }

  static RefState getAllocateUnchecked(const Stmt *s) { 
    return RefState(AllocateUnchecked, s); 
  }
  static RefState getAllocateFailed() {
    return RefState(AllocateFailed, 0);
  }
  static RefState getReleased(const Stmt *s) { return RefState(Released, s); }
  static RefState getEscaped(const Stmt *s) { return RefState(Escaped, s); }
  static RefState getRelinquished(const Stmt *s) {
    return RefState(Relinquished, s);
  }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(K);
    ID.AddPointer(S);
  }
};

class RegionState {};

class MallocChecker : public Checker<check::DeadSymbols,
                                     check::EndPath,
                                     check::PreStmt<ReturnStmt>,
                                     check::PostStmt<CallExpr>,
                                     check::Location,
                                     check::Bind,
                                     eval::Assume>
{
  mutable OwningPtr<BuiltinBug> BT_DoubleFree;
  mutable OwningPtr<BuiltinBug> BT_Leak;
  mutable OwningPtr<BuiltinBug> BT_UseFree;
  mutable OwningPtr<BuiltinBug> BT_UseRelinquished;
  mutable OwningPtr<BuiltinBug> BT_BadFree;
  mutable IdentifierInfo *II_malloc, *II_free, *II_realloc, *II_calloc;

public:
  MallocChecker() : II_malloc(0), II_free(0), II_realloc(0), II_calloc(0) {}

  /// In pessimistic mode, the checker assumes that it does not know which
  /// functions might free the memory.
  struct ChecksFilter {
    DefaultBool CMallocPessimistic;
    DefaultBool CMallocOptimistic;
  };

  ChecksFilter Filter;

  void initIdentifierInfo(CheckerContext &C) const;

  void checkPostStmt(const CallExpr *CE, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SymReaper, CheckerContext &C) const;
  void checkEndPath(CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *S, CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef state, SVal Cond,
                            bool Assumption) const;
  void checkLocation(SVal l, bool isLoad, const Stmt *S,
                     CheckerContext &C) const;
  void checkBind(SVal location, SVal val, const Stmt*S,
                 CheckerContext &C) const;

private:
  static void MallocMem(CheckerContext &C, const CallExpr *CE);
  static void MallocMemReturnsAttr(CheckerContext &C, const CallExpr *CE,
                                   const OwnershipAttr* Att);
  static ProgramStateRef MallocMemAux(CheckerContext &C, const CallExpr *CE,
                                     const Expr *SizeEx, SVal Init,
                                     ProgramStateRef state) {
    return MallocMemAux(C, CE,
                        state->getSVal(SizeEx, C.getLocationContext()),
                        Init, state);
  }
  static ProgramStateRef MallocMemAux(CheckerContext &C, const CallExpr *CE,
                                     SVal SizeEx, SVal Init,
                                     ProgramStateRef state);

  void FreeMem(CheckerContext &C, const CallExpr *CE) const;
  void FreeMemAttr(CheckerContext &C, const CallExpr *CE,
                   const OwnershipAttr* Att) const;
  ProgramStateRef FreeMemAux(CheckerContext &C, const CallExpr *CE,
                                 ProgramStateRef state, unsigned Num,
                                 bool Hold) const;

  void ReallocMem(CheckerContext &C, const CallExpr *CE) const;
  static void CallocMem(CheckerContext &C, const CallExpr *CE);
  
  bool checkEscape(SymbolRef Sym, const Stmt *S, CheckerContext &C) const;
  bool checkUseAfterFree(SymbolRef Sym, CheckerContext &C,
                         const Stmt *S = 0) const;

  static bool SummarizeValue(raw_ostream &os, SVal V);
  static bool SummarizeRegion(raw_ostream &os, const MemRegion *MR);
  void ReportBadFree(CheckerContext &C, SVal ArgVal, SourceRange range) const;

  /// The bug visitor which allows us to print extra diagnostics along the
  /// BugReport path. For example, showing the allocation site of the leaked
  /// region.
  class MallocBugVisitor : public BugReporterVisitor {
  protected:
    // The allocated region symbol tracked by the main analysis.
    SymbolRef Sym;

  public:
    MallocBugVisitor(SymbolRef S) : Sym(S) {}
    virtual ~MallocBugVisitor() {}

    void Profile(llvm::FoldingSetNodeID &ID) const {
      static int X = 0;
      ID.AddPointer(&X);
      ID.AddPointer(Sym);
    }

    inline bool isAllocated(const RefState *S, const RefState *SPrev) {
      // Did not track -> allocated. Other state (released) -> allocated.
      return ((S && S->isAllocated()) && (!SPrev || !SPrev->isAllocated()));
    }

    inline bool isReleased(const RefState *S, const RefState *SPrev) {
      // Did not track -> released. Other state (allocated) -> released.
      return ((S && S->isReleased()) && (!SPrev || !SPrev->isReleased()));
    }

    PathDiagnosticPiece *VisitNode(const ExplodedNode *N,
                                   const ExplodedNode *PrevN,
                                   BugReporterContext &BRC,
                                   BugReport &BR);
  };
};
} // end anonymous namespace

typedef llvm::ImmutableMap<SymbolRef, RefState> RegionStateTy;

namespace clang {
namespace ento {
  template <>
  struct ProgramStateTrait<RegionState> 
    : public ProgramStatePartialTrait<RegionStateTy> {
    static void *GDMIndex() { static int x; return &x; }
  };
}
}

void MallocChecker::initIdentifierInfo(CheckerContext &C) const {
  ASTContext &Ctx = C.getASTContext();
  if (!II_malloc)
    II_malloc = &Ctx.Idents.get("malloc");
  if (!II_free)
    II_free = &Ctx.Idents.get("free");
  if (!II_realloc)
    II_realloc = &Ctx.Idents.get("realloc");
  if (!II_calloc)
    II_calloc = &Ctx.Idents.get("calloc");
}

void MallocChecker::checkPostStmt(const CallExpr *CE, CheckerContext &C) const {
  const FunctionDecl *FD = C.getCalleeDecl(CE);
  if (!FD)
    return;
  initIdentifierInfo(C);

  if (FD->getIdentifier() == II_malloc) {
    MallocMem(C, CE);
    return;
  }
  if (FD->getIdentifier() == II_realloc) {
    ReallocMem(C, CE);
    return;
  }

  if (FD->getIdentifier() == II_calloc) {
    CallocMem(C, CE);
    return;
  }

  if (FD->getIdentifier() == II_free) {
    FreeMem(C, CE);
    return;
  }

  if (Filter.CMallocOptimistic)
  // Check all the attributes, if there are any.
  // There can be multiple of these attributes.
  if (FD->hasAttrs()) {
    for (specific_attr_iterator<OwnershipAttr>
                  i = FD->specific_attr_begin<OwnershipAttr>(),
                  e = FD->specific_attr_end<OwnershipAttr>();
         i != e; ++i) {
      switch ((*i)->getOwnKind()) {
      case OwnershipAttr::Returns: {
        MallocMemReturnsAttr(C, CE, *i);
        break;
      }
      case OwnershipAttr::Takes:
      case OwnershipAttr::Holds: {
        FreeMemAttr(C, CE, *i);
        break;
      }
      }
    }
  }

  if (Filter.CMallocPessimistic) {
    ProgramStateRef State = C.getState();
    // The pointer might escape through a function call.
    for (CallExpr::const_arg_iterator I = CE->arg_begin(),
                                      E = CE->arg_end(); I != E; ++I) {
      const Expr *A = *I;
      if (A->getType().getTypePtr()->isAnyPointerType()) {
        SymbolRef Sym = State->getSVal(A, C.getLocationContext()).getAsSymbol();
        if (!Sym)
          return;
        checkEscape(Sym, A, C);
        checkUseAfterFree(Sym, C, A);
      }
    }
  }
}

void MallocChecker::MallocMem(CheckerContext &C, const CallExpr *CE) {
  ProgramStateRef state = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(),
                                      C.getState());
  C.addTransition(state);
}

void MallocChecker::MallocMemReturnsAttr(CheckerContext &C, const CallExpr *CE,
                                         const OwnershipAttr* Att) {
  if (Att->getModule() != "malloc")
    return;

  OwnershipAttr::args_iterator I = Att->args_begin(), E = Att->args_end();
  if (I != E) {
    ProgramStateRef state =
        MallocMemAux(C, CE, CE->getArg(*I), UndefinedVal(), C.getState());
    C.addTransition(state);
    return;
  }
  ProgramStateRef state = MallocMemAux(C, CE, UnknownVal(), UndefinedVal(),
                                        C.getState());
  C.addTransition(state);
}

ProgramStateRef MallocChecker::MallocMemAux(CheckerContext &C,
                                           const CallExpr *CE,
                                           SVal Size, SVal Init,
                                           ProgramStateRef state) {
  SValBuilder &svalBuilder = C.getSValBuilder();

  // Get the return value.
  SVal retVal = state->getSVal(CE, C.getLocationContext());

  // Fill the region with the initialization value.
  state = state->bindDefault(retVal, Init);

  // Set the region's extent equal to the Size parameter.
  const SymbolicRegion *R = cast<SymbolicRegion>(retVal.getAsRegion());
  DefinedOrUnknownSVal Extent = R->getExtent(svalBuilder);
  DefinedOrUnknownSVal DefinedSize = cast<DefinedOrUnknownSVal>(Size);
  DefinedOrUnknownSVal extentMatchesSize =
    svalBuilder.evalEQ(state, Extent, DefinedSize);

  state = state->assume(extentMatchesSize, true);
  assert(state);
  
  SymbolRef Sym = retVal.getAsLocSymbol();
  assert(Sym);

  // Set the symbol's state to Allocated.
  return state->set<RegionState>(Sym, RefState::getAllocateUnchecked(CE));
}

void MallocChecker::FreeMem(CheckerContext &C, const CallExpr *CE) const {
  ProgramStateRef state = FreeMemAux(C, CE, C.getState(), 0, false);

  if (state)
    C.addTransition(state);
}

void MallocChecker::FreeMemAttr(CheckerContext &C, const CallExpr *CE,
                                const OwnershipAttr* Att) const {
  if (Att->getModule() != "malloc")
    return;

  for (OwnershipAttr::args_iterator I = Att->args_begin(), E = Att->args_end();
       I != E; ++I) {
    ProgramStateRef state =
      FreeMemAux(C, CE, C.getState(), *I,
                 Att->getOwnKind() == OwnershipAttr::Holds);
    if (state)
      C.addTransition(state);
  }
}

ProgramStateRef MallocChecker::FreeMemAux(CheckerContext &C,
                                              const CallExpr *CE,
                                              ProgramStateRef state,
                                              unsigned Num,
                                              bool Hold) const {
  const Expr *ArgExpr = CE->getArg(Num);
  SVal ArgVal = state->getSVal(ArgExpr, C.getLocationContext());

  DefinedOrUnknownSVal location = cast<DefinedOrUnknownSVal>(ArgVal);

  // Check for null dereferences.
  if (!isa<Loc>(location))
    return 0;

  // FIXME: Technically using 'Assume' here can result in a path
  //  bifurcation.  In such cases we need to return two states, not just one.
  ProgramStateRef notNullState, nullState;
  llvm::tie(notNullState, nullState) = state->assume(location);

  // The explicit NULL case, no operation is performed.
  if (nullState && !notNullState)
    return 0;

  assert(notNullState);

  // Unknown values could easily be okay
  // Undefined values are handled elsewhere
  if (ArgVal.isUnknownOrUndef())
    return 0;

  const MemRegion *R = ArgVal.getAsRegion();
  
  // Nonlocs can't be freed, of course.
  // Non-region locations (labels and fixed addresses) also shouldn't be freed.
  if (!R) {
    ReportBadFree(C, ArgVal, ArgExpr->getSourceRange());
    return 0;
  }
  
  R = R->StripCasts();
  
  // Blocks might show up as heap data, but should not be free()d
  if (isa<BlockDataRegion>(R)) {
    ReportBadFree(C, ArgVal, ArgExpr->getSourceRange());
    return 0;
  }
  
  const MemSpaceRegion *MS = R->getMemorySpace();
  
  // Parameters, locals, statics, and globals shouldn't be freed.
  if (!(isa<UnknownSpaceRegion>(MS) || isa<HeapSpaceRegion>(MS))) {
    // FIXME: at the time this code was written, malloc() regions were
    // represented by conjured symbols, which are all in UnknownSpaceRegion.
    // This means that there isn't actually anything from HeapSpaceRegion
    // that should be freed, even though we allow it here.
    // Of course, free() can work on memory allocated outside the current
    // function, so UnknownSpaceRegion is always a possibility.
    // False negatives are better than false positives.
    
    ReportBadFree(C, ArgVal, ArgExpr->getSourceRange());
    return 0;
  }
  
  const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(R);
  // Various cases could lead to non-symbol values here.
  // For now, ignore them.
  if (!SR)
    return 0;

  SymbolRef Sym = SR->getSymbol();
  const RefState *RS = state->get<RegionState>(Sym);

  // If the symbol has not been tracked, return. This is possible when free() is
  // called on a pointer that does not get its pointee directly from malloc(). 
  // Full support of this requires inter-procedural analysis.
  if (!RS)
    return 0;

  // Check double free.
  if (RS->isReleased()) {
    if (ExplodedNode *N = C.generateSink()) {
      if (!BT_DoubleFree)
        BT_DoubleFree.reset(
          new BuiltinBug("Double free",
                         "Try to free a memory block that has been released"));
      BugReport *R = new BugReport(*BT_DoubleFree, 
                                   BT_DoubleFree->getDescription(), N);
      R->addVisitor(new MallocBugVisitor(Sym));
      C.EmitReport(R);
    }
    return 0;
  }

  // Normal free.
  if (Hold)
    return notNullState->set<RegionState>(Sym, RefState::getRelinquished(CE));
  return notNullState->set<RegionState>(Sym, RefState::getReleased(CE));
}

bool MallocChecker::SummarizeValue(raw_ostream &os, SVal V) {
  if (nonloc::ConcreteInt *IntVal = dyn_cast<nonloc::ConcreteInt>(&V))
    os << "an integer (" << IntVal->getValue() << ")";
  else if (loc::ConcreteInt *ConstAddr = dyn_cast<loc::ConcreteInt>(&V))
    os << "a constant address (" << ConstAddr->getValue() << ")";
  else if (loc::GotoLabel *Label = dyn_cast<loc::GotoLabel>(&V))
    os << "the address of the label '" << Label->getLabel()->getName() << "'";
  else
    return false;
  
  return true;
}

bool MallocChecker::SummarizeRegion(raw_ostream &os,
                                    const MemRegion *MR) {
  switch (MR->getKind()) {
  case MemRegion::FunctionTextRegionKind: {
    const FunctionDecl *FD = cast<FunctionTextRegion>(MR)->getDecl();
    if (FD)
      os << "the address of the function '" << *FD << '\'';
    else
      os << "the address of a function";
    return true;
  }
  case MemRegion::BlockTextRegionKind:
    os << "block text";
    return true;
  case MemRegion::BlockDataRegionKind:
    // FIXME: where the block came from?
    os << "a block";
    return true;
  default: {
    const MemSpaceRegion *MS = MR->getMemorySpace();
    
    if (isa<StackLocalsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = NULL;
      
      if (VD)
        os << "the address of the local variable '" << VD->getName() << "'";
      else
        os << "the address of a local stack variable";
      return true;
    }

    if (isa<StackArgumentsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = NULL;
      
      if (VD)
        os << "the address of the parameter '" << VD->getName() << "'";
      else
        os << "the address of a parameter";
      return true;
    }

    if (isa<GlobalsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = NULL;
      
      if (VD) {
        if (VD->isStaticLocal())
          os << "the address of the static variable '" << VD->getName() << "'";
        else
          os << "the address of the global variable '" << VD->getName() << "'";
      } else
        os << "the address of a global variable";
      return true;
    }

    return false;
  }
  }
}

void MallocChecker::ReportBadFree(CheckerContext &C, SVal ArgVal,
                                  SourceRange range) const {
  if (ExplodedNode *N = C.generateSink()) {
    if (!BT_BadFree)
      BT_BadFree.reset(new BuiltinBug("Bad free"));
    
    SmallString<100> buf;
    llvm::raw_svector_ostream os(buf);
    
    const MemRegion *MR = ArgVal.getAsRegion();
    if (MR) {
      while (const ElementRegion *ER = dyn_cast<ElementRegion>(MR))
        MR = ER->getSuperRegion();
      
      // Special case for alloca()
      if (isa<AllocaRegion>(MR))
        os << "Argument to free() was allocated by alloca(), not malloc()";
      else {
        os << "Argument to free() is ";
        if (SummarizeRegion(os, MR))
          os << ", which is not memory allocated by malloc()";
        else
          os << "not memory allocated by malloc()";
      }
    } else {
      os << "Argument to free() is ";
      if (SummarizeValue(os, ArgVal))
        os << ", which is not memory allocated by malloc()";
      else
        os << "not memory allocated by malloc()";
    }
    
    BugReport *R = new BugReport(*BT_BadFree, os.str(), N);
    R->addRange(range);
    C.EmitReport(R);
  }
}

void MallocChecker::ReallocMem(CheckerContext &C, const CallExpr *CE) const {
  ProgramStateRef state = C.getState();
  const Expr *arg0Expr = CE->getArg(0);
  const LocationContext *LCtx = C.getLocationContext();
  DefinedOrUnknownSVal arg0Val 
    = cast<DefinedOrUnknownSVal>(state->getSVal(arg0Expr, LCtx));

  SValBuilder &svalBuilder = C.getSValBuilder();

  DefinedOrUnknownSVal PtrEQ =
    svalBuilder.evalEQ(state, arg0Val, svalBuilder.makeNull());

  // Get the size argument. If there is no size arg then give up.
  const Expr *Arg1 = CE->getArg(1);
  if (!Arg1)
    return;

  // Get the value of the size argument.
  DefinedOrUnknownSVal Arg1Val = 
    cast<DefinedOrUnknownSVal>(state->getSVal(Arg1, LCtx));

  // Compare the size argument to 0.
  DefinedOrUnknownSVal SizeZero =
    svalBuilder.evalEQ(state, Arg1Val,
                       svalBuilder.makeIntValWithPtrWidth(0, false));

  // If the ptr is NULL and the size is not 0, the call is equivalent to 
  // malloc(size).
  ProgramStateRef stateEqual = state->assume(PtrEQ, true);
  if (stateEqual && state->assume(SizeZero, false)) {
    // Hack: set the NULL symbolic region to released to suppress false warning.
    // In the future we should add more states for allocated regions, e.g., 
    // CheckedNull, CheckedNonNull.
    
    SymbolRef Sym = arg0Val.getAsLocSymbol();
    if (Sym)
      stateEqual = stateEqual->set<RegionState>(Sym, RefState::getReleased(CE));

    ProgramStateRef stateMalloc = MallocMemAux(C, CE, CE->getArg(1), 
                                              UndefinedVal(), stateEqual);
    C.addTransition(stateMalloc);
  }

  if (ProgramStateRef stateNotEqual = state->assume(PtrEQ, false)) {
    // If the size is 0, free the memory.
    if (ProgramStateRef stateSizeZero =
          stateNotEqual->assume(SizeZero, true))
      if (ProgramStateRef stateFree = 
          FreeMemAux(C, CE, stateSizeZero, 0, false)) {

        // Bind the return value to NULL because it is now free.
        C.addTransition(stateFree->BindExpr(CE, LCtx,
                                            svalBuilder.makeNull(), true));
      }
    if (ProgramStateRef stateSizeNotZero =
          stateNotEqual->assume(SizeZero,false))
      if (ProgramStateRef stateFree = FreeMemAux(C, CE, stateSizeNotZero,
                                                0, false)) {
        // FIXME: We should copy the content of the original buffer.
        ProgramStateRef stateRealloc = MallocMemAux(C, CE, CE->getArg(1), 
                                                   UnknownVal(), stateFree);
        C.addTransition(stateRealloc);
      }
  }
}

void MallocChecker::CallocMem(CheckerContext &C, const CallExpr *CE) {
  ProgramStateRef state = C.getState();
  SValBuilder &svalBuilder = C.getSValBuilder();
  const LocationContext *LCtx = C.getLocationContext();
  SVal count = state->getSVal(CE->getArg(0), LCtx);
  SVal elementSize = state->getSVal(CE->getArg(1), LCtx);
  SVal TotalSize = svalBuilder.evalBinOp(state, BO_Mul, count, elementSize,
                                        svalBuilder.getContext().getSizeType());  
  SVal zeroVal = svalBuilder.makeZeroVal(svalBuilder.getContext().CharTy);

  C.addTransition(MallocMemAux(C, CE, TotalSize, zeroVal, state));
}

void MallocChecker::checkDeadSymbols(SymbolReaper &SymReaper,
                                     CheckerContext &C) const
{
  if (!SymReaper.hasDeadSymbols())
    return;

  ProgramStateRef state = C.getState();
  RegionStateTy RS = state->get<RegionState>();
  RegionStateTy::Factory &F = state->get_context<RegionState>();

  bool generateReport = false;
  
  for (RegionStateTy::iterator I = RS.begin(), E = RS.end(); I != E; ++I) {
    if (SymReaper.isDead(I->first)) {
      if (I->second.isAllocated())
        generateReport = true;

      // Remove the dead symbol from the map.
      RS = F.remove(RS, I->first);

    }
  }
  
  ExplodedNode *N = C.addTransition(state->set<RegionState>(RS));

  // FIXME: This does not handle when we have multiple leaks at a single
  // place.
  // TODO: We don't have symbol info in the diagnostics here!
  if (N && generateReport) {
    if (!BT_Leak)
      BT_Leak.reset(new BuiltinBug("Memory leak",
              "Allocated memory never released. Potential memory leak."));
    // FIXME: where it is allocated.
    BugReport *R = new BugReport(*BT_Leak, BT_Leak->getDescription(), N);
    //Report->addVisitor(new MallocBugVisitor(Sym));
    C.EmitReport(R);
  }
}

void MallocChecker::checkEndPath(CheckerContext &Ctx) const {
  ProgramStateRef state = Ctx.getState();
  RegionStateTy M = state->get<RegionState>();

  for (RegionStateTy::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    RefState RS = I->second;
    if (RS.isAllocated()) {
      ExplodedNode *N = Ctx.addTransition(state);
      if (N) {
        if (!BT_Leak)
          BT_Leak.reset(new BuiltinBug("Memory leak",
                    "Allocated memory never released. Potential memory leak."));
        BugReport *R = new BugReport(*BT_Leak, BT_Leak->getDescription(), N);
        R->addVisitor(new MallocBugVisitor(I->first));
        Ctx.EmitReport(R);
      }
    }
  }
}

bool MallocChecker::checkEscape(SymbolRef Sym, const Stmt *S,
                                CheckerContext &C) const {
  ProgramStateRef state = C.getState();
  const RefState *RS = state->get<RegionState>(Sym);
  if (!RS)
    return false;

  if (RS->isAllocated()) {
    state = state->set<RegionState>(Sym, RefState::getEscaped(S));
    C.addTransition(state);
    return true;
  }
  return false;
}

void MallocChecker::checkPreStmt(const ReturnStmt *S, CheckerContext &C) const {
  const Expr *E = S->getRetValue();
  if (!E)
    return;
  SymbolRef Sym = C.getState()->getSVal(E, C.getLocationContext()).getAsSymbol();
  if (!Sym)
    return;

  checkEscape(Sym, S, C);
}

ProgramStateRef MallocChecker::evalAssume(ProgramStateRef state,
                                              SVal Cond, 
                                              bool Assumption) const {
  // If a symbolic region is assumed to NULL, set its state to AllocateFailed.
  // FIXME: should also check symbols assumed to non-null.

  RegionStateTy RS = state->get<RegionState>();

  for (RegionStateTy::iterator I = RS.begin(), E = RS.end(); I != E; ++I) {
    // If the symbol is assumed to NULL, this will return an APSInt*.
    if (state->getSymVal(I.getKey()))
      state = state->set<RegionState>(I.getKey(),RefState::getAllocateFailed());
  }

  return state;
}

bool MallocChecker::checkUseAfterFree(SymbolRef Sym, CheckerContext &C,
                                      const Stmt *S) const {
  assert(Sym);
  const RefState *RS = C.getState()->get<RegionState>(Sym);
  if (RS && RS->isReleased()) {
    if (ExplodedNode *N = C.addTransition()) {
      if (!BT_UseFree)
        BT_UseFree.reset(new BuiltinBug("Use dynamically allocated memory "
            "after it is freed."));

      BugReport *R = new BugReport(*BT_UseFree, BT_UseFree->getDescription(),N);
      if (S)
        R->addRange(S->getSourceRange());
      R->addVisitor(new MallocBugVisitor(Sym));
      C.EmitReport(R);
      return true;
    }
  }
  return false;
}

// Check if the location is a freed symbolic region.
void MallocChecker::checkLocation(SVal l, bool isLoad, const Stmt *S,
                                  CheckerContext &C) const {
  SymbolRef Sym = l.getLocSymbolInBase();
  if (Sym)
    checkUseAfterFree(Sym, C);
}

void MallocChecker::checkBind(SVal location, SVal val,
                              const Stmt *BindS, CheckerContext &C) const {
  // The PreVisitBind implements the same algorithm as already used by the 
  // Objective C ownership checker: if the pointer escaped from this scope by 
  // assignment, let it go.  However, assigning to fields of a stack-storage 
  // structure does not transfer ownership.

  ProgramStateRef state = C.getState();
  DefinedOrUnknownSVal l = cast<DefinedOrUnknownSVal>(location);

  // Check for null dereferences.
  if (!isa<Loc>(l))
    return;

  // Before checking if the state is null, check if 'val' has a RefState.
  // Only then should we check for null and bifurcate the state.
  SymbolRef Sym = val.getLocSymbolInBase();
  if (Sym) {
    if (const RefState *RS = state->get<RegionState>(Sym)) {
      // If ptr is NULL, no operation is performed.
      ProgramStateRef notNullState, nullState;
      llvm::tie(notNullState, nullState) = state->assume(l);

      // Generate a transition for 'nullState' to record the assumption
      // that the state was null.
      if (nullState)
        C.addTransition(nullState);

      if (!notNullState)
        return;

      if (RS->isAllocated()) {
        // Something we presently own is being assigned somewhere.
        const MemRegion *AR = location.getAsRegion();
        if (!AR)
          return;
        AR = AR->StripCasts()->getBaseRegion();
        do {
          // If it is on the stack, we still own it.
          if (AR->hasStackNonParametersStorage())
            break;

          // If the state can't represent this binding, we still own it.
          if (notNullState == (notNullState->bindLoc(cast<Loc>(location),
                                                     UnknownVal())))
            break;

          // We no longer own this pointer.
          notNullState =
            notNullState->set<RegionState>(Sym,
                                        RefState::getRelinquished(BindS));
        }
        while (false);
      }
      C.addTransition(notNullState);
    }
  }
}

PathDiagnosticPiece *
MallocChecker::MallocBugVisitor::VisitNode(const ExplodedNode *N,
                                           const ExplodedNode *PrevN,
                                           BugReporterContext &BRC,
                                           BugReport &BR) {
  const RefState *RS = N->getState()->get<RegionState>(Sym);
  const RefState *RSPrev = PrevN->getState()->get<RegionState>(Sym);
  if (!RS && !RSPrev)
    return 0;

  // We expect the interesting locations be StmtPoints corresponding to call
  // expressions. We do not support indirect function calls as of now.
  const CallExpr *CE = 0;
  if (isa<StmtPoint>(N->getLocation()))
    CE = dyn_cast<CallExpr>(cast<StmtPoint>(N->getLocation()).getStmt());
  if (!CE)
    return 0;
  const FunctionDecl *funDecl = CE->getDirectCallee();
  if (!funDecl)
    return 0;
  StringRef funName = funDecl->getName();

  // Find out if this is an interesting point and what is the kind.
  const char *Msg = 0;
  if (isAllocated(RS, RSPrev))
    Msg = "Memory is allocated here";
  else if (isReleased(RS, RSPrev))
    Msg = "Memory is released here";
  if (!Msg)
    return 0;

  // Generate the extra diagnostic.
  PathDiagnosticLocation Pos(CE, BRC.getSourceManager(),
                             N->getLocationContext());
  return new PathDiagnosticEventPiece(Pos, Msg);
}


#define REGISTER_CHECKER(name) \
void ento::register##name(CheckerManager &mgr) {\
  mgr.registerChecker<MallocChecker>()->Filter.C##name = true;\
}

REGISTER_CHECKER(MallocPessimistic)
REGISTER_CHECKER(MallocOptimistic)
