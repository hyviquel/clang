//===-- clang/Analysis/DefUse.cpp - DefUse analysis -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This file contains the implementation of DefUse analysis
//
//===---------------------------------------------------------------------===//

#include "clang/Analysis/DefUse.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Analysis/CFG.h"
#include <llvm/Support/raw_ostream.h>

// todo: Use DenseMap and llvm sets
#include <set>
#include <map>

using namespace clang;
using namespace defuse;

// utility functions
static unsigned Line(SourceLocation const& l, clang::SourceManager const& sm) {
  PresumedLoc pl = sm.getPresumedLoc(l);
  return pl.getLine();
}

static unsigned Column(SourceLocation const& l, SourceManager const& sm) {
  PresumedLoc pl = sm.getPresumedLoc(l);
  return pl.getColumn();
}

static std::string PrintClangStmt(Stmt const* stmt, ASTContext const& ctx) {
  std::string str;
  llvm::raw_string_ostream pp(str);
  stmt->printPretty(pp, 0, PrintingPolicy(ctx.getLangOpts()), 0);
  return pp.str();
}

//===------------------------- DefUseNode -------------------------===//
inline bool defuse::DefUseNode::operator==(DefUseNode const& n) const {
  if (n.usage != usage || n.kind != kind) return false;
  return kind == VarDecl ? var_decl == n.var_decl : var_ref == n.var_ref;
}

inline clang::VarDecl const* DefUseNode::getDecl() const {
  return kind == VarRef ? dyn_cast<clang::VarDecl> (var_ref->getDecl()) :
    dyn_cast<clang::VarDecl> (var_decl);
}

typedef std::set<DefUseNode const*> VarRefsSet;
typedef std::set<VarDecl const*> VarDeclsSet;
typedef std::map<VarDecl const*, DefUseVect> DefUseChain;

//===------------------------- DefUseBlock -------------------------===//

class defuse::DefUseBlock : public DefUseChain {
public:
  VarDeclsSet uses;
  VarRefsVect defsout;
  VarDeclsSet killed;
  VarRefsVect reaches;

  class not_killed_set_iterator : public std::iterator<std::input_iterator_tag,
      DefUseNode, std::ptrdiff_t, DefUseNode const*> {
    DefUseBlock& block;
    VarRefsVect::const_iterator ptr_it;

    not_killed_set_iterator& inc(bool first) {
      if (ptr_it != block.reaches.end() && !first)
        ++ptr_it;
      while (ptr_it != block.reaches.end() &&
          (block.killed.find((*ptr_it)->getDecl()) != block.killed.end() &&
          std::find(block.defsout.begin(), block.defsout.end(), *ptr_it)
          == block.defsout.end()))
        ++ptr_it;
      return *this;
    }
  public:
    not_killed_set_iterator(DefUseBlock& block_,
        VarRefsVect::const_iterator ptr_it_) :
      block(block_), ptr_it(ptr_it_) { inc(true); }
    not_killed_set_iterator& operator++() { return inc(false); }
    bool operator!=(not_killed_set_iterator const& iter) const {
      return !((&iter.block) == &(block) && iter.ptr_it == ptr_it);
    }
    const DefUseNode* const & operator*() const { return *ptr_it; }
  };

  not_killed_set_iterator begin_not_killed_set() {
    return DefUseBlock::not_killed_set_iterator(*this, reaches.begin());
  }
  not_killed_set_iterator end_not_killed_set() {
    return DefUseBlock::not_killed_set_iterator(*this, reaches.end());
  }
};

//===------------------------- VarDeclMap -------------------------===//

class defuse::VarDeclMap : public StmtVisitor<VarDeclMap> ,
    public llvm::DenseMap<VarDecl const*, DeclStmt const*> {
public:
  VarDeclMap(Stmt const* body) { VisitStmt(const_cast<Stmt*> (body)); }

  void VisitDeclStmt(DeclStmt *DS) {
    for (DeclStmt::decl_iterator I = DS->decl_begin(), E = DS->decl_end();
        I != E; ++I)
    {
      if (VarDecl *VD = dyn_cast<VarDecl>(*I))
        insert(std::make_pair(VD, DS));
    }
  }
  void VisitStmt(Stmt* S) {
    for (Stmt::child_iterator I = S->child_begin(), E = S->child_end();
        I != E; ++I)
      if (*I) Visit(*I);
  }
};

//===------------------------- DefUseHelper -------------------------===//
struct defuse::DefUseHelper::DefUseHelperImpl {
  DefUseData* data;
  VarRefBlockMap* bm;
  VarDeclMap* dm;
  unsigned blockID;

  DefUseHelperImpl() :
    data(NULL), bm(NULL), dm(NULL) { }
};

defuse::DefUseHelper::DefUseHelper() :
  pimpl(new DefUseHelperImpl), current_use(DefUseNode::Use) { }

defuse::DefUseHelper::~DefUseHelper() { delete pimpl; }

void defuse::DefUseHelper::InitializeValues(DefUseData* data,
    VarRefBlockMap* bm, VarDeclMap* dm) {
  pimpl->data = data;
  pimpl->bm = bm;
  pimpl->dm = dm;
}

void defuse::DefUseHelper::HandleDeclRefExpr(const DeclRefExpr *DR) {
  VarRefBlockMap& bm = *pimpl->bm;
  unsigned blockID = pimpl->blockID;
  DefUseBlock& block_data = (*pimpl->data)[blockID];
  if (const VarDecl *VD = dyn_cast<VarDecl>(DR->getDecl())) {
    // map this variable to the current block
    bm[DR] = blockID;
    if (block_data.find(VD) == block_data.end()) {
      block_data.insert(std::make_pair(VD, DefUseVect()));
      block_data.uses.insert(VD); // this variable is used in the current
      //  block and it has no prior definition within the block
    }
    if (current_use == DefUseNode::UseDef) { // in case of compound operators
      DefUseVect &B = block_data[VD];
      B.push_back(DefUseNode(DR, DefUseNode::Use));
      B.push_back(DefUseNode(DR, DefUseNode::Def));
    } else block_data[VD].push_back(DefUseNode(DR, current_use));
  }
}

void defuse::DefUseHelper::HandleDeclStmt(const DeclStmt *DS) {
  for (DeclStmt::const_decl_iterator I = DS->decl_begin(), E = DS->decl_end();
      I != E; I++)
  {
    if (VarDecl* VD = dyn_cast<VarDecl> (*I)) {
      VarRefBlockMap& bm = *pimpl->bm;
      VarDeclMap& dm = *pimpl->dm;
      unsigned blockID = pimpl->blockID;
      DefUseBlock& block_data = (*pimpl->data)[blockID];
      assert((block_data.find(VD) == block_data.end()) &&
          "Variable redefined in the same block");
      block_data[VD].push_back(DefUseNode(VD));
      bm[dm[VD]] = blockID;
    }
  }
}

void defuse::DefUseHelper::HandleBinaryOperator(const BinaryOperator* B) {
  DefUseNode::UseKind backup = current_use; // backup the usage
  if (B->isAssignmentOp()) {
    current_use = DefUseNode::Use;
    Visit(B->getRHS());
    current_use = DefUseNode::Def;
    if (B->isCompoundAssignmentOp())
      current_use = DefUseNode::UseDef;
    Visit(B->getLHS());
  } else {
    Visit(B->getLHS());
    current_use = DefUseNode::Use;
    Visit(B->getRHS());
  }
  current_use = backup; // write back the usage to the current usage
}

void defuse::DefUseHelper::HandleConditionalOperator(const ConditionalOperator*) {}
void defuse::DefUseHelper::HandleOMPTargetDataDirective(const OMPTargetDataDirective*) {}

void defuse::DefUseHelper::HandleCallExpr(const CallExpr* C) {
  DefUseNode::UseKind backup = current_use; // backup the usage
  for (CallExpr::const_arg_iterator I = C->arg_begin(), E = C->arg_end();
      I != E; ++I)
  {
    if ((*I)->getType()->isPointerType() || (*I)->getType()->isReferenceType())
      current_use = DefUseNode::Use; // Should be DefUse, need interprocedural
    else
      current_use = DefUseNode::Use;
    Visit(*I);
    current_use = backup;
  }
}

void defuse::DefUseHelper::HandleUnaryOperator(const UnaryOperator* U) {
  DefUseNode::UseKind backup = current_use; // backup the usage
  switch (U->getOpcode()) {
  case UO_PostInc:
  case UO_PostDec:
  case UO_PreInc:
  case UO_PreDec:
    current_use = DefUseNode::UseDef;
    break;
  case UO_Plus:
  case UO_Minus:
  case UO_Not:
  case UO_LNot:
    current_use = DefUseNode::Use;
    break;
  case UO_AddrOf:
  case UO_Deref:
    // use the current_use
    break;
  default:
    // DEBUG("Operator " << UnaryOperator::getOpcodeStr(U->getOpcode()) <<
    // " not supported in def-use analysis");
    break;
  }
  Visit(U->getSubExpr());
  current_use = backup; // write back the usage to the current usage
}

void defuse::DefUseHelper::HandleArraySubscriptExpr(const ArraySubscriptExpr* AS) {
  DefUseNode::UseKind backup = current_use; // backup the usage
  Visit(AS->getBase());
  current_use = DefUseNode::Use;
  Visit(AS->getIdx());
  current_use = backup; // write back the usage to the current usage
}

void defuse::DefUseHelper::VisitCFGBlock(const CFGBlock & blk,
    const CFGBlock & entry) {
  pimpl->blockID = blk.getBlockID();
  for (CFGBlock::const_iterator I = blk.begin(), E = blk.end(); I != E; ++I)
    if (Optional<CFGStmt> S = I->getAs<CFGStmt>())
      Visit(S->getStmt());

  unsigned blockID = pimpl->blockID;
  DefUseBlock& block_data = (*pimpl->data)[blockID];

  // Post processing
  // build up the uses(b), defsout(b) and killed(b)

  // for each variable
  for (DefUseBlock::iterator I = block_data.begin(), E = block_data.end(); I
      != E; ++I) {
    // in case of references to ParmVar what we do is insert the variable
    // definition in the first block of the CFG
    DefUseBlock& root_data = (*pimpl->data)[entry.getBlockID()];
    if (isa<ParmVarDecl>(I->first) &&
        std::find(root_data[I->first].begin(), root_data[I->first].end(),
        DefUseNode(I->first)) == root_data[I->first].end())
    {
      root_data[I->first].push_back(DefUseNode(I->first));
      root_data.defsout.push_back(&root_data[I->first].back());
    }
    // if this is a local variable, should't be in the defsout,killed sets
    // defsout is calculated by iterating backwards to find the last use of
    // the variable
    for (DefUseVect::const_reverse_iterator UI = I->second.rbegin(), UE =
        I->second.rend(); UI != UE; ++UI)
      if (UI->getUse() == DefUseNode::Def && (block_data.defsout.empty()
          || block_data.defsout.back()->getDecl() != I->first)) {
        block_data.defsout.push_back(&(*UI));
        block_data.killed.insert(UI->getDecl());
        break;
      }
  }
}

void defuse::DefUseHelper::VisitStmt(const Stmt* S) {
  for (Stmt::const_child_iterator I = S->child_begin(), E = S->child_end();
      I != E; ++I)
    if (*I) Visit(*I);
}

bool ContainsStmt(Stmt const* block, Stmt const* stmt) {
  if (block == stmt) return true;
  for (Stmt::const_child_iterator I = block->child_begin(), E =
      block->child_end(); I != E; ++I)
    if (*I && ContainsStmt(*I, stmt))
      return true;
  return false;
}

Stmt const* GetEnclosingBlock(ParentMap const& pm, Stmt const* stmt) {
  Stmt const* E = stmt;
  while (E) {
    Stmt const* cond = NULL;
    switch (E->getStmtClass()) {
    case Stmt::IfStmtClass:
      cond = dyn_cast<IfStmt> (E)->getCond();
      break;
    case Stmt::WhileStmtClass:
      cond = dyn_cast<WhileStmt> (E)->getCond();
      break;
    case Stmt::DoStmtClass:
      cond = dyn_cast<DoStmt> (E)->getCond();
      break;
    case Stmt::ForStmtClass:
      cond = dyn_cast<ForStmt> (E)->getCond();
      break;
    case Stmt::SwitchStmtClass:
      cond = dyn_cast<SwitchStmt> (E)->getCond();
      break;
    case Stmt::CompoundStmtClass:
      return E;
    default:
      break;
    }
    if (cond && !ContainsStmt(cond, stmt)) return E;
    E = pm.getParent(E);
  }
  return NULL;
}

void RemoveLocalDefs(CFG const& cfg, ParentMap const& pm,
    VarDeclMap& decl_vars, DefUseData& data) {
  for (CFG::const_reverse_iterator it = cfg.rbegin(), E = cfg.rend(); it
      != E; ++it) {
    // every time we find a node with more than 1 predecessor it means
    // we are at a junction point and the local variable defined in the
    // previous blocks must be removed from the defsout set
    if ((*it)->pred_size() <= 1) continue;

    if ((*it)->front().getKind() != CFGElement::Statement)
        continue;
    // for each predecessor
    for (CFGBlock::const_pred_iterator pit = (*it)->pred_begin(),
           end = (*it)->pred_end(); pit != end && (*pit)->size(); ++pit) {
      if ((*pit)->front().getKind() != CFGElement::Statement)
        continue;
      CFGStmt FirstBlock = (*pit)->front().castAs<CFGStmt>();
      DefUseBlock& block_data = data[(*pit)->getBlockID()];
      for (VarRefsVect::iterator dit = block_data.defsout.begin();
          dit != block_data.defsout.end(); ++dit) {
        // we have to be sure the element we are using to see if the Decl
        // is in the same block of block statements is also present in the AST,
        // so if the first statement of the CFG block is a VarDecl, we use
        // the VarDeclMap to retrieve the corresponding AST node.
        const Stmt * testStmt = FirstBlock.getStmt();
        if (DeclStmt const* DS = dyn_cast<DeclStmt> (testStmt))
          testStmt = decl_vars[dyn_cast<VarDecl> (DS->getSingleDecl())];
        // if the variable is local to one of the prec blocks and we are at a
        // junction point it means the variable must be removed from the
        // defsout set
        if (GetEnclosingBlock(pm, decl_vars[(*dit)->getDecl()])
            == GetEnclosingBlock(pm, testStmt)) {
          // it can be the case the following block is in the same scope of
          // the previous block (for example if stmt without else)
          FirstBlock = (*it)->front().castAs<CFGStmt>();
          Stmt const* testStmt = FirstBlock.getStmt();
          if (DeclStmt const* DS = dyn_cast<DeclStmt> (testStmt))
            testStmt = decl_vars[dyn_cast<VarDecl> (DS->getSingleDecl())];
          if (GetEnclosingBlock(pm, decl_vars[(*dit)->getDecl()])
              != GetEnclosingBlock(pm, testStmt))
            block_data.defsout.erase(dit--);
        }
      }
    }
  }
}

// implements reaching definitions according to Kennedy's book iterative algorithm
void ComputeReaches(CFG const& cfg, DefUseData& data) {
  bool changed = true;
  while (changed) {
    changed = false;
    VarRefsSet newreaches;
    for (CFG::const_reverse_iterator I = cfg.rbegin(), E = cfg.rend();
        I != E; ++I) {
      DefUseBlock& BlockData = data[(*I)->getBlockID()];
      newreaches = VarRefsSet();
      for (CFGBlock::const_pred_iterator pit = (*I)->pred_begin(),
             end = (*I)->pred_end(); pit != end; ++pit) {
        unsigned p = (*pit)->getBlockID();
        // s(p) = reaches(p) - killed(p)
        VarRefsSet s, temp_set, temp_set_1;
        copy(data[p].begin_not_killed_set(), data[p].end_not_killed_set(),
            inserter(s, s.begin()));
        // tmp_set(p) = defsout(p) U s(p)
        set_union(s.begin(), s.end(), data[p].defsout.begin(),
            data[p].defsout.end(), inserter(temp_set, temp_set.begin()));
        // tmp_set_1 = newreaches U tmp_set(p)
        set_union(newreaches.begin(), newreaches.end(), temp_set.begin(),
            temp_set.end(), inserter(temp_set_1, temp_set_1.begin()));
        newreaches.swap(temp_set_1);
      }
      if (BlockData.reaches.size() != newreaches.size()) {
        BlockData.reaches = VarRefsVect(newreaches.begin(), newreaches.end());
        changed = true;
      }
    }
  }
}

//===------------------------- Printing utilities -------------------------===//

struct printer : public std::iterator<std::input_iterator_tag, printer,
    std::ptrdiff_t, printer> {
  printer(llvm::raw_ostream& x, ASTContext const& ctx_, const char* s = "") :
    o(x), ctx(ctx_), delim(s) {}
  template<typename T>
  printer& operator=(const T& x);
  printer& operator*() { return *this; }
  printer& operator++() { return *this; }
  printer& operator++(int) { return *this; }

  llvm::raw_ostream& o;
  ASTContext const& ctx;
  const char* delim;
};

template<>
printer& printer::operator=<VarDecl const*>(const VarDecl* const & x) {
  o << x->getNameAsString() << delim;
  return *this;
}

template<>
printer& printer::operator=<DefUseNode const*>(const DefUseNode* const & x) {
  SourceLocation loc;
  if (x->getKind() == DefUseNode::VarRef)
    loc = x->getVarRef()->getLocStart();
  else
    loc = x->getVarDecl()->getTypeSpecStartLoc();
  o << x->getDecl()->getNameAsString() << " " << "(" << Line(loc,
      ctx.getSourceManager()) << "," << Column(loc, ctx.getSourceManager())
      << " " << ((x->getUse() == DefUseNode::Use) ? "USE" : "DEF") << ")"
      << delim;
  return *this;
}

template<>
printer& printer::operator=<DefUseNode>(DefUseNode const& x) {
  return this->operator=(&x);
}

void printDefUseData(llvm::raw_ostream& out, ASTContext const& ctx,
    DefUseData const& data) {
  out << "--------------- PRINTING ANALYSIS DATA ----------------------\n";
  unsigned blockID = data.size() - 1;
  for (DefUseData::const_reverse_iterator it = data.rbegin(),
      end = data.rend(); it != end; ++it, --blockID) {
    out << "* BlockID '" << blockID << "' uses vars:\n";
    for (DefUseBlock::const_iterator pit = it->begin(), end = it->end();
        pit != end; ++pit)
    {
      out << "'" << pit->first->getNameAsString() << "':\n\t";
      std::copy(pit->second.begin(), pit->second.end(), printer(out, ctx, ", "));
      out << "\n";
    }
  }

  blockID = data.size() - 1;
  for (DefUseData::const_reverse_iterator it = data.rbegin(), end = data.rend();
      it != end; ++it, --blockID) {
    DefUseBlock const& BlockData = *it;
    out << "---------------- Block: " << blockID << " ----------------\n";
    out << "# uses(" << blockID << "): {";
    copy(BlockData.uses.begin(), BlockData.uses.end(), printer(out, ctx, ", "));
    out << "}\n";
    out << "# defsout(" << blockID << "): {";
    copy(BlockData.defsout.begin(), BlockData.defsout.end(), printer(out, ctx,
        ", "));
    out << "}\n";
    out << "# killed(" << blockID << "): {";
    copy(BlockData.killed.begin(), BlockData.killed.end(), printer(out, ctx,
        ", "));
    out << "}\n";
    out << "# reaches(" << blockID << "): {";
    copy(BlockData.reaches.begin(), BlockData.reaches.end(), printer(out, ctx,
        ", "));
    out << "}\n";
  }
}

//===------------------------- DefUse -------------------------===//

DefUse::~DefUse() {
  delete analysis_data;
  delete decl_map;
  delete block_map;
}

bool isDef(ASTContext const& ctx, DefUseNode const& n, int* blockID,
    DefUseVect::const_iterator* iter, DefUseData const* analysis_data,
    VarDeclMap const* decl_map, VarRefBlockMap const* block_map,
    Stmt const** stmt = NULL)
{
  // retrieve the blockID of the CFG where the statement N (which can be
  // wither a VarDecl or a DeclRefExpr) is placed
  Stmt const* _stmt_;
  if (n.getKind() == DefUseNode::VarDecl) {
    VarDecl const* vd = n.getVarDecl();
    VarDeclMap::const_iterator it = decl_map->find(vd);
    // we use the decl_map to translate the VarDecl to the corresponding
    // DeclStmt this is useful when DeclStmts from the CFG (which usually
    // are different from the original ones) are used
    assert(it != decl_map->end() &&
        "Variable declaration for not mapped to any statement in the syntax tree");
    _stmt_ = it->second;
  } else
    _stmt_ = n.getVarRef();
  assert(_stmt_ != NULL);
  VarRefBlockMap::const_iterator it = block_map->find(_stmt_);
  assert(it != block_map->end() && "Statement not mapped to any block of the CFG");

  // We have to find the definition inside the block
  DefUseBlock::const_iterator data_it =
      (*analysis_data)[it->second].find(n.getDecl());
  // if we don't find the variable among the variable used or defined in the
  // block it means something went wrong
  assert(data_it != (*analysis_data)[it->second].end() &&
      "Block doesn't contains usage or definitions for variable");

  // we find a Variable definition, now we have to iterate through the
  // DefUseNodes in order to find thethe node corresponding to the n variable
  // passed by the user
  DefUseVect::const_iterator _iter_ =
      std::find(data_it->second.begin(), data_it->second.end(), n);
  if (blockID) *blockID = it->second;
  if (iter)    *iter = _iter_;
  if (stmt)    *stmt = _stmt_;
  return _iter_ != data_it->second.end();
}

bool isUse(ASTContext const& ctx, clang::DeclRefExpr const* var, int* blockID,
    DefUseVect::const_iterator* iter, DefUseData const* analysis_data,
    VarRefBlockMap const* block_map)
{
  Stmt const* stmt = var;
  VarRefBlockMap::const_iterator it = block_map->find(stmt);
  assert(it != block_map->end() &&
      "Statement not mapped to any block of the CFG!");

  DefUseBlock::const_iterator data_it =
      (*analysis_data)[it->second].find(dyn_cast<VarDecl> (var->getDecl()));
  assert(data_it != (*analysis_data)[it->second].end() &&
      "Block doesn't contains usage or definitions for variable");

  DefUseVect::const_iterator _iter_ =
      std::find(data_it->second.begin(), data_it->second.end(),
          DefUseNode(var, DefUseNode::Use));
  if (blockID) *blockID = it->second;
  if (iter) *iter = _iter_;
  return _iter_ != data_it->second.end();
}

//===------------------------- uses_iterator -------------------------===//

DefUse::uses_iterator::uses_iterator(DefUse const* du, DefUseNode const& n) :
  DefUse::iterator_impl(du), iter_ptr(-1), inDefBlock(true) {
  Stmt const* stmt = NULL;
  if (!::isDef(du->ctx, n, &iter_ptr.block_id, &iter_ptr.block_it,
      du->analysis_data, du->decl_map, du->block_map, &stmt))
    assert(true && "Not a definition");
  // there is no definition for the DeclRefExpr passed by the user
  // this means he trying to list the uses of a declrefexpr which is a use!
  // an excpetion is thrown.
  node = &(*(iter_ptr.block_it));
  inc(true);
}

bool DefUse::uses_iterator::operator!=(uses_iterator const& iter) {
  return !(iter_ptr.block_id == iter.iter_ptr.block_id
      && iter.iter_ptr.block_it == iter.iter_ptr.block_it);
}

DeclRefExpr const* DefUse::uses_iterator::operator*() {
  assert(iter_ptr.block_it->getKind() == DefUseNode::VarRef);
  return iter_ptr.block_it->getVarRef();
}

DefUse::uses_iterator& DefUse::uses_iterator::inc(bool first) {
  if (iter_ptr.block_id == -1) return *this;
  DefUseBlock const& m = (*du->analysis_data)[iter_ptr.block_id];
  DefUseBlock::const_iterator it = m.find(node->getDecl());
  assert(it != m.end());
  if (iter_ptr.block_it != it->second.end()) iter_ptr.block_it++;

  if (iter_ptr.block_it != it->second.end() &&
      iter_ptr.block_it->getDecl() == node->getDecl()) {
    if (iter_ptr.block_it->getUse() == DefUseNode::Use)
      return *this; // this is a use of the variable
    else if (inDefBlock) {
      // found a new definition, stop iterating
      iter_ptr.block_it = DefUseVect::const_iterator();
      iter_ptr.block_id = -1;
      return *this;
    }
  }
  // if variable in defsout we have to check successor blocks
  if (inDefBlock && std::find(m.defsout.begin(), m.defsout.end(), node)
      == m.defsout.end()) {
    iter_ptr.block_it = DefUseVect::const_iterator();
    iter_ptr.block_id = -1; // it means there are no other uses of this
                            // variable
    return *this;
  }
  // We have to change block
  // the following code has the precondition the blocks are numbered in a
  // decrescent order we lock into the block_id and see if the decl is in
  // the reaches() set or not
  int new_block = iter_ptr.block_id;
  while (--new_block >= 0) {
    DefUseBlock const& m = (*du->analysis_data)[new_block];
    if (std::find(m.reaches.begin(), m.reaches.end(), node) !=
        m.reaches.end()) {
      inDefBlock = false;
      DefUseBlock::const_iterator data_it =
          (*du->analysis_data)[new_block].find(node->getDecl());
      if (data_it != (*du->analysis_data)[new_block].end() &&
          data_it->second.begin()->getUse() == DefUseNode::Use) {
        iter_ptr.block_id = new_block;
        iter_ptr.block_it = data_it->second.begin();
        return *this;
      }
    }
  }
  iter_ptr.block_it = DefUseVect::const_iterator();
  iter_ptr.block_id = -1;
  return *this;
}

//===------------------------- defs_iterator -------------------------===//

// pin the vtable here.
DefUse::iterator_impl::~iterator_impl() {}

DefUse::defs_iterator::defs_iterator(DefUse const* du, DeclRefExpr const& n) :
  iterator_impl(du), iter_ptr(-1), blockDef(false) {
  if (!::isUse(du->ctx, &n, &iter_ptr.block_id, &iter_ptr.block_it,
      du->analysis_data, du->block_map))
    assert(true && "Not an use");
  // the DeclRefExpr passed by the user is never used in this block but only
  // defined, the user is trying to iterate through the definitions of a
  // definition... which doesn't make sense an exception must be thrown
  iter_ptr.reaches_it =
      (*du->analysis_data)[iter_ptr.block_id].reaches.begin();
  node = &(*(iter_ptr.block_it));
  inc(true);
}
bool DefUse::defs_iterator::operator!=(defs_iterator const& iter) {
  return !(iter_ptr.block_id == iter.iter_ptr.block_id
      && iter.iter_ptr.block_it == iter.iter_ptr.block_it
      && iter.iter_ptr.reaches_it == iter_ptr.reaches_it);
}

DefUseNode const* DefUse::defs_iterator::operator*() {
  DefUseBlock::const_iterator it =
      (*du->analysis_data)[iter_ptr.block_id].find(node->getDecl());
  assert(it != (*du->analysis_data)[iter_ptr.block_id].end());
  if (blockDef)
    return &(*iter_ptr.block_it);
  return *iter_ptr.reaches_it;
}

DefUse::defs_iterator& DefUse::defs_iterator::inc(bool first) {
  if (iter_ptr.block_id == -1) return *this;
  // look inside reaches vector
  DefUseBlock::const_iterator it =
      (*du->analysis_data)[iter_ptr.block_id].find(node->getDecl());
  assert(it != (*du->analysis_data)[iter_ptr.block_id].end());
  if (first) {
    // we have to find a def in the block, if exist
    for (; iter_ptr.block_it >= it->second.begin(); iter_ptr.block_it--)
      if (iter_ptr.block_it->getUse() == DefUseNode::Def) {
        blockDef = true;
        break;
      }
    // the def is in the block, so it means we don't have to look any further
    // next time the iterator is incremented we return an empty one
    if (blockDef)
      return *this;

  } else if (blockDef) {
    iter_ptr.block_it = DefUseVect::const_iterator();
    iter_ptr.reaches_it = VarRefsVect::const_iterator();
    iter_ptr.block_id = -1;
    return *this;
  }
  // we didn't find a def in the block, so we have to look at the reaches set
  VarRefsVect const& reaches_v =
      (*du->analysis_data)[iter_ptr.block_id].reaches;
  if (!first && iter_ptr.reaches_it != reaches_v.end())
    iter_ptr.reaches_it++;
  for (; iter_ptr.reaches_it != reaches_v.end(); iter_ptr.reaches_it++)
    if ((*iter_ptr.reaches_it)->getDecl() == node->getDecl())
      return *this;

  iter_ptr.block_it = DefUseVect::const_iterator();
  iter_ptr.reaches_it = VarRefsVect::const_iterator();
  iter_ptr.block_id = -1;
  return *this;
}

DefUse::uses_iterator
DefUse::uses_begin(clang::DeclRefExpr const* var) const {
  return DefUse::uses_iterator(this, DefUseNode(var, DefUseNode::Def));
}
DefUse::uses_iterator
DefUse::uses_begin(clang::VarDecl const* var) const {
  return DefUse::uses_iterator(this, DefUseNode(var));
}
DefUse::uses_iterator DefUse::uses_end() const {
  return DefUse::uses_iterator();
}

DefUse::defs_iterator
DefUse::defs_begin(clang::DeclRefExpr const* var) const {
  return DefUse::defs_iterator(this, *var);
}
DefUse::defs_iterator DefUse::defs_end() const {
  return DefUse::defs_iterator();
}

bool DefUse::isUse(clang::DeclRefExpr const* var) const {
  return ::isUse(ctx, var, NULL, NULL, analysis_data, block_map);
}

bool DefUse::isDef(DefUseNode const& n) const {
  return ::isDef(ctx, n, NULL, NULL, analysis_data, decl_map, block_map);
}

//===------------------------- BuildDefUseChains -------------------------===//

DefUse* DefUse::BuildDefUseChains(Stmt* body, ASTContext *ctx,
    CFG* cfg, ParentMap* pm, DefUseHelper* helper,bool verbose,
    llvm::raw_ostream& out)
{
  std::unique_ptr<DefUseHelper> helperReclaim;
  if (!helper) {
    helper = new DefUseHelper();
    helperReclaim.reset(helper);
  }

  DefUseData* data = new DefUseData(cfg->getNumBlockIDs());

  VarDeclMap* dm = new VarDeclMap(body); // map VarDecl to AST nodes -
  // this is needed as when the CFG is created by DeclStmt
  // with several declarations are rewritten into separate DeclStmts

  VarRefBlockMap* bm = new VarRefBlockMap; // map each VarRef to
                                           // the CFG block where it
                                           // has been used

  if (verbose) cfg->dump(ctx->getLangOpts(), /*ShowColors*/true);
  helper->InitializeValues(data, bm, dm);

  for (CFG::const_iterator I = cfg->begin(), E = cfg->end(); I != E; ++I)
    helper->VisitCFGBlock(**I, cfg->getEntry());

  // remove from defsout local variables
  RemoveLocalDefs(*cfg, *pm, *dm, *data);
  // Calculates the reaches set for each block of the CFG
  ComputeReaches(*cfg, *data);

  if (verbose) printDefUseData(out, *ctx, *data);

  return new DefUse(*ctx, data, dm, bm, cfg->getNumBlockIDs());
}

//===------------------------- DefUseChainTest -------------------------===//

class DefUseChainTest : public StmtVisitor<DefUseChainTest> ,
    public ASTConsumer {
  llvm::raw_ostream& out;
  ASTContext* ctx;
  DefUse const* du;

public:
  DefUseChainTest(llvm::raw_ostream& out_) :
    out(out_), ctx(NULL), du(NULL) { }

  void Initialize(ASTContext& context) { ctx = &context; }

  bool HandleTopLevelDecl(DeclGroupRef declGroupRef);
  // void HandleTranslationUnit(clang::ASTContext& context);

  void VisitDeclRefExpr(DeclRefExpr *DR);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitStmt(Stmt* S);
};

bool DefUseChainTest::HandleTopLevelDecl(DeclGroupRef declRef) {
  for (DeclGroupRef::iterator I = declRef.begin(), E = declRef.end();
      I != E; ++I) {
    if (!isa<FunctionDecl>(*I))
      continue;
    // Top-level Function declaration
    FunctionDecl* func_decl = dyn_cast<FunctionDecl> (*I);
    Stmt* func_body = func_decl->getBody();
    if (!func_body)
      continue;

    out << "--- Building Def-Use Chains for function '"
        << func_decl->getNameAsString() << ":" << func_decl->getNumParams()
        << "' ---\n";
    std::unique_ptr<CFG> cfg
      = CFG::buildCFG(func_decl, func_body, ctx, CFG::BuildOptions());
    ParentMap pm(func_body);

    du = DefUse::BuildDefUseChains(func_body, ctx, cfg.get(), &pm, 0, true);
    out << "------ Testing Def-Use iterators ------\n";
    VisitStmt(func_body);
    out << "**************************************************\n";
    delete du;
  }
  return true;
}

void defuse::PrintVarDefs(DefUse const* DU, DeclRefExpr const* DR,
    ASTContext& ctx, llvm::raw_ostream& out) {
  if (!isa<VarDecl>(DR->getDecl()))
    return;

  if(DU->isUse(DR)){
    VarDecl const* VD = dyn_cast<VarDecl> (DR->getDecl());
    SourceLocation loc = DR->getLocStart();
    out << "Variable ref '" << VD->getNameAsString() << "' in LOC: " << "(" <<
        Line(loc, ctx.getSourceManager()) << "," <<
        Column(loc,ctx.getSourceManager()) << ") -> defs_iterator:\n";
    for (DefUse::defs_iterator I = DU->defs_begin(DR),
        E = DU->defs_end(); I != E; ++I) {
      SourceLocation loc;
      if ((*I)->getKind() == DefUseNode::VarRef)
        loc = (*I)->getVarRef()->getLocStart();
      else
        loc = (*I)->getVarDecl()->getTypeSpecStartLoc();
      out << "\t* Found DEF in loc: " << "(" <<
          Line(loc, ctx.getSourceManager()) << "," <<
          Column(loc, ctx.getSourceManager()) << ")\n";
    }
  }
}
void PrintVarUses(DefUse const* DU, DefUseNode const& n, ASTContext& ctx,
    llvm::raw_ostream& out) {

  if((n.getKind() == DefUseNode::VarRef && DU->isDef(n.getVarRef())) ||
      (n.getKind() == DefUseNode::VarDecl)) {
    VarDecl const* VD = n.getDecl();
    SourceLocation loc = VD ->getTypeSpecStartLoc();
    if (n.getKind() == DefUseNode::VarRef)
      loc = n.getVarRef()->getLocStart();
    out << "Variable ref '" << VD->getNameAsString() << "' in LOC: " <<
        "(" << Line(loc, ctx.getSourceManager()) << "," <<
        Column(loc, ctx.getSourceManager()) << ") -> uses_iterator:\n";
    DefUse::uses_iterator it = DU->uses_end();
    if (n.getKind() == DefUseNode::VarRef)
      it = DU->uses_begin(n.getVarRef());
    else
      it = DU->uses_begin(VD);
    for (; it != DU->uses_end(); ++it) {
      SourceLocation loc = (*it)->getLocStart();
      out << "\t* Found USE in loc: " << "(" <<
          Line(loc, ctx.getSourceManager()) << "," <<
          Column(loc, ctx.getSourceManager()) << ")\n";
    }
  }
}

ASTConsumer* defuse::CreateDefUseTestConsumer(llvm::raw_ostream& out) {
  return new DefUseChainTest(out);
}

inline void defuse::PrintVarUses(DefUse const* DU, DeclRefExpr const* DR,
    ASTContext& ctx, llvm::raw_ostream& out) {
  ::PrintVarUses(DU, DefUseNode(DR), ctx, out);
}
inline void defuse::PrintVarUses(DefUse const* DU, VarDecl const* VD,
    ASTContext& ctx, llvm::raw_ostream& out) {
  ::PrintVarUses(DU, DefUseNode(VD), ctx, out);
}

void DefUseChainTest::VisitDeclRefExpr(DeclRefExpr *DR) {
  if (isa<VarDecl>(DR->getDecl())){
    PrintVarUses(du, DR, *ctx, out);
    PrintVarDefs(du, DR, *ctx, out);
  }
}

void DefUseChainTest::VisitDeclStmt(DeclStmt *DS) {
  for (DeclStmt::decl_iterator I = DS->decl_begin(), E = DS->decl_end();
      I != E; ++I) {
    if (isa<VarDecl>(*I)) continue;

    VarDecl* VD = dyn_cast<VarDecl> (*I);
    PrintVarUses(du, VD, *ctx, out);

    if (Stmt *S = VD->getInit()) Visit(S);
  }
}

void DefUseChainTest::VisitStmt(Stmt* S) {
  for (Stmt::child_iterator I = S->child_begin(), E = S->child_end(); I != E; ++I)
    if (*I) Visit(*I);
}
