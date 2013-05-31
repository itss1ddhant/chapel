
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include "astutil.h"
#include "expr.h"
#include "misc.h"
#include "passes.h"
#include "stmt.h"
#include "stringutil.h"
#include "type.h"
#include "codegen.h"

// some prototypes
static GenRet codegenCallExpr(const char* fnName);
static void codegenAssign(GenRet to_ptr, GenRet from);
static GenRet codegenCast(Type* t, GenRet value, bool Cparens = true);
static GenRet codegenCast(const char* typeName, GenRet value, bool Cparens = true);
static GenRet codegenCastToVoidStar(GenRet value);
static GenRet createTempVar(Type* t);
#define createTempRef(t) createTempVar(t)

// These functions operate on wide pointers. There are several different
// kinds of wide pointers:
//  1 wide reference to something
//    (wide.chplType->symbol->hasFlag(FLAG_WIDE))
//  2 wide class pointer
//    (wide.chplType->symbol->hasFlag(FLAG_WIDE_CLASS))
//  3 wide result of codegenFieldPtr or codegenElementPtr etc
//    (wide.isLVPtr == GEN_WIDE_PTR)
// These functions need to handle all of these cases, but they
//  may transform the last case into the 1st. They can't transform
//  case 2 into case 3 because we wouldn't have a Chapel type for
//  the body of a class.
static GenRet codegenRaddr(GenRet wide);
static GenRet codegenRlocale(GenRet wide);
static GenRet codegenRnode(GenRet wide);
static GenRet codegenRsubloc(GenRet wide);

static GenRet codegenAddrOf(GenRet r);

/* Note well the difference between codegenCall and codegenCallExpr.
 * codegenCallExpr always returns the call as an expression in the
 * returned GenRet. But codegenCall instead adds the call to the
 * generated statements. If one uses codegenCallExpr instead of codegenCall,
 * the C backend will never actually emit the call, since it won't
 * be added to the list of statements.
 */
static GenRet codegenCallExpr(GenRet function, std::vector<GenRet> & args, FnSymbol* fSym, bool defaultToValues);
static GenRet codegenCallExpr(const char* fnName, std::vector<GenRet> & args, bool defaultToValues = true);
static GenRet codegenCallExpr(const char* fnName);
static GenRet codegenCallExpr(const char* fnName, GenRet a1);
static GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2);
static GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3);
static GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3, GenRet a4);
static GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3, GenRet a4, GenRet a5);
static void codegenCall(const char* fnName, std::vector<GenRet> & args, bool defaultToValues = true);
static void codegenCall(const char* fnName, GenRet a1);
static void codegenCall(const char* fnName, GenRet a1, GenRet a2);
static void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3);
//static void codegenCallNotValues(const char* fnName, GenRet a1, GenRet a2, GenRet a3);
static void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3, GenRet a4);

static GenRet codegenZero();
static GenRet codegenOne();

static GenRet codegenFieldPtr(GenRet base, const char* field);

static int codegen_tmp = 1;

Expr::Expr(AstTag astTag) :
  BaseAST(astTag),
  prev(NULL),
  next(NULL),
  list(NULL),
  parentExpr(NULL),
  parentSymbol(NULL)
{}


Expr* Expr::getStmtExpr() {
  for (Expr* expr = this; expr; expr = expr->parentExpr) {
    if (IS_STMT(expr) || isBlockStmt(expr->parentExpr))
      return expr;
  }
  return NULL;
}


void Expr::verify() {
  if (prev || next)
    if (!list)
      INT_FATAL(this, "Expr is in list but does not point at it");
  if (!parentSymbol)
    INT_FATAL(this, "Expr::parentSymbol is NULL");
  if (parentExpr && parentExpr->parentSymbol != parentSymbol)
    INT_FATAL(this, "Bad Expr::parentSymbol");
}


bool Expr::inTree(void) {
  if (parentSymbol)
    return parentSymbol->inTree();
  else
    return false;
}


Type* Expr::typeInfo(void) {
  INT_FATAL(this, "Illegal call to Expr::typeInfo()");
  return NULL;
}


static void
callReplaceChild(Expr* expr, Expr* new_ast) {
  if (expr->parentExpr) {
    expr->parentExpr->replaceChild(expr, new_ast);
  } else {
    expr->parentSymbol->replaceChild(expr, new_ast);
  }
}

void Expr::prettyPrint(std::ofstream *o) {
  if (BlockStmt *stmt = toBlockStmt(this))
    printf("blockstmt %s", stmt->userLabel);
  else if (CondStmt *stmt = toCondStmt(this))
    printf("condstmt %s", stmt->condExpr->parentSymbol->name);
  else if (GotoStmt *stmt = toGotoStmt(this))
    printf("gotostmt %s", stmt->label->parentSymbol->name);
  printf("Oh no! This method hasn't been defined for this class!\n");
}

Expr* Expr::remove(void) {
  if (!this)
    return this;
  if (list) {
    if (next)
      next->prev = prev;
    else
      list->tail = prev;
    if (prev)
      prev->next = next;
    else
      list->head = next;
    list->length--;
    next = NULL;
    prev = NULL;
    list = NULL;
  } else {
    callReplaceChild(this, NULL);
  }
  if (parentSymbol)
    remove_help(this, 'r');
  else
    trace_remove(this, 'R');
  return this;
}


void Expr::replace(Expr* new_ast) {
  if (new_ast->parentSymbol)
    INT_FATAL(new_ast, "Argument is already in AST in Expr::replace");
  if (new_ast->list)
    INT_FATAL(new_ast, "Argument is in a list in Expr::replace");
  if (list) {
    new_ast->next = next;
    new_ast->prev = prev;
    new_ast->list = list;
    if (next)
      next->prev = new_ast;
    else
      list->tail = new_ast;
    if (prev)
      prev->next = new_ast;
    else
      list->head = new_ast;
    next = NULL;
    prev = NULL;
    list = NULL;
  } else {
    callReplaceChild(this, new_ast);
  }

  Symbol* myParentSymbol = parentSymbol;
  Expr* myParentExpr = parentExpr;
  remove_help(this, 'p');
  insert_help(new_ast, myParentExpr, myParentSymbol);
}


void Expr::insertBefore(Expr* new_ast) {
  if (new_ast->parentSymbol)
    INT_FATAL(new_ast, "Argument is already in AST in Expr::insertBefore");
  if (!list)
    INT_FATAL(this, "Cannot call insertBefore on Expr not in a list");
  if (new_ast->list)
    INT_FATAL(new_ast, "Argument is in a list in Expr::insertBefore");
  new_ast->prev = prev;
  new_ast->next = this;
  new_ast->list = list;
  if (prev)
    prev->next = new_ast;
  else
    list->head = new_ast;
  prev = new_ast;
  if (parentSymbol)
    sibling_insert_help(this, new_ast);
  list->length++;
}


void Expr::insertAfter(Expr* new_ast) {
  if (new_ast->parentSymbol)
    INT_FATAL(new_ast, "Argument is already in AST in Expr::insertAfter");
  if (!list)
    INT_FATAL(this, "Cannot call insertAfter on Expr not in a list");
  if (new_ast->list)
    INT_FATAL(new_ast, "Argument is in a list in Expr::insertAfter");
  new_ast->prev = this;
  new_ast->next = next;
  new_ast->list = list;
  if (next)
    next->prev = new_ast;
  else
    list->tail = new_ast;
  next = new_ast;
  if (parentSymbol)
    sibling_insert_help(this, new_ast);
  list->length++;
}


SymExpr::SymExpr(Symbol* init_var) :
  Expr(E_SymExpr),
  var(init_var)
{
  if (!init_var)
    INT_FATAL(this, "Bad call to SymExpr");
  gSymExprs.add(this);
}


void 
SymExpr::replaceChild(Expr* old_ast, Expr* new_ast) {
  INT_FATAL(this, "Unexpected case in SymExpr::replaceChild");
}


void
SymExpr::verify() {
  Expr::verify();
  if (astTag != E_SymExpr)
    INT_FATAL(this, "Bad SymExpr::astTag");
  if (!var)
    INT_FATAL(this, "SymExpr::var is NULL");
  if (var && var->defPoint && !var->defPoint->parentSymbol)
    INT_FATAL(this, "SymExpr::var::defPoint is not in AST");
}


SymExpr*
SymExpr::copyInner(SymbolMap* map) {
  return new SymExpr(var);
}


Type* SymExpr::typeInfo(void) {
  return var->type;
}


GenRet SymExpr::codegen() {
  GenInfo* info = gGenInfo;
  FILE* outfile = info->cfile;
  GenRet ret;
  if( outfile ) {
    if (getStmtExpr() && getStmtExpr() == this)
      codegenStmt(this);
    ret = var->codegen();
  } else {
#ifdef HAVE_LLVM
    if(isVarSymbol(var)) {
      ret = toVarSymbol(var)->codegen();
    } else if(isArgSymbol(var)) {
      ret = info->lvt->getValue(var->cname);
    } else if(isTypeSymbol(var)) {
      ret.type = toTypeSymbol(var)->codegen().type;
    } else if(isFnSymbol(var) ){
      ret = toFnSymbol(var)->codegen();
    } else {
      ret = info->lvt->getValue(var->cname);
      if( ! ret.val ) {
        INT_FATAL(this, "!!!!!!! UNHANDLED SYM EXPR !!!!!!!");
      }
    }
#endif
  }
  return ret;
}


void SymExpr::prettyPrint(std::ofstream *o) {
  if (strcmp(var->name, "nil") != 0) {
    if (var->isImmediate()) {
      if (VarSymbol *sym = toVarSymbol(var)) {
        if (sym->immediate->const_kind == CONST_KIND_STRING)
          *o << "\"" << sym->immediate->v_string << "\"";
        else if (sym->immediate->num_index == INT_SIZE_1)
          *o << sym->immediate->v_bool;
        else if (sym->immediate->const_kind == NUM_KIND_INT)
          *o << sym->immediate->int_value();
        else if (sym->immediate->const_kind == NUM_KIND_UINT)
          *o << sym->immediate->uint_value();
      }
    } else {
      *o << var->name;
    }
  }
}


UnresolvedSymExpr::UnresolvedSymExpr(const char* i_unresolved) :
  Expr(E_UnresolvedSymExpr),
  unresolved(astr(i_unresolved))
{
  if (!i_unresolved)
    INT_FATAL(this, "bad call to UnresolvedSymExpr");
  gUnresolvedSymExprs.add(this);
}


void 
UnresolvedSymExpr::replaceChild(Expr* old_ast, Expr* new_ast) {
  INT_FATAL(this, "unexpected case in UnresolvedSymExpr::replaceChild");
}


void
UnresolvedSymExpr::verify() {
  Expr::verify();
  if (astTag != E_UnresolvedSymExpr)
    INT_FATAL(this, "bad UnresolvedSymExpr::astTag");
  if (!unresolved)
    INT_FATAL(this, "UnresolvedSymExpr::unresolved is NULL");
}


UnresolvedSymExpr*
UnresolvedSymExpr::copyInner(SymbolMap* map) {
  return new UnresolvedSymExpr(unresolved);
}


Type* UnresolvedSymExpr::typeInfo(void) {
  return dtUnknown;
}


GenRet UnresolvedSymExpr::codegen() {
  GenInfo* info = gGenInfo;
  FILE* outfile = info->cfile;
  GenRet ret;
  INT_FATAL(this, "UnresolvedSymExpr::codegen called");
  if( outfile ) fprintf(outfile, "%s /* unresolved symbol */", unresolved);
  return ret;
}

void UnresolvedSymExpr::prettyPrint(std::ofstream *o) {
  *o << unresolved;
}


DefExpr::DefExpr(Symbol* initSym, BaseAST* initInit, BaseAST* initExprType) :
  Expr(E_DefExpr),
  sym(initSym),
  init(NULL),
  exprType(NULL)
{
  if (sym)
    sym->defPoint = this;

  if (Expr* a = toExpr(initInit))
    init = a;
  else if (Symbol* a = toSymbol(initInit))
    init = new SymExpr(a);
  else if (initInit)
    INT_FATAL(this, "DefExpr initialized with bad init ast");

  if (Expr* a = toExpr(initExprType))
    exprType = a;
  else if (Symbol* a = toSymbol(initExprType))
    exprType = new SymExpr(a);
  else if (initExprType)
    INT_FATAL(this, "DefExpr initialized with bad exprType ast");

  if (init && init->parentSymbol)
    INT_FATAL(this, "DefExpr initialized with init already in tree");

  if (exprType && exprType->parentSymbol)
    INT_FATAL(this, "DefExpr initialized with exprType already in tree");

  if (isArgSymbol(sym) && (exprType || init))
    INT_FATAL(this, "DefExpr of ArgSymbol cannot have either exprType or init");

  gDefExprs.add(this);
}


void DefExpr::verify() {
  Expr::verify();
  if (astTag != E_DefExpr) {
    INT_FATAL(this, "Bad DefExpr::astTag");
  }
  if (!sym) {
    INT_FATAL(this, "DefExpr has no sym");
  }
  if (toFnSymbol(sym) && (exprType || init))
    INT_FATAL(this, "Bad FnSymbol::defPoint");
  if (toArgSymbol(sym) && (exprType || init))
    INT_FATAL(this, "Bad ArgSymbol::defPoint");
  if (init && init->parentExpr != this)
    INT_FATAL(this, "Bad DefExpr::init::parentExpr");
  if (exprType && exprType->parentExpr != this)
    INT_FATAL(this, "Bad DefExpr::exprType::parentExpr");
}


DefExpr*
DefExpr::copyInner(SymbolMap* map) {
  return new DefExpr(COPY_INT(sym), COPY_INT(init), COPY_INT(exprType));
}


void DefExpr::replaceChild(Expr* old_ast, Expr* new_ast) {
  if (old_ast == init) {
    init = new_ast;
  } else if (old_ast == exprType) {
    exprType = new_ast;
  } else {
    INT_FATAL(this, "Unexpected case in DefExpr::replaceChild");
  }
}


Type* DefExpr::typeInfo(void) {
  INT_FATAL(this, "Illegal call to DefExpr::typeInfo()");
  return NULL;
}


GenRet DefExpr::codegen() {
  GenInfo* info = gGenInfo;
  GenRet ret;
  if( info->cfile ) {
    if (toLabelSymbol(sym)) {
      std::string str = sym->cname;
      str += ":;\n"; // ; avoids 'label at end of compound statement' error
      info->cStatements.push_back(str);
    }
  } else {
#ifdef HAVE_LLVM
    if (toLabelSymbol(sym)) {
      llvm::Function *func = info->builder->GetInsertBlock()->getParent();
      
      llvm::BasicBlock *blockLabel;
      
      if(!(blockLabel = info->lvt->getBlock(sym->cname))) {
        blockLabel = llvm::BasicBlock::Create(
            info->module->getContext(), sym->cname);
        info->lvt->addBlock(sym->cname, blockLabel);
      }
      
      info->builder->CreateBr(blockLabel);

      func->getBasicBlockList().push_back(blockLabel);
      info->builder->SetInsertPoint(blockLabel);
    }
#endif
  }
  return ret;
}


#define WIDE_GEP_LOC 0
#define WIDE_GEP_LOC_NODE 0
#define WIDE_GEP_ADDR 1

// Generates code to load the wide version of an address and returns an
// expression that evaluates to this address.
//
// The wide address is generated by a call to the runtime support
// function chpl_build_wide_ptr_loc.
//
static
GenRet codegenWideAddr(GenRet locale, GenRet raddr, Type* wideType = NULL)
{
  GenRet ret;
  GenInfo* info = gGenInfo;
  Type* wideRefType = NULL;

  if( locale.chplType ) INT_ASSERT(locale.chplType == dtLocaleID->typeInfo());

  // This function does not work on wide strings.
  if( raddr.chplType && raddr.chplType == dtString )
    INT_FATAL("can't build wide address from string");

  if( raddr.chplType && !wideType ) {
    INT_ASSERT(raddr.isLVPtr != GEN_WIDE_PTR);
    Type* refType;
    if( raddr.isLVPtr == GEN_VAL ) {
      // Then we should have a ref or a class.
      INT_ASSERT(raddr.chplType == dtNil ||
                 isClass(raddr.chplType) ||
                 raddr.chplType->symbol->hasFlag(FLAG_REF));
      refType = raddr.chplType;
    } else {
      // GEN_REF
      refType = getOrMakeRefTypeDuringCodegen(raddr.chplType);
    }
    wideRefType = getOrMakeWideTypeDuringCodegen(refType);
    INT_ASSERT(wideRefType);
  } else {
    wideRefType = wideType;
  }

  INT_ASSERT(wideRefType);

  locale = codegenValue(locale);
  if( widePointersStruct ) {
    // Create a stack-local stored wide pointer
    // of the appropriate type.
    ret = createTempVar(wideRefType);
    if( info->cfile ) {
      std::string localeAssign;
      std::string addrAssign;
      ret = codegenValue(ret); // remove the & part.
      localeAssign = ret.c + ".locale = " + locale.c + ";\n";
      info->cStatements.push_back(localeAssign);
      addrAssign = ret.c + ".addr = " + raddr.c + ";\n";
      info->cStatements.push_back(addrAssign);
    } else {
#ifdef HAVE_LLVM
      llvm::Value* adr = info->builder->CreateStructGEP(ret.val, WIDE_GEP_ADDR);
      llvm::Value* loc = info->builder->CreateStructGEP(ret.val, WIDE_GEP_LOC);

      // cast address if needed. This is necessary for building a wide
      // NULL pointer since NULL is actually an i8*.
      llvm::Type* addrType = adr->getType()->getPointerElementType();
      llvm::Value* addrVal = raddr.val;
      if (raddr.val->getType() != addrType){
        addrVal = convertValueToType(addrVal, addrType);
      }
      INT_ASSERT(addrVal);

      info->builder->CreateStore(addrVal, adr);
      info->builder->CreateStore(locale.val, loc);
#endif
    }
    // Load whatever we stored...
    ret = codegenValue(ret);
  } else {
    if( fLLVMWideOpt ) {
#ifdef HAVE_LLVM
      GenRet wideTy = wideRefType; // get the LLVM type for the wide ref.
      llvm::PointerType *addrType = llvm::cast<llvm::PointerType>(wideTy.type);

      // call GLOBAL_FN_GLOBAL_MAKE dummy function 
      llvm::Function* fn = getMakeFn(info->module, &info->globalToWideInfo,
                                     addrType);
      INT_ASSERT(fn);
      llvm::Type* eltType = addrType->getElementType();
      llvm::Type* locAddrType = llvm::PointerType::getUnqual(eltType);
      // Null pointers require us to possibly cast to the pointer type
      // we are supposed to have since null has type void*.
      llvm::Value* locAddr = raddr.val;
      locAddr = info->builder->CreatePointerCast(locAddr, locAddrType);
      ret.val = info->builder->CreateCall2(fn, locale.val, locAddr);
#endif
    } else {
      // Packed wide pointers.
      ret = codegenCallExpr("chpl_return_wide_ptr_loc",
                            locale, codegenCastToVoidStar(raddr));
      ret = codegenCast(wideRefType, ret);
    }
  }

  ret.chplType = wideRefType->getValType();
  ret.isLVPtr = GEN_WIDE_PTR;
  return ret;
}

// Generates a new version of a wide address which has a different
// .addr part, leaving the locale part alone.
static
GenRet codegenWideAddrWithAddr(GenRet base, GenRet newAddr, Type* wideType = NULL)
{
  // NOTE - if computing the entire localeID becomes one day
  // expensive, and it can be inferred from the pointer part,
  // update this to just use the node part.
  return codegenWideAddr(codegenRlocale(base), newAddr);
}

#ifdef HAVE_LLVM

// Set USE_TBAA to 0 to disable the emission of Type Based Alias Analysis
// metadata when generating LLVM loads or stores.
// Set USE_TBAA to emit TBAA metadata with loads and stores.
// It is currently disabled until we gain more confidence in the
// implementation.
#define USE_TBAA 0

// Create an LLVM store instruction possibly adding
// appropriate metadata based upon the Chapel type of val.
//
static
llvm::StoreInst* codegenStoreLLVM(llvm::Value* val,
                                  llvm::Value* ptr,
                                  Type* valType = NULL)
{
  GenInfo *info = gGenInfo;
  llvm::StoreInst* ret = info->builder->CreateStore(val, ptr);
  llvm::MDNode* tbaa = NULL;
  if( USE_TBAA && valType ) tbaa = valType->symbol->llvmTbaaNode;
  if( tbaa ) ret->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
  return ret;
}


static
llvm::StoreInst* codegenStoreLLVM(GenRet val,
                                  GenRet ptr,
                                  Type* valType = NULL)
{
  if( val.chplType && !valType ) valType = val.chplType;
  if( ptr.chplType && !valType ) valType = ptr.chplType->getValType();
  return codegenStoreLLVM(val.val, ptr.val, valType);
}
// Create an LLVM store instruction possibly adding
// appropriate metadata based upon the Chapel type of ptr.
static
llvm::LoadInst* codegenLoadLLVM(llvm::Value* ptr,
                                Type* valType = NULL,
                                bool isConst = false)
{
  GenInfo* info = gGenInfo;
  llvm::LoadInst* ret = info->builder->CreateLoad(ptr);
  llvm::MDNode* tbaa = NULL;
  if( USE_TBAA && valType ) {
    if( isConst ) tbaa = valType->symbol->llvmConstTbaaNode;
    else tbaa = valType->symbol->llvmTbaaNode;
  }
  if( tbaa ) ret->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
  return ret;
}

static
llvm::LoadInst* codegenLoadLLVM(GenRet ptr,
                                Type* valType = NULL,
                                bool isConst = false)
{
  if( ptr.chplType && !valType ) valType = ptr.chplType->getValType();
  return codegenLoadLLVM(ptr.val, valType, isConst);
}

#endif

static
GenRet codegenLocaleForNode(GenRet node)
{
  GenInfo* info = gGenInfo;
  Type* localeType = LOCALE_ID_TYPE;
  GenRet ret;

  ret.chplType = localeType;
  node = codegenValue(node);

  if( info->cfile ) {
    INT_ASSERT(0); // not implemented yet.
  } else {
#ifdef HAVE_LLVM
    GenRet genType = localeType;
    unsigned Idxs[1];
    Idxs[0] = WIDE_GEP_LOC_NODE;
    llvm::Value* v0 = llvm::ConstantAggregateZero::get(genType.type);
    llvm::Value* v1 = info->builder->CreateInsertValue(v0, node.val, Idxs);
    ret.val = v1;
#endif
  }

  return ret;
}


static
GenRet codegenUseGlobal(const char* global)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  if( info->cfile ) {
    ret.c = global;
  } else {
#ifdef HAVE_LLVM
    ret = info->lvt->getValue(global);
    if( ! ret.val )
      INT_FATAL("Could not find global %s, "
                "perhaps it is missing or is complicated macro?", global);
    assert( ret.isLVPtr != GEN_WIDE_PTR );
    if( ret.isLVPtr == GEN_PTR ) {
      ret.val = codegenLoadLLVM(ret);
    }
    INT_ASSERT(ret.val);
#endif
  }
  ret.isLVPtr = GEN_VAL;
  return ret;
}

static
GenRet codegenUseCid(Type* classType)
{
  std::string varname;
  varname = varname + "chpl__cid_" + classType->symbol->cname;
  GenRet ret = codegenUseGlobal(varname.c_str());
  ret.chplType = CLASS_ID_TYPE;
  return ret;
}

// A construct which gives the current node ID (int32_t).
static
GenRet codegenGetNodeID(void)
{
  GenRet ret =  codegenUseGlobal("chpl_nodeID");
  ret.chplType = NODE_ID_TYPE;
  return ret;
}

// A construct which gives the current sublocale ID.
static
GenRet codegenGetSublocID(void)
{
  GenRet ret =  codegenCallExpr("chpl_task_getSubLoc");
  ret.chplType = SUBLOC_ID_TYPE;
  return ret;
}

static
GenRet codegenLocaleID(GenRet node, GenRet subloc)
{
  GenRet ret;
  Type* localeType = LOCALE_ID_TYPE;

  ret = codegenCallExpr("chpl_return_localeID", node, subloc);
  if( ret.val ) {
#ifdef HAVE_LLVM
    GenRet expectType = LOCALE_ID_TYPE;
    ret.val = convertValueToType(ret.val, expectType.type);
#endif
  }
  ret.chplType = localeType;
  return ret;
}

// A construct which gives the current locale ID.
static
GenRet codegenGetLocaleID(void)
{
  return codegenLocaleID(codegenGetNodeID(), codegenGetSublocID());
}

static
GenRet codegenUseGlobal(std::string str)
{
  return codegenUseGlobal(str.c_str());
}

static
GenRet codegenWideHere(GenRet addr, Type* wideType = NULL)
{
  GenRet locale = codegenGetLocaleID();
  GenRet addrVal = codegenValue(addr);
  GenRet ret = codegenWideAddr(locale, addrVal, wideType);
  return ret;
}

static bool isWide(GenRet x)
{
  if( x.isLVPtr == GEN_WIDE_PTR ) return true;
  if( x.chplType && x.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_WIDE_CLASS) ) return true;
  return false;
}

static
Type* wideRefTypeToLocalRefType(GenRet wide, Type** wideRefTypeOut)
{
  Type* ret = NULL;
  Type* wideRefType = NULL;
  if( wide.chplType ) {
    // Set the resulting Chapel type.
    if( wide.isLVPtr == GEN_WIDE_PTR ) {
      // wide lv-pointer, e.g. to int,
      // so we return a reference to int.
      ret = getOrMakeRefTypeDuringCodegen(wide.chplType);
      wideRefType = getOrMakeWideTypeDuringCodegen(ret);
    } else {
      if( wide.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_WIDE_CLASS) ) {
        ret = wide.chplType->getField("addr")->typeInfo();
        wideRefType = wide.chplType;
      } else {
        INT_ASSERT(0);
      }
    }
  }
  if( wideRefTypeOut ) *wideRefTypeOut = wideRefType;
  return ret;
}

// This function casts a wide pointer to a void* wide pointer (ie wide_ptr_t)
// for use with packed wide pointers.
static GenRet codegenCastWideToVoid(GenRet wide) {

  INT_ASSERT(wide.isLVPtr == GEN_WIDE_PTR ||
             (wide.chplType &&
              wide.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_WIDE_CLASS)));

  // If we have a local pointer to a wide reference,
  // codegen value it.
  if( wide.isLVPtr == GEN_PTR ) {
    wide = codegenValue(wide);
  }

  // If we don't already have a wide reference - that is, if
  // wide.isLVPtr == GEN_WIDE_PTR - convert it to a Chapel reference in order
  // to create a temporary variable and have fewer cases below.
  if( wide.isLVPtr == GEN_WIDE_PTR ) {
    wide = codegenAddrOf(wide);
    // The result is wide.isLVPtr == GEN_VAL but wide.chplType is a wide ref
  }

  return codegenCast("wide_ptr_t", wide);
}

// Extract a field of a wide string, returning an lvalue-pointer to the that
// field if we have a pointer to the wide string.  We need this function
// because codegenRaddr and friends now work with void wide pointer data-types
// (and wide strings are not the same as other wide types), and because
// codegenFieldPtr doesn't work to extract wide string fields (since it thinks
// it's supposed to be extracting fields from the class rather than from the
// wide ptr).
static GenRet codegenWideStringField(GenRet ws, const char* field)
{
  GenRet ret;
  GenInfo* info = gGenInfo;

  INT_ASSERT(isWideString(ws.chplType));

  ClassType *classType = toClassType(ws.chplType);
  if( info->cfile ) {
    if (ws.isLVPtr == GEN_PTR) {
      ret.isLVPtr = GEN_PTR;
      ret.c += "&((" + ws.c + ")->" + field + ")";
    } else {
      // we don't have an l-value for this one.
      // Could be wide lv-ptr or GEN_VAL wide ref.
      ret.isLVPtr = GEN_VAL;
      ret.c += "(" + ws.c + ")." + field;
    }
  } else {
#ifdef HAVE_LLVM
    if (ws.val->getType()->isPointerTy()){
      ret.isLVPtr = GEN_PTR;
      ret.val = info->builder->CreateConstInBoundsGEP2_32(ws.val, 0, classType->getMemberGEP(field));
    } else {
      ret.isLVPtr = GEN_VAL;
      ret.val = info->builder->CreateExtractValue(ws.val, classType->getMemberGEP(field));
    }
#endif
  }
  Symbol* fieldSymbol = classType->getField("size", true);
  ret.chplType = fieldSymbol->typeInfo();
  return ret;
}

// Generates code to load the remote address from a wide address.
// Always returns the address portion of the wide pointer as a value.
// The .chplType of the result will be a reference or class type.
GenRet codegenRaddr(GenRet wide)
{
  GenRet ret;
  GenInfo* info = gGenInfo;
  Type* wideRefType = NULL;
  Type* type = NULL;
 
  if( wide.isLVPtr != GEN_WIDE_PTR && isWideString(wide.chplType)) {
   ret = codegenWideStringField(wide,"addr");
   ret.chplType = dtString;
   return ret;
  }

  type = wideRefTypeToLocalRefType(wide, &wideRefType);

  if( widePointersStruct ) {
    if (info->cfile ) {
      if (wide.isLVPtr == GEN_PTR) {
        ret.c += "(" + wide.c + ")->addr";
      } else {
        // could we GEN_WIDE_PTR or a wide reference.
        ret.c += "(" + wide.c + ").addr";
      }
    } else {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) {
        llvm::Value* ptr = info->builder->CreateStructGEP(wide.val,
                                                          WIDE_GEP_ADDR);
        ret.val = info->builder->CreateLoad(ptr);
      } else {
        // if wide is not a pointer...
        ret.val = info->builder->CreateExtractValue(wide.val, WIDE_GEP_ADDR);
      }
#endif
    }
  } else {
    if( fLLVMWideOpt ) {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) wide = codegenValue(wide);
      GenRet wideTy = wideRefType; // get the LLVM type for the wide ref.
      llvm::PointerType *addrType = llvm::cast<llvm::PointerType>(wideTy.type);

      // call GLOBAL_FN_GLOBAL_ADDR dummy function 
      llvm::Function* fn = getAddrFn(info->module, &info->globalToWideInfo,
                                     addrType);
      INT_ASSERT(fn);
      ret.val = info->builder->CreateCall(fn, wide.val);
#endif
    } else {

      // Packed wide pointers
      ret = codegenCallExpr("chpl_wide_ptr_get_address",
                            codegenCastWideToVoid(wide));
    }
    ret = codegenCast(type, ret);
  }
  ret.isLVPtr = GEN_VAL;
  ret.chplType = type;
  return ret;
}

// Generates code to load the remote locale from a wide address
static GenRet codegenRlocale(GenRet wide)
{
  GenRet ret;
  GenInfo* info = gGenInfo;
  Type* type = LOCALE_ID_TYPE;

  // This function does not work on wide strings.
  if( wide.isLVPtr != GEN_WIDE_PTR && isWideString(wide.chplType)) {
    ret = codegenWideStringField(wide,"locale");
    ret.chplType = type;
    return ret;
  }

  if( widePointersStruct ) {
    if (info->cfile ) {
      if (wide.isLVPtr == GEN_PTR) {
        ret.c += "(" + wide.c + ")->locale";
      } else {
        // could we GEN_WIDE_PTR or a wide reference.
        ret.c += "(" + wide.c + ").locale";
      }
    } else {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) {
        llvm::Value* ptr = info->builder->CreateStructGEP(wide.val,
                                                          WIDE_GEP_LOC);
        ret.val = info->builder->CreateLoad(ptr);
      } else {
        // if wide is not a pointer...
        ret.val = info->builder->CreateExtractValue(wide.val, WIDE_GEP_LOC);
      }
#endif
    }
  } else {
    if( fLLVMWideOpt ) {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) wide = codegenValue(wide);
      GenRet wideTy = wide.chplType; // get the LLVM type for the wide ref.
      llvm::PointerType *addrType = llvm::cast<llvm::PointerType>(wideTy.type);

      // call GLOBAL_FN_GLOBAL_LOCID dummy function 
      llvm::Function* fn = getLocFn(info->module, &info->globalToWideInfo, addrType);
      INT_ASSERT(fn);
      ret.val = info->builder->CreateCall(fn, wide.val);
#endif
    } else {

      // Packed wide pointers
      ret = codegenCallExpr("chpl_wide_ptr_get_localeID",
                            codegenCastWideToVoid(wide));
      if( ret.val ) {
#ifdef HAVE_LLVM
        GenRet expectType = LOCALE_ID_TYPE;
        ret.val = convertValueToType(ret.val, expectType.type);
#endif
      }
    }
  }
  ret.isLVPtr = GEN_VAL;
  ret.chplType = type;
  return ret;
}

// Applies to wide strings only.
// Returns a reference to the size field in a wide string struct.
static GenRet codegenRsize(GenRet wideString)
{
  Type* type = wideString.chplType;
  INT_ASSERT(isWideString(type));
  GenRet ret = codegenWideStringField(wideString, "size");
  return ret;
}

static GenRet codegenRnode(GenRet wide){
  GenRet ret;
  GenInfo* info = gGenInfo;
  Type* type = NODE_ID_TYPE;

  // This function does not work on wide strings.
  if( wide.isLVPtr != GEN_WIDE_PTR && isWideString(wide.chplType)) {
    ret = codegenWideStringField(wide,"locale");
    ret.chplType = LOCALE_ID_TYPE;
    ret = codegenCallExpr("chpl_localeID_get_node", ret);
    ret.chplType = type;
    return ret;
  }

  if( widePointersStruct ) {
    if (info->cfile ) {
      if (wide.isLVPtr == GEN_PTR) {
        ret.c += "(" + wide.c + ")->locale.node";
      } else {
        // could we GEN_WIDE_PTR or a wide reference.
        ret.c += "(" + wide.c + ").locale.node";
      }
    } else {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) {
        llvm::Value* ptr = info->builder->CreateStructGEP(wide.val,
                                                          WIDE_GEP_LOC);
        ptr = info->builder->CreateStructGEP(ptr, WIDE_GEP_LOC_NODE);
        ret.val = info->builder->CreateLoad(ptr);
      } else {
        // if wide is not a pointer...
        ret.val = info->builder->CreateExtractValue(wide.val, WIDE_GEP_LOC);
        ret.val = info->builder->CreateExtractValue(ret.val, WIDE_GEP_LOC_NODE);
      }
#endif
    }
  } else {
    if( fLLVMWideOpt ) {
#ifdef HAVE_LLVM
      if (wide.isLVPtr == GEN_PTR) wide = codegenValue(wide);
      GenRet wideTy = wide.chplType; // get the LLVM type for the wide ref.
      llvm::PointerType *addrType = llvm::cast<llvm::PointerType>(wideTy.type);

      // call GLOBAL_FN_GLOBAL_NODEID dummy function 
      llvm::Function* fn = getNodeFn(info->module, &info->globalToWideInfo, addrType);
      INT_ASSERT(fn);
      ret.val = info->builder->CreateCall(fn, wide.val);
#endif
    } else {
      // Packed wide pointers
      ret = codegenCallExpr("chpl_wide_ptr_get_node",
                            codegenCastWideToVoid(wide));
    }
  }

  ret.chplType = type;
  ret.isLVPtr = GEN_VAL;
  return ret;
}

static GenRet codegenRsubloc(GenRet wide){
  GenRet ret;
  Type* type = SUBLOC_ID_TYPE;

  ret = codegenRlocale(wide);
  ret = codegenCallExpr("chpl_localeID_get_subloc", ret);

  ret.chplType = type;
  ret.isLVPtr = GEN_VAL;
  return ret;
}

static const int field_normal = 0;
static const int field_cid = 1;
static const int field_uid = 2;
static const int field_other = 3;

// Generates code to produce a pointer to the member (ie a field).
// Does not normally do any loads,stores,puts,or gets;
//  just does address arithmetic. The exception is if it has
//  to read an lvalue or when we have a reference to a class.
//
// This function handles the following cases:
//  1 base.chplType is a Chapel class
//  2 base.chplType is a Chapel wide class
//  3 base.chplType is a Chapel record and base.isLVPtr is set
//  4 base.chplType is a Chapel union and base.isLVPtr is set
//  5 base.chplType is a Chapel reference or wide reference type to a record
//  6 base.chplType is a Chapel reference or wide reference type to a union
//  7 base.chplType is a Chapel reference or wide reference to
//    a class or wide class (* causes a load)
//
//  In addition, it handles some special cases which are not reflected
//   in the Chapel type system, like getting the class ID or union ID
//   fields.
//
//  In any case, returns a GEN_PTR or GEN_WIDE_PTR to the field.
//
//  This is equivalent to C (assuming x has ptr type e.g. struct mystruct*)
//    & x->myfield
//
static
GenRet codegenFieldPtr(
    GenRet base,
    const char *c_field_name,
    const char* chpl_field_name,
    int special /* field_normal,field_cid, or field_uid */ ) {
  GenInfo* info = gGenInfo;
  GenRet ret;
  Type* baseType = base.chplType;
  ClassType* ct = NULL;
  Type* castType = NULL;
  
  if( special == field_normal ) {
    INT_ASSERT(baseType);
  }

  if( baseType ) {
    // This function does not work on wide strings.
    if( isWideString(baseType) )
      INT_FATAL("can't use codegenFieldPtr on wide string");

    // Reduce the Chapel reference or wide reference cases
    // to GEN_PTR or GEN_WIDE_PTR cases.
    if (baseType->symbol->hasEitherFlag(FLAG_REF,FLAG_WIDE)) {
      base = codegenDeref(base);
      return codegenFieldPtr(base, c_field_name, chpl_field_name, special);
    }
  }

  if( ! fLLVMWideOpt ) {
    // Reduce GEN_WIDE_PTR or FLAG_WIDE_CLASS cases to local versions
    // and rebuild addresses.
    if( base.isLVPtr == GEN_WIDE_PTR ||
        baseType->symbol->hasFlag(FLAG_WIDE_CLASS) ) {
      GenRet addr;
      addr = codegenRaddr(base);
      addr = codegenFieldPtr(addr, c_field_name, chpl_field_name, special);
      ret = codegenWideAddrWithAddr(base, addr);
      return ret;
    }
  }

  if( baseType ) {
    // At this point, baseType should be a record, union, class, or wide class
    // All of these types are in the ClassType AST node.
    ct = toClassType(baseType);
    INT_ASSERT(ct);

    if ( isClass(ct) ) {
      // ok, we have a class type. We should codegenValue
      // to make sure we have no extra indirection.
      base = codegenValue(base);
    } else if ( baseType->symbol->hasFlag(FLAG_WIDE_CLASS)) {
      // Get the local version of the class (because it has the fields)
      base = codegenValue(base);
      baseType = baseType->getField("addr")->typeInfo();
      ct = toClassType(baseType);
    } else {
      // Must be a record or union type, and we must have an
      // lvalue-ptr to one of them.
      INT_ASSERT(isRecord(ct) || isUnion(ct));
      INT_ASSERT( base.isLVPtr != GEN_VAL );
    }
  }

  // No Chapel field name? it must be special.
  if( !chpl_field_name && !special ) special = field_other;

  if( special ) {
    if( special == field_cid ) {
      INT_ASSERT( ct && isClass(ct) );
      ret.chplType = CLASS_ID_TYPE;
      castType = dtObject;
    } else if( special == field_uid ) {
      ret.chplType = UNION_ID_TYPE;
    } else {
      ret.chplType = NULL;
    }
  } else if( ct ) {
    // The field might be in a base class, so we
    // cast to the right base class type. If the field
    // is in the class, there is no need to cast.
    Symbol* fieldSymbol = ct->getField(chpl_field_name);
    if( isClass(ct) ) {
      castType = fieldSymbol->defPoint->parentSymbol->typeInfo();
      if( castType == ct ) castType = NULL;
    }
    ret.chplType = fieldSymbol->typeInfo();
  }

  if( fLLVMWideOpt && castType && isWide(base) ) {
    // for fLLVMWideOpt
    castType = getOrMakeWideTypeDuringCodegen(castType);
  }

  ret.isLVPtr = GEN_PTR;
  // with LLVMWideOpt, we might return a wide ptr.
  if( fLLVMWideOpt && isWide(base) ) ret.isLVPtr = GEN_WIDE_PTR;

  if (isClass(ct) ) {
    base = codegenValue(base);
  } else {
    // not a class. base is a lvalue pointer.
    if( !fLLVMWideOpt ) INT_ASSERT(base.isLVPtr == GEN_PTR);
    else INT_ASSERT(base.isLVPtr != GEN_VAL);
  }
  if( info->cfile ) {
    ret.c = '&';
    ret.c += "(";
    if( castType ) ret.c += codegenCast(castType,base).c;
    else ret.c += "(" + base.c + ")";
    ret.c += "->";
    if (isUnion(ct) && !special)
      ret.c += "_u.";
    ret.c += c_field_name;
    ret.c += ")";
  } else {
#ifdef HAVE_LLVM
    // LLVM codegen
    llvm::Value* baseValue = base.val;

    // cast if needed
    if (castType) {
      llvm::Type* castTypeLLVM = castType->codegen().type;
      baseValue = convertValueToType(base.val, castTypeLLVM, !base.isUnsigned);
      INT_ASSERT(baseValue);
    }

    ClassType *cBaseType = toClassType(baseType);

    if( isUnion(ct) && !special ) {
      // Get a pointer to the union data then cast it to the right type
      ret.val = info->builder->CreateConstInBoundsGEP2_32(
          baseValue, 0, cBaseType->getMemberGEP("_u"));
      GenRet retType = ret.chplType;
      llvm::PointerType* ty =
        llvm::PointerType::get(retType.type,
                               baseValue->getType()->getPointerAddressSpace());
      // Now cast it to the right type.
      ret.val = convertValueToType(ret.val, ty, false);
      INT_ASSERT(ret.val);
    } else {
      // Normally, we just use a GEP.
      ret.val = info->builder->CreateConstInBoundsGEP2_32(
          baseValue, 0, cBaseType->getMemberGEP(c_field_name));
    }
#endif
  }
  return ret;
}

void DefExpr::prettyPrint(std::ofstream *o) {
  *o << "<DefExprType>";
}

static
GenRet codegenFieldPtr(GenRet base, Expr* field) {
  const char* cname = NULL;
  const char* name = NULL;
  if(DefExpr *de = toDefExpr(field)) {
    cname = de->sym->cname;
    name = de->sym->name;
  } else if(SymExpr *se = toSymExpr(field)) {
    cname = se->var->cname;
    name = se->var->name;
  } else if(NamedExpr *ne = toNamedExpr(field)) {
    cname = name = ne->name;
  } else {
    INT_FATAL("Unknown field in codegenFieldPtr");
  }
  return codegenFieldPtr(base, cname, name, field_normal);
}

static
GenRet codegenFieldPtr(GenRet base, Symbol* field) {
  const char* cname = NULL;
  const char* name = NULL;
  cname = field->cname;
  name = field->name;
  return codegenFieldPtr(base, cname, name, field_normal);
}

static
GenRet codegenFieldPtr(GenRet base, const char* field) {
  const char* cname = NULL;
  const char* name = NULL;
  cname = field;
  name = field;
  return codegenFieldPtr(base, cname, name, field_normal);
}

static
GenRet codegenFieldCidPtr(GenRet base) {
  GenRet ret = codegenFieldPtr(base, "chpl__cid", NULL, field_cid);
  //if( ! ret.chplType ) ret.chplType = CLASS_ID_TYPE;
  return ret;
}

static
GenRet codegenFieldUidPtr(GenRet base) {
  GenRet ret = codegenFieldPtr(base, "_uid", NULL, field_uid);
  //if( ! ret.chplType ) ret.chplType = UNION_ID_TYPE;
  return ret;
}


// Generates code to produce a pointer an array element.
//
// Handles the following cases:
//  1 base.chplType is a data class (ie _ddata)
//  2 base.chplType is a wide data class
//  3 base.chplType is a homogeneous tuple (aka star tuple) and isLVPtr != 0
//  4 base.chplType is a Chapel reference or wide reference
//    to a data class, wide data class, or homogeneous tuple.
//
// In any case, returns a GEN_PTR or GEN_WIDE_PTR to the field.
//
// This is equivalent to C (assuming ptr is a pointer type)
//   ptr + i
//
static
GenRet codegenElementPtr(GenRet base, GenRet index) {
  GenRet ret;
  GenInfo* info = gGenInfo;
  Type* baseType = NULL;
  Type* eltType = NULL;
  std::string addr;
  bool isStarTuple = false;

  INT_ASSERT(base.chplType);

  // Handle references to arrays or star tuples
  // by converting them to isLVPtr != GEN_VAL
  if( base.chplType->symbol->hasEitherFlag(FLAG_REF,FLAG_WIDE) ) {
    base = codegenDeref(base);
  }

  baseType = base.chplType;

  // Now we should either have:
  //  - wide data class
  //  - data class
  //  - star tuple with isLVPtr != 0

  if( ! fLLVMWideOpt ) {
    // Convert wide pointer operations to the local counterparts.
    if( base.isLVPtr == GEN_WIDE_PTR ||
        baseType->symbol->hasFlag(FLAG_WIDE_CLASS) ) {
      GenRet newAddr = codegenElementPtr(codegenRaddr(base), index);
      return codegenWideAddrWithAddr(base, newAddr);
    }
  }

  ret.isLVPtr = GEN_PTR;
  if( fLLVMWideOpt && isWide(base) ) ret.isLVPtr = GEN_WIDE_PTR;

  if( baseType->symbol->hasFlag(FLAG_STAR_TUPLE) ) {
    eltType = baseType->getField("x1")->typeInfo();
    isStarTuple = true;
    // Star tuples should only be passed by reference here...
    INT_ASSERT(base.isLVPtr != GEN_VAL);
  } else if( baseType->symbol->hasFlag(FLAG_DATA_CLASS) ) {
    eltType = getDataClassType(baseType->symbol)->typeInfo();
    isStarTuple = false;
  }

  ret.chplType = eltType;

  index = codegenValue(index);
  if( !isStarTuple ) base = codegenValue(base);

  if( info->cfile ) {
    base = codegenValue(base); // even for tuple, for style.
    ret.c = "(" + base.c + " + " + index.c + ")";
  } else {
#ifdef HAVE_LLVM
    // in LLVM, arrays are not pointers and cannot be used in
    // calls to CreateGEP, CreateCall, CreateStore, etc.
    // so references to arrays must be used instead
    // (i.e. if it is a reference to an array, do not deref)
    std::vector<llvm::Value *> GEPLocs;

    // add zero as first index if tuple
    if (isStarTuple){
      GEPLocs.push_back(
          llvm::Constant::getNullValue(
            llvm::IntegerType::getInt64Ty(info->module->getContext())));
    }
    GEPLocs.push_back(index.val);

    ret.val = info->builder->CreateInBoundsGEP(base.val, GEPLocs);
#endif
  }

  return ret;
}

#ifdef HAVE_LLVM
llvm::Value* createTempVarLLVM(llvm::Type* type)
{
  char name[32];
  sprintf(name, "chpl_macro_tmp_%d", codegen_tmp++);
  return createTempVarLLVM(type, name);
}
#endif

static
GenRet createTempVar(const char* ctype)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  char name[32];
  sprintf(name, "chpl_macro_tmp_%d", codegen_tmp++);
  
  ret.isLVPtr = GEN_PTR;
  if( info->cfile ) {
    // Add a temporary variable
    info->cLocalDecls.push_back(std::string(ctype) + " " + name);
    ret.c = std::string("&") + name;
  } else {
#ifdef HAVE_LLVM
    llvm::Type* llTy = info->lvt->getType(ctype);
    INT_ASSERT(llTy);
    ret.val = createTempVarLLVM(llTy, name);
#endif
  }
  return ret;
}

// use this function for chplTypes
static GenRet createTempVar(Type* t)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  if( info->cfile ) {
    // Just use the C-name.
    ret = createTempVar(t->symbol->cname);
  } else {
#ifdef HAVE_LLVM
    // We need to code-generate the type in the event
    // that it does not exist. That happens for some
    // types that are constructed during code generation
    // (to do with references and references pointers)
    // It's not a problem for C since the type will
    // be added to the header before the C compiler runs.
    GenRet tmp = t;
    llvm::Type* llTy = tmp.type;
    INT_ASSERT(llTy);
    ret.isLVPtr = GEN_PTR;
    ret.val = createTempVarLLVM(llTy);
#endif
  }
  ret.chplType = t;
  return ret;
}

GenRet createTempVarWith(GenRet v)
{
  GenInfo* info = gGenInfo;
  Type* t = v.chplType;
  INT_ASSERT(t);
  GenRet ret = createTempRef(t);
  ret.isUnsigned = v.isUnsigned;
  // now store into the temp var the value we have.
  if( info->cfile ) {
    std::string stmt = codegenValue(ret).c + " = " + codegenValue(v).c + ";\n";
    info->cStatements.push_back(stmt);
  } else {
#ifdef HAVE_LLVM
    codegenStoreLLVM(codegenValue(v), ret);
#endif
  }
  return ret;
}


// For C code generation
// Codegen *(ptr), but we optimize away
// & or &(something)
// if GenRet is a wide pointer, we will emit a 'get'.
// For a star tuple, if we have a reference to a star tuple,
// returns the base address.
GenRet codegenValue(GenRet r)
{
  GenInfo* info = gGenInfo;
  GenRet ret = r;
  ret.isLVPtr = GEN_VAL;

  if( r.isLVPtr == GEN_VAL ) return ret;
  if( r.isLVPtr == GEN_WIDE_PTR && !fLLVMWideOpt) {
    // with fLLVMWideOpt, we can just load directly below.
    assert(r.chplType);
 
    // Emit a temporary.
    // Assign from wide pointer value into temporary
    // Return local pointer to temporary
    ret = createTempRef(r.chplType);
    codegenAssign(ret, r);
    return codegenValue(ret);
  }

  // At this point r.isPtr == GEN_PTR.
  if( r.chplType ) {
    // If we have a Chapel type, propagate it.
    ret.chplType = r.chplType;
    // NOT value type if it's a reference, since
    // codegenValue on a Chapel reference just returns the pointer!
  }

  if( info->cfile ) {
    INT_ASSERT(r.c.length() > 0);
    if( r.c[0] == '&' ) {
      if( r.c[1] == '(' && r.c[r.c.length()-1] == ')' ) {
        // we have &(something)
        ret.c = r.c.substr(2, r.c.length()-3);
      } else {
        // we have &something
        ret.c = r.c.substr(1, r.c.length()-1);
      }
    } else if( r.c[0] == '(' && r.c[r.c.length()-1] == ')') {
      // we have (something)
      ret.c = "*" + r.c;
    } else {
      ret.c = "*(" + r.c + ")";
    }
  } else {
#ifdef HAVE_LLVM
    if (r.isLVPtr) {
      ret.val = codegenLoadLLVM(r); // TODO - is r pointer to const?
    } else {
      ret.val = r.val;
    }
#endif
  }
  return ret;
}

// Create a temporary
// value holding r and return a pointer to it.
// If r is already a pointer, do nothing.
// Does not handle homogeneous tuples.
// Does not handle wide pointers.
GenRet codegenValuePtr(GenRet r)
{
  GenRet ret = r;

  if( ret.isLVPtr == GEN_PTR ) return ret;

  if( r.chplType ) {
    bool isStarTuple = r.chplType->symbol->hasFlag(FLAG_STAR_TUPLE);
    INT_ASSERT(!isStarTuple);
  } 

  INT_ASSERT(r.isLVPtr != GEN_WIDE_PTR);
  ret = createTempVarWith(r);
  return ret;
}


// Converts an L-value pointer into a
// pointer value, so that it can for example
// be stored in another pointer.
static GenRet codegenAddrOf(GenRet r)
{
  GenRet ret = r;

  if (r.isLVPtr == GEN_WIDE_PTR) {
    if(r.chplType) {
      Type* refType = getOrMakeRefTypeDuringCodegen(r.chplType);
      ret.chplType = getOrMakeWideTypeDuringCodegen(refType);
    }
    ret.isLVPtr = GEN_VAL;
    return ret;
  } else if( r.isLVPtr == GEN_PTR ) {
    if(r.chplType) ret.chplType = getOrMakeRefTypeDuringCodegen(r.chplType);
    ret.isLVPtr = GEN_VAL;
  } else {
    INT_FATAL("misuse of codegenAddrOf");
  }

  return ret;
}
// Converts an L-value pointer into a
// pointer value, so that it can for example
// be stored in another pointer.
// If we start with a wide pointer, we just discard
// the locale portion (ie assume it is local).
static
GenRet codegenLocalAddrOf(GenRet r)
{
  if (r.isLVPtr == GEN_WIDE_PTR) {
    return codegenRaddr(r);
  }
  return codegenAddrOf(r);
}



GenRet codegenLocalDeref(GenRet r)
{
  GenRet ret;
  // LocalDeref on a wide pointer should just give
  // the address field as a reference.
  if( r.chplType && r.chplType->symbol->hasFlag(FLAG_WIDE) ) {
    ret = codegenRaddr(r);
    return ret;
  }
  // For some reason, ArgSymbol might not have a real Chapel
  // reference type, so we have this function as a workaround
  // (instead of running codegenDeref with chplType=type->refType )
  ret = codegenValue(r);
  ret.isLVPtr = GEN_PTR;
  if( r.chplType ) ret.chplType = r.chplType->getValType(); 
  return ret;
}

// codegenValue(r) to remove & or add * (if & already removed) and sets isLVPtr
GenRet codegenDeref(GenRet r)
{
  GenRet ret;

  INT_ASSERT(r.chplType);
  if (r.chplType->symbol->hasEitherFlag(FLAG_WIDE, FLAG_WIDE_CLASS)) {
    ret = codegenValue(r);
    ret.isLVPtr = GEN_WIDE_PTR;
    ret.chplType = r.chplType->getValType();
  } else if ( r.chplType->symbol->hasFlag(FLAG_REF) ){
    return codegenLocalDeref(r);
  } else {
    INT_ASSERT(0); // not a reference.
  }

  return ret;
}

static
GenRet codegenEquals(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  ret.chplType = dtBool;
  if( info->cfile ) ret.c = "(" + av.c + " == " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
   // if type mismatch, create cast on RHS
   if (av.val->getType() != bv.val->getType()){
     bv.val = convertValueToType(bv.val, av.val->getType(), !bv.isUnsigned);
     INT_ASSERT(bv.val);
   }
   if( av.val->getType()->isFPOrFPVectorTy() ) {
     ret.val = info->builder->CreateFCmpOEQ(av.val, bv.val);
   } else {
     ret.val = info->builder->CreateICmpEQ(av.val, bv.val);
   }
#endif
  }
  return ret;
}

static
GenRet codegenNotEquals(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  ret.chplType = dtBool;
  if( info->cfile ) ret.c = "(" + av.c + " != " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
   // if type mismatch, create cast on RHS
   if (av.val->getType() != bv.val->getType()){
     bv.val = convertValueToType(bv.val, av.val->getType(), !bv.isUnsigned);   
     INT_ASSERT(bv.val);
   }
   if( av.val->getType()->isFPOrFPVectorTy() ) {
     ret.val = info->builder->CreateFCmpONE(av.val, bv.val);
   } else {
     ret.val = info->builder->CreateICmpNE(av.val, bv.val);
   }
#endif
  }
  return ret;
}

static
GenRet codegenLogicalOr(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  ret.chplType = dtBool;
  if( info->cfile ) ret.c = "(" + av.c + " || " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
    ret.val = info->builder->CreateOr(info->builder->CreateIsNotNull(av.val),
                                      info->builder->CreateIsNotNull(bv.val));
#endif
  }
  return ret;
}
static
GenRet codegenLogicalAnd(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  ret.chplType = dtBool;
  if( info->cfile ) ret.c = "(" + av.c + " && " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
    ret.val = info->builder->CreateAnd(info->builder->CreateIsNotNull(av.val),
                                       info->builder->CreateIsNotNull(bv.val));
#endif
  }
  return ret;
}


static
GenRet codegenAdd(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  if( info->cfile ) ret.c = "(" + av.c + " + " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
    bool a_signed = false;
    bool b_signed = false;
    if( a.chplType ) a_signed = is_signed(a.chplType);
    if( b.chplType ) b_signed = is_signed(b.chplType);
    PromotedPair values =
      convertValuesToLarger(av.val, bv.val, a_signed, b_signed);
    if(values.a->getType()->isFPOrFPVectorTy()) {
      ret.val = info->builder->CreateFAdd(values.a, values.b);
    } else {
      ret.val = info->builder->CreateAdd(values.a, values.b);
    }
    ret.isUnsigned = !values.isSigned;
#endif
  }
  return ret;
}

static
GenRet codegenSub(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  if( info->cfile ) ret.c = "(" + av.c + " - " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
    bool a_signed = false;
    bool b_signed = false;
    if( a.chplType ) a_signed = is_signed(a.chplType);
    if( b.chplType ) b_signed = is_signed(b.chplType);
    PromotedPair values =
      convertValuesToLarger(av.val, bv.val, a_signed, b_signed);
    if(values.a->getType()->isFPOrFPVectorTy()) {
      ret.val = info->builder->CreateFSub(values.a, values.b);
    } else {
      ret.val = info->builder->CreateSub(values.a, values.b);
    }
    ret.isUnsigned = !values.isSigned;
#endif
  }
  return ret;
}

static
GenRet codegenNeg(GenRet a)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  if( info->cfile ) ret.c = "(-" + av.c + ")";
  else {
#ifdef HAVE_LLVM
    llvm::Value *value = av.val;
    if(value->getType()->isFPOrFPVectorTy()) {
      ret.val = info->builder->CreateFNeg(value);
    } else {
      ret.val = info->builder->CreateNeg(value);
    }
    ret.isUnsigned = false;
#endif
  }
  return ret;
}


static
GenRet codegenMul(GenRet a, GenRet b)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  GenRet av = codegenValue(a);
  GenRet bv = codegenValue(b);
  if( info->cfile ) ret.c = "(" + av.c + " * " + bv.c + ")";
  else {
#ifdef HAVE_LLVM
    bool a_signed = false;
    bool b_signed = false;
    if( a.chplType ) a_signed = is_signed(a.chplType);
    if( b.chplType ) b_signed = is_signed(b.chplType);
    PromotedPair values =
      convertValuesToLarger(av.val, bv.val, a_signed, b_signed);
    if(values.a->getType()->isFPOrFPVectorTy()) {
      ret.val = info->builder->CreateFMul(values.a, values.b);
    } else {
      ret.val = info->builder->CreateMul(values.a, values.b);
    }
    ret.isUnsigned = !values.isSigned;
#endif
  }
  return ret;
}



static
GenRet codegenTernary(GenRet cond, GenRet ifTrue, GenRet ifFalse)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  Type* type = ifTrue.chplType;
  if( ! type ) type = ifFalse.chplType;
  ret.chplType = type;
#ifdef HAVE_LLVM
  bool ifTrueSigned = !ifTrue.isUnsigned;
  bool ifFalseSigned = !ifFalse.isUnsigned;
  if( ifTrue.chplType ) ifTrueSigned = is_signed(ifTrue.chplType);
  if( ifFalse.chplType ) ifFalseSigned = is_signed(ifFalse.chplType);
#endif

  if( info->cfile ) {
    ret.c = "(" + cond.c + ")?(" + ifTrue.c + "):(" + ifFalse.c + ")";
  } else {
#ifdef HAVE_LLVM
    llvm::Function *func = info->builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *blockIfTrue =llvm::BasicBlock::Create(
        info->module->getContext(), "ternaryBlockIfTrue");
    llvm::BasicBlock *blockIfFalse = llvm::BasicBlock::Create(
        info->module->getContext(), "ternaryBlockIfFalse");
    llvm::BasicBlock *blockEnd = llvm::BasicBlock::Create(
        info->module->getContext(), "ternaryBlockEnd");

    GenRet ifTrueVal = codegenValue(ifTrue);
    GenRet ifFalseVal = codegenValue(ifFalse);
    PromotedPair values = convertValuesToLarger(
        ifTrueVal.val, ifFalseVal.val, ifTrueSigned, ifFalseSigned);

    char name[32];
    sprintf(name, "chpl_macro_tmp_tv_%d", codegen_tmp++);

    llvm::Value* tmp = createTempVarLLVM(values.a->getType(), name);

    info->builder->CreateCondBr(
        codegenValue(cond).val, blockIfTrue, blockIfFalse);

    func->getBasicBlockList().push_back(blockIfTrue);
    info->builder->SetInsertPoint(blockIfTrue);
    info->builder->CreateStore(values.a, tmp);
    info->builder->CreateBr(blockEnd);

    func->getBasicBlockList().push_back(blockIfFalse);
    info->builder->SetInsertPoint(blockIfFalse);
    info->builder->CreateStore(values.b, tmp);
    info->builder->CreateBr(blockEnd);

    func->getBasicBlockList().push_back(blockEnd);
    info->builder->SetInsertPoint(blockEnd);
    ret.val = info->builder->CreateLoad(tmp);
    ret.isUnsigned = !values.isSigned;
#endif
  }
  return ret;
}

// AKA == null
static
GenRet codegenIsZero(GenRet x)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  if (x.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_WIDE_CLASS) ) {
    x = codegenRaddr(x);
    if (info->cfile) {
      ret.c = x.c;
      ret.c += " == nil";
    } else {
#ifdef HAVE_LLVM
      ret.val = info->builder->CreateIsNull(x.val);
#endif
    }
  } else {
    GenRet xv = codegenValue(x);
    if( info->cfile ) ret.c = "(! " + xv.c + ")";
    else {
#ifdef HAVE_LLVM
      ret.val = info->builder->CreateIsNull(xv.val);
#endif
    }
  }

  return ret;
}

// AKA != null
static
GenRet codegenIsNotZero(GenRet x)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  if (x.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_WIDE_CLASS) ) {
    x = codegenRaddr(x);
    if (info->cfile) {
      ret.c = x.c;
      ret.c += " != nil";
    } else {
#ifdef HAVE_LLVM
      ret.val = info->builder->CreateIsNotNull(x.val);
#endif
    }
  } else { 
    GenRet xv = codegenValue(x);
    if( info->cfile ) ret.c = "(!(! " + xv.c + "))";
    else {
#ifdef HAVE_LLVM
      ret.val = info->builder->CreateIsNotNull(xv.val);
#endif
    }
  }
  return ret;
}

static
GenRet codegenDynamicCastCheck(GenRet cid, Type* type)
{
  GenRet ret = codegenEquals(cid, codegenUseCid(type));
  forv_Vec(Type, child, type->dispatchChildren) {
    ret = codegenLogicalOr(ret, codegenDynamicCastCheck(cid, child));
  }
  return ret;
}

static void
codegenNullAssignments(FILE* outfile,
                       const char* cname,
                       ClassType* ct,
                       int skip=0) {
  if (!skip && isClass(ct))
    fprintf(outfile, "%s = NULL;\n", cname);
  else {
    for_fields(field, ct) {
      if (ClassType* fct = toClassType(field->type)) {
        char buffer[1024];
        strcpy(buffer, cname);
        if (skip)
          strcat(buffer, "->");
        else
          strcat(buffer, ".");
        strcat(buffer, field->cname);
        codegenNullAssignments(outfile, buffer, fct, 0);
      }
    }
  }
}

#ifdef HAVE_LLVM
static 
void convertArgumentForCall(llvm::FunctionType *fnType,
                            GenRet arg,
                            std::vector<llvm::Value*> & outArgs)
{
  GenInfo* info = gGenInfo;

  llvm::Value* v = arg.val;
  llvm::Type* t = v->getType();


  bool isSigned = false;
  if( arg.chplType ) isSigned = is_signed(arg.chplType);

  llvm::Type* targetType = NULL;
  if( outArgs.size() < fnType->getNumParams() ) {
    targetType = fnType->getParamType(outArgs.size());
  }

  // Check that we're not casting between global address
  // space and local address space pointers (since that
  // would be invalid!)
  if( targetType ) {
    llvm::PointerType* tgtPtr = llvm::dyn_cast<llvm::PointerType>(targetType);
    llvm::PointerType* tPtr = llvm::dyn_cast<llvm::PointerType>(t);
    if( tgtPtr && tPtr ) {
      bool tgtGlobal =
        tgtPtr->getAddressSpace() == info->globalToWideInfo.globalSpace;
      bool tGlobal =
        tPtr->getAddressSpace() == info->globalToWideInfo.globalSpace;
      INT_ASSERT(tgtGlobal == tGlobal);
    }
  }

  llvm::Value* out;
  if( targetType ) out = convertValueToType(v, targetType, isSigned);
  else out = v; // no target type means we just emit it.

  if( out ) {
    // OK, we were able to emit it...
    outArgs.push_back(out);
  } else if( t->isEmptyTy() ) {
    // OK, just don't emit an argument at all.
  } else if( t->isStructTy() || t->isArrayTy() || t->isVectorTy() ) {
    // We might need to put the arguments in one-at-a-time,
    // in order to put up with structure expansion done by clang.
    // TODO - this should actually depend on the clang ABI,
    // or else we should find a way to disable the optimization in clang.
    //   It should be possible to get the necessariy information from clang
    //   with cgModule->getTypes()->arrangeFunctionDeclaration(FunctionDecl)

    if( t->isStructTy() || t->isArrayTy() ) {
      unsigned n;
      if( t->isStructTy() ) n = t->getStructNumElements();
      else n = t->getArrayNumElements();
      for( unsigned i = 0; i < n; i++ ) {
        unsigned indexes[1] = { i };
        GenRet r;
        r.val = info->builder->CreateExtractValue(v, indexes);
        convertArgumentForCall(fnType, r, outArgs);
      }
    } else {
      // vector types.
      unsigned n = t->getVectorNumElements();
      for( unsigned i = 0; i < n; i++ ) {
        GenRet r;
        r.val =
          info->builder->CreateExtractElement(
              v,
              llvm::ConstantInt::get(
                llvm::IntegerType::getInt64Ty(info->llvmContext), i));
        convertArgumentForCall(fnType, r, outArgs);
      }
    }
  } else {
    INT_FATAL("Could not convert arguments for call");
  }
}

#endif

static
GenRet codegenArgForFormal(GenRet arg,
                           ArgSymbol* formal,
                           bool defaultToValues,
                           bool isExtern)
{
  // NOTE -- VMT call had add & if arg isRecord.
  if( formal ) {
    if (!isExtern &&
        formal->requiresCPtr() &&
        !formal->type->symbol->hasFlag(FLAG_REF)) { 
      if( arg.isLVPtr == GEN_VAL ) {
        arg = codegenValuePtr(arg);
      }
    } else {
      if( arg.isLVPtr != GEN_VAL ) {
        arg = codegenValue(arg);
      }
    }
  } else {
    if( defaultToValues ) {
      if( arg.isLVPtr != GEN_VAL ) {
        arg = codegenValue(arg);
      }
    } // otherwise, leave it be.
  }
  return arg;
}

// if fSym is non-NULL, we use that to decide what to dereference.
// Otherwise, if defaultToValues=true, we will codegenValue() the arguments,
//            and if it is false, they will pass by reference if they
//            are references.
static
GenRet codegenCallExpr(GenRet function,
                       std::vector<GenRet> & args,
                       FnSymbol* fSym,
                       bool defaultToValues)
{
  GenInfo* info = gGenInfo;
  GenRet ret;


  if( info->cfile ) {
    ret.c = function.c;
    ret.c += '(';
    bool first_actual = true;
    for( size_t i = 0; i < args.size(); i++ ) {
      {
        // Convert formals if we have fSym
        ArgSymbol* formal = NULL;
        bool isExtern = true;
        if( fSym ) {
          Expr* e = fSym->formals.get(i + 1);
          DefExpr* de = toDefExpr(e);
          formal = toArgSymbol(de->sym);
          INT_ASSERT(formal);
          if (!fSym->hasFlag(FLAG_EXTERN))
            isExtern = false;
        }
        args[i] =
          codegenArgForFormal(args[i], formal, defaultToValues, isExtern);
      }

      if (first_actual)
        first_actual = false;
      else
        ret.c += ", ";
      ret.c += args[i].c;
    }
    ret.c += ')';
  } else {
#ifdef HAVE_LLVM
    INT_ASSERT(function.val);
    llvm::Value *val = function.val;
    // Maybe function is bit-cast to a pointer?
    llvm::Function *func = llvm::dyn_cast<llvm::Function>(val);
    llvm::FunctionType *fnType;
    if (func){
      fnType = func->getFunctionType();
    } else {
      fnType = llvm::cast<llvm::FunctionType>(
          val->getType()->getPointerElementType());
    }

    std::vector<llvm::Value *> llArgs;
    llvm::Value* sret = NULL;

    // We might be doing 'structure return'
    if( fnType->getReturnType()->isVoidTy() &&
        fnType->getNumParams() >= 1 &&
        func && func->hasStructRetAttr() ) {
      // We must allocate a temporary to store the return value
      llvm::PointerType* ptrToRetTy = llvm::cast<llvm::PointerType>(
          fnType->getParamType(0));
      llvm::Type* retTy = ptrToRetTy->getElementType();
      sret = createTempVarLLVM(retTy);
      llArgs.push_back(sret);
    }

    for( size_t i = 0; i < args.size(); i++ ) {
      // If we are passing byval, get the pointer to the
      // argument
      if( llArgs.size() < fnType->getNumParams() &&
          func &&
          llvm_fn_param_has_attr(func,llArgs.size()+1,LLVM_ATTRIBUTE::ByVal) ){
        args[i] = codegenAddrOf(codegenValuePtr(args[i]));
        // TODO -- this is not working!
      }

      // Convert formals if we have fSym
      {
        ArgSymbol* formal = NULL;
        bool isExtern = true;
        if( fSym ) {
          Expr* e = fSym->formals.get(i + 1);
          DefExpr* de = toDefExpr(e);
          formal = toArgSymbol(de->sym);
          INT_ASSERT(formal);
          if (!fSym->hasFlag(FLAG_EXTERN))
            isExtern = false;
        }
        args[i] =
          codegenArgForFormal(args[i], formal, defaultToValues, isExtern);
      }

      // Handle structure expansion done by clang.
      convertArgumentForCall(fnType, args[i], llArgs);
    }
    
    if (func) { 
      ret.val = info->builder->CreateCall(func, llArgs);
    } else {
      ret.val = info->builder->CreateCall(val, llArgs);
    }

    if( sret ) {
      ret.val = codegenLoadLLVM(sret, fSym->retType);
    }
#endif
  }
  return ret;
}

static
GenRet codegenCallExpr(const char* fnName,
                       std::vector<GenRet> & args,
                       bool defaultToValues)
{
  GenInfo* info = gGenInfo;
  GenRet fn;
  if( info->cfile ) fn.c = fnName;
  else {
#ifdef HAVE_LLVM
    fn.val = getFunctionLLVM(fnName);
    INT_ASSERT(fn.val);
#endif
  }
  return codegenCallExpr(fn, args, NULL, defaultToValues);
}

static
void codegenCall(const char* fnName, std::vector<GenRet> & args, bool defaultToValues)
{
  GenInfo* info = gGenInfo;
  GenRet ret = codegenCallExpr(fnName, args, defaultToValues);
  if( info->cfile ) {
    info->cStatements.push_back(ret.c + ";\n");
  }
}

/* These overloads of codegenCall are a bit boring-looking,
 * but they make it much easier to write the primitive call
 * generation in Expr::codegen
 */
static
GenRet codegenCallExpr(const char* fnName)
{
  std::vector<GenRet> args;
  return codegenCallExpr(fnName, args);
}
static
GenRet codegenCallExpr(const char* fnName, GenRet a1)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  return codegenCallExpr(fnName, args);
}
static
GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  return codegenCallExpr(fnName, args);
}
static
GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  return codegenCallExpr(fnName, args);
}
static
GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                       GenRet a4)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  return codegenCallExpr(fnName, args);
}
static
GenRet codegenCallExpr(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                       GenRet a4, GenRet a5)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  return codegenCallExpr(fnName, args);
}


/* static
void codegenCall(const char* fnName)
{
  std::vector<GenRet> args;
  codegenCall(fnName, args);
}*/
static
void codegenCall(const char* fnName, GenRet a1)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  codegenCall(fnName, args);
}
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  codegenCall(fnName, args);
}
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  codegenCall(fnName, args);
}
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3, GenRet a4)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  codegenCall(fnName, args);
}
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  codegenCall(fnName, args);
}
/*static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  codegenCall(fnName, args);
}
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6, GenRet a7)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  args.push_back(a7);
  codegenCall(fnName, args);
}
*/
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6, GenRet a7, GenRet a8)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  args.push_back(a7);
  args.push_back(a8);
  codegenCall(fnName, args);
}

/*
static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6, GenRet a7, GenRet a8,
                 GenRet a9)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  args.push_back(a7);
  args.push_back(a8);
  args.push_back(a9);
  codegenCall(fnName, args);
}*/
/*static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6, GenRet a7, GenRet a8,
                 GenRet a9, GenRet a10)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  args.push_back(a7);
  args.push_back(a8);
  args.push_back(a9);
  args.push_back(a10);
  codegenCall(fnName, args);
}*/

static
void codegenCall(const char* fnName, GenRet a1, GenRet a2, GenRet a3,
                 GenRet a4, GenRet a5, GenRet a6, GenRet a7, GenRet a8,
                 GenRet a9, GenRet a10, GenRet a11)
{
  std::vector<GenRet> args;
  args.push_back(a1);
  args.push_back(a2);
  args.push_back(a3);
  args.push_back(a4);
  args.push_back(a5);
  args.push_back(a6);
  args.push_back(a7);
  args.push_back(a8);
  args.push_back(a9);
  args.push_back(a10);
  args.push_back(a11);
  codegenCall(fnName, args);
}

static 
GenRet codegenBasicPrimitiveExpr(CallExpr* call) {
  std::vector<GenRet> args;

  for_actuals(actual, call) {
    GenRet gen = actual;

    // Make wide pointers/classes local
    if (actual->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) ||
        actual->typeInfo()->symbol->hasFlag(FLAG_WIDE))
      gen = codegenRaddr(gen);

    // Dereference reference or now-local wide reference
    if (actual->typeInfo()->symbol->hasFlag(FLAG_REF) ||
        actual->typeInfo()->symbol->hasFlag(FLAG_WIDE))
      gen = codegenDeref(gen);

    gen = codegenValue(gen);
    args.push_back(gen);
  }
 
  return codegenCallExpr(call->primitive->name, args);
}

static
GenRet codegenZero()
{
  return new_IntSymbol(0, INT_SIZE_64)->codegen();
}

static
GenRet codegenOne()
{
  return new_IntSymbol(1, INT_SIZE_64)->codegen();
}

static
GenRet codegenNullPointer()
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  ret.chplType = dtNil;
  if( info->cfile ) {
    ret.c = "NULL";
  } else {
#ifdef HAVE_LLVM
    ret.val = llvm::Constant::getNullValue(info->builder->getInt8PtrTy());
#endif
  }
  return ret;
}

static 
void codegenCallMemcpy(GenRet dest, GenRet src, GenRet size,
                       Type* eltType) {
  GenInfo *info = gGenInfo;

  // Must call with two real pointer arguments
  //  (and not any lvalues)
  INT_ASSERT(dest.isLVPtr == GEN_VAL);
  INT_ASSERT(src.isLVPtr == GEN_VAL);

  if( info->cfile ) {
    codegenCall("memcpy", dest, src, size);
  } else {
#ifdef HAVE_LLVM

    // Caller should use e.g. codegenAddrOf, this function does
    // not operate on lvalues.
    dest = codegenValue(dest);
    src = codegenValue(src);
    size = codegenValue(size);

    llvm::Type *int8Ty = llvm::Type::getInt8Ty(info->llvmContext);
    llvm::Type *types[3];
    unsigned addrSpaceDest = llvm::cast<llvm::PointerType>(dest.val->getType())->getAddressSpace();
    unsigned addrSpaceSrc = llvm::cast<llvm::PointerType>(src.val->getType())->getAddressSpace();
    types[0] = llvm::PointerType::get(int8Ty, addrSpaceDest);
    types[1] = llvm::PointerType::get(int8Ty, addrSpaceSrc);
    types[2] = llvm::Type::getInt64Ty(info->llvmContext);
    //types[3] = llvm::Type::getInt32Ty(info->llvmContext);
    //types[4] = llvm::Type::getInt1Ty(info->llvmContext);

    llvm::Function *func = llvm::Intrinsic::getDeclaration(info->module, llvm::Intrinsic::memcpy, types);
    //llvm::FunctionType *fnType = func->getFunctionType();
    llvm::Value* llArgs[5];

    llArgs[0] = convertValueToType(dest.val, types[0], false);
    llArgs[1] = convertValueToType(src.val, types[1], false);
    llArgs[2] = convertValueToType(size.val, types[2], false);

    // LLVM memcpy intrinsic has additional arguments alignment, isvolatile
    // alignment
    llArgs[3] = llvm::ConstantInt::get(llvm::Type::getInt32Ty(info->module->getContext()), 0, false);
    // isVolatile?
    llArgs[4] = llvm::ConstantInt::get(llvm::Type::getInt1Ty(info->module->getContext()), 0, false);

    // We can't use IRBuilder::CreateMemCpy because that adds
    //  a cast to i8 (in address space 0).
    llvm::CallInst* CI = info->builder->CreateCall(func, llArgs);

    llvm::MDNode* tbaaTag = NULL;
    llvm::MDNode* tbaaStructTag = NULL;
    if( eltType ) {
      tbaaTag = eltType->symbol->llvmTbaaNode;
      tbaaStructTag = eltType->symbol->llvmTbaaStructNode;
    }
    if( tbaaTag )
      CI->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaTag);
    if( tbaaStructTag )
      CI->setMetadata(llvm::LLVMContext::MD_tbaa_struct, tbaaStructTag);
#endif
  }
}
#ifdef HAVE_LLVM
static
GenRet codegenSizeof(llvm::Type* type)
{
  GenRet ret;
  ret.chplType = SIZE_TYPE;
  ret.val = codegenSizeofLLVM(type);
  return ret;
}
#endif

static
GenRet codegenSizeof(const char* name)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  ret.chplType = SIZE_TYPE;
  if( info->cfile ) {
    ret.c = "sizeof(";
    ret.c += name;
    ret.c += ')';
  } else {
#ifdef HAVE_LLVM
    ret.val = codegenSizeofLLVM(getTypeLLVM(name));
#endif
  }
  return ret;
}


static
GenRet codegenSizeof(Type* t)
{
  return codegenSizeof(t->symbol->cname);
}

// dest dest must have isLVPtr set. This function implements
// part of codegenAssign.
static
void codegenCopy(GenRet dest, GenRet src, Type* chplType=NULL)
{
  assert( dest.isLVPtr );

  if( ! chplType ) chplType = src.chplType;
  if( ! chplType ) chplType = dest.chplType;

  // This does handle isLVPtr being wide but only
  // if we're using fLLVMWideOpt.
  if( ! fLLVMWideOpt ) {
    assert( dest.isLVPtr != GEN_WIDE_PTR );
    assert( src.isLVPtr != GEN_WIDE_PTR );
  }

#ifdef HAVE_LLVM
  GenInfo *info = gGenInfo;
  if( ! info->cfile ) {
    bool useMemcpy = false;
    if( src.isLVPtr ) {
      // Consider using memcpy instead of stack allocating a possibly
      // large structure.

      llvm::Type* ptrTy = src.val->getType();
      llvm::Type* eltTy = ptrTy->getPointerElementType();

      if( isTypeSizeSmallerThan(info->targetData, eltTy, 
                                256 /* max bytes to load/store */ ) )  {
        // OK
      } else {
        useMemcpy = true;
      }
    }

    if( !useMemcpy ) {
      // Do it with store(load(src_ptr), dst_ptr)
      src = codegenValue(src);
      codegenStoreLLVM(src, dest);
      return;
    }
  }
#endif

  // Otherwise call memcpy.
  dest = codegenAddrOf(dest);
  if( ! src.isLVPtr ) src = codegenValuePtr(src);
  src = codegenAddrOf(src);
  GenRet size = codegenSizeof(chplType);
  codegenCallMemcpy(dest, src, size, chplType);
}

static bool
isTupleOfTuple(BaseAST *e) {
  return (e->typeInfo()->symbol->hasFlag(FLAG_STAR_TUPLE) &&
          toDefExpr(toClassType(e->typeInfo())->fields.head)->sym->
               type->symbol->hasFlag(FLAG_TUPLE));
}


// MPF - perhaps this should always call codegenValue
// and return isLVPtr = GEN_VAL.
GenRet codegenCast(Type* t, GenRet value, bool Cparens)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  ret.chplType = t;
  ret.isLVPtr = value.isLVPtr;
  
  // if we are casting a C99 wide pointer, parens around the value
  // will result in an error, hence the Cparens parameter
  // e.g. ((chpl____wide_DefaultRectangularArr_locale_1_int64_t_F)(
  //         { .locale = chpl_nodeID, .addr = nil }))
  // won't compile
 
  if (info->cfile){
    ret.c = "((" + t->codegen().c + ")";
    if (Cparens){
      ret.c += "(";
    }
    ret.c += value.c;
    if (Cparens){
      ret.c += ")";
    }
    ret.c += ")";
  } else {
#ifdef HAVE_LLVM
    llvm::Type* castType = t->codegen().type;

    ret.val = convertValueToType(value.val, castType, !value.isUnsigned);
    INT_ASSERT(ret.val);
#endif
  }
  return ret;
}


static
GenRet codegenCast(const char* typeName, GenRet value, bool Cparens)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  ret.isLVPtr = value.isLVPtr;
  ret.chplType = getNamedType(std::string(typeName));
     
  if( info->cfile ) {
    ret.c = "((";
    ret.c += typeName; 
    ret.c += ")";
    
    if (Cparens){
      ret.c += "(";
    }
    ret.c += value.c;
    if (Cparens){
      ret.c += ")";
    }
    ret.c += ")";

    /*ret.c = "((";
    ret.c += typeName;
    ret.c += ")(";
    ret.c += value.c;
    ret.c += "))";*/
  } else {
#ifdef HAVE_LLVM
    GenRet really = codegenValue(value);
    llvm::Type* castType = getTypeLLVM(typeName);
    ret.val = convertValueToType(really.val, castType, !really.isUnsigned);
    INT_ASSERT(ret.val);
#endif
  }
  return ret;
}

static
GenRet codegenCastToVoidStar(GenRet value)
{
  GenInfo* info = gGenInfo;
  GenRet ret;

  if( info->cfile ) {
    ret.c = "((void*)(";
    ret.c += value.c;
    ret.c += "))";
  } else {
#ifdef HAVE_LLVM
    llvm::Type* castType = info->builder->getInt8PtrTy();
    ret.val = convertValueToType(value.val, castType, !value.isUnsigned);
    INT_ASSERT(ret.val);
#endif
  }
  return ret;
}

static
GenRet codegenCastToCharStar(GenRet value)
{
  GenInfo* info = gGenInfo;
  GenRet ret;
  if( info->cfile ) {
    ret.c = "((char*)(";
    ret.c += value.c;
    ret.c += "))";
  } else {
#ifdef HAVE_LLVM
    llvm::Type* castType = info->builder->getInt8PtrTy(); 
    ret.val = convertValueToType(value.val, castType, !value.isUnsigned);
    INT_ASSERT(ret.val);
#endif
  }
  return ret;
}

// Generates code to perform an "assignment" operation, given
//  a destination pointer and a value.
// That's basically
//  (*)to_ptr = (*)from
// but for a homogeneous tuple, we will copy element-by-element
// or possibly call memcpy (in order to copy more that the first element).
// 
// If to_ptr or from_ptr is a wide reference type (but not both),
// we will generate a PUT or a GET.
//
// from_type is used (in C) to create a temporary in case that
// is needed.
// 
// This function will always copy some value. If that is not
// desired, other functions should be used.
//
// This function should have lvalues according to what should
// be copied; that is to_ptr should have isLVPtr set, and from
// can optionally have isLVPtr set.
static
void codegenAssign(GenRet to_ptr, GenRet from)
{
  GenInfo* info = gGenInfo;

  // To must be a pointer.
  INT_ASSERT(to_ptr.isLVPtr);

  // Both can't be wide
  INT_ASSERT(!(to_ptr.isLVPtr == GEN_WIDE_PTR && from.isLVPtr == GEN_WIDE_PTR));

  Type* type = from.chplType;
  if( ! type ) type = to_ptr.chplType;
  INT_ASSERT(type);

  bool isStarTuple = type->symbol->hasFlag(FLAG_STAR_TUPLE);
  int starTupleLength = 0;
  if( isStarTuple ) starTupleLength = toClassType(type)->fields.length;

  // if from is a wide ptr a ref to dtNil, set from to
  // a nil pointer of the correct type.
  if (from.chplType && to_ptr.chplType){
    ClassType* ct = toClassType(from.chplType);
    if (ct && ct->symbol->hasEitherFlag(FLAG_WIDE, FLAG_WIDE_CLASS)) {
      Symbol* valField = ct->getField("addr");
      if (valField && valField->getValType() == dtNil) {
         from = codegenAddrOf(
             codegenWideHere(codegenNullPointer(), to_ptr.chplType));
      }
    }
  }

  if( (to_ptr.isLVPtr != GEN_WIDE_PTR && from.isLVPtr != GEN_WIDE_PTR )) {
    // Neither is wide.
    if( isStarTuple ) {
      // Homogenous tuples are pointers even when GEN_VAL is set.
      // Homogeneous tuples are copied specially
      if ( !fNoTupleCopyOpt &&
           starTupleLength <= tuple_copy_limit &&
           !isTupleOfTuple(type) ) {
          // tuple copy optimization
          int i = 0;
          for_fields(field, toClassType(type)) {
            GenRet to_i =
              codegenElementPtr(to_ptr, new_IntSymbol(i, INT_SIZE_64));
            GenRet from_i =
              codegenElementPtr(from, new_IntSymbol(i, INT_SIZE_64));
            codegenAssign(to_i, from_i);
            i++;
          }
      } else {
        // tuple copy but use memcpy
        // to is already a pointer.
        codegenCopy(to_ptr, from, type);
      }
    } else {
      // not a homogeneous tuple copy
      if( info->cfile ) {
        std::string stmt = codegenValue(to_ptr).c + " = ";
        stmt += codegenValue(from).c;
        stmt += ";\n";
        info->cStatements.push_back(stmt);
      } else {
#ifdef HAVE_LLVM
        // LLVM codegen assignment (non-wide, non-tuple)
        GenRet value = codegenValue(from);
        llvm::Type* ptrValType =llvm::cast<llvm::PointerType>(
            to_ptr.val->getType())->getElementType();
        
        // implicit cast in C, needs to be made explicit in LLVM
        // e.g. T3 = alloca i8; 
        //      T3 = (T == T2);   // not actual LLVM syntax
        // in LLVM, boolean type is i1
        if (value.val->getType() != ptrValType){
          value.val =
            convertValueToType(value.val, ptrValType, !value.isUnsigned);
          INT_ASSERT(value.val);
        }

        codegenStoreLLVM(value, to_ptr, type);
#endif
      }
    }
  } else {
      // both should not be wide
      if (from.isLVPtr == GEN_WIDE_PTR && to_ptr.isLVPtr == GEN_WIDE_PTR){
        INT_FATAL("Cannot assign two wide pointers");
      }
    // One of the types is a wide pointer type, so we have to
    // call get or put.
    
    if (fLLVMWideOpt) {
      // LLVM pass will translate it to a get after some opts
      // We already know to is a pointer (wide or not).
      // Make sure that from is a pointer
      codegenCopy(to_ptr, from, type);
    } else if( from.isLVPtr == GEN_WIDE_PTR ) {
      INT_ASSERT(type);
      // would also be nice to call createTempVarWith to
      // store a temporary wide pointer so we don't get 
      // code like:
      //  chpl_comm_get(..., 
      //        ((wide type) {.locale = ..., .addr = ...}).locale, 
      //        ((wide type) {.locale = ..., .addr = ...}).addr, 
      //         ...);

      // Generate a GET
      if (type == wideStringType)
        // Special case for wide strings:
        // We perform a deep copy to obtain a char* that can be referred to locally.
        // Currently, the local character buffer is always leaked. :(
        codegenCall("chpl_gen_comm_wide_string_get",
                    codegenCastToVoidStar(to_ptr),
                    codegenRnode(from),
                    codegenRaddr(from),
                    codegenSizeof(type),
                    genTypeStructureIndex(type->symbol),
                    codegenOne(),
                    info->lineno, info->filename 
          );
      else
        codegenCall("chpl_gen_comm_get",
                    codegenCastToVoidStar(to_ptr),
                    codegenRnode(from),
                    codegenRaddr(from),
                    codegenSizeof(type),
                    genTypeStructureIndex(type->symbol),
                    codegenOne(),
                    info->lineno, info->filename 
          );
    } else {
      // Generate a PUT
      // to is already a pointer.
      codegenCall("chpl_gen_comm_put",
                   codegenCastToVoidStar(codegenValuePtr(from)),
                   codegenRnode(to_ptr),
                   codegenRaddr(to_ptr),
                   codegenSizeof(type),
                   genTypeStructureIndex(type->symbol),
                   codegenOne(),
                   info->lineno, info->filename
                  );
    }
  }
}


static GenRet 
codegenExprMinusOne(Expr* expr)
{
  int64_t i;
  IF1_int_type width = INT_SIZE_64;
  if( get_width(expr->typeInfo()) <= 8 ) width = INT_SIZE_8;
  else if( get_width(expr->typeInfo()) <= 16 ) width = INT_SIZE_16;
  else if( get_width(expr->typeInfo()) <= 32 ) width = INT_SIZE_32;

  if (get_int(expr, &i)) {
    return new_IntSymbol(i-1, width)->codegen();
  } else {
    return codegenSub(expr, new_IntSymbol(1, width));
  }
}

static void callExprHelper(CallExpr* call, BaseAST* arg) {
  if (!arg)
    return;
  if (toSymbol(arg) || toExpr(arg))
    call->insertAtTail(arg);
  else
    INT_FATAL(call, "Bad argList in CallExpr constructor");
}


CallExpr::CallExpr(BaseAST* base, BaseAST* arg1, BaseAST* arg2,
                   BaseAST* arg3, BaseAST* arg4) :
  Expr(E_CallExpr),
  baseExpr(NULL),
  argList(),
  primitive(NULL),
  partialTag(false),
  methodTag(false),
  square(false)
{
  if (Symbol* b = toSymbol(base)) {
    baseExpr = new SymExpr(b);
  } else if (Expr* b = toExpr(base)) {
    baseExpr = b;
  } else {
    INT_FATAL(this, "Bad baseExpr in CallExpr constructor");
  }
  callExprHelper(this, arg1);
  callExprHelper(this, arg2);
  callExprHelper(this, arg3);
  callExprHelper(this, arg4);
  argList.parent = this;
  gCallExprs.add(this);
}


CallExpr::CallExpr(PrimitiveOp *prim, BaseAST* arg1, BaseAST* arg2,
                   BaseAST* arg3, BaseAST* arg4) :
  Expr(E_CallExpr),
  baseExpr(NULL),
  argList(),
  primitive(prim),
  partialTag(false),
  methodTag(false),
  square(false)
{
  callExprHelper(this, arg1);
  callExprHelper(this, arg2);
  callExprHelper(this, arg3);
  callExprHelper(this, arg4);
  argList.parent = this;
  gCallExprs.add(this);
}

CallExpr::CallExpr(PrimitiveTag prim, BaseAST* arg1, BaseAST* arg2,
                   BaseAST* arg3, BaseAST* arg4) :
  Expr(E_CallExpr),
  baseExpr(NULL),
  argList(),
  primitive(primitives[prim]),
  partialTag(false),
  methodTag(false),
  square(false)
{
  callExprHelper(this, arg1);
  callExprHelper(this, arg2);
  callExprHelper(this, arg3);
  callExprHelper(this, arg4);
  argList.parent = this;
  gCallExprs.add(this);
}


CallExpr::CallExpr(const char* name, BaseAST* arg1, BaseAST* arg2,
                   BaseAST* arg3, BaseAST* arg4) :
  Expr(E_CallExpr),
  baseExpr(new UnresolvedSymExpr(name)),
  argList(),
  primitive(NULL),
  partialTag(false),
  methodTag(false),
  square(false)
{
  callExprHelper(this, arg1);
  callExprHelper(this, arg2);
  callExprHelper(this, arg3);
  callExprHelper(this, arg4);
  argList.parent = this;
  gCallExprs.add(this);
}


CallExpr::~CallExpr() { }


void CallExpr::verify() {
  Expr::verify();
  if (astTag != E_CallExpr) {
    INT_FATAL(this, "Bad CallExpr::astTag");
  }
  if (argList.parent != this)
    INT_FATAL(this, "Bad AList::parent in CallExpr");
  if (baseExpr && baseExpr->parentExpr != this)
    INT_FATAL(this, "Bad baseExpr::parent in CallExpr");
  if (normalized && isPrimitive(PRIM_RETURN)) {
    FnSymbol* fn = toFnSymbol(parentSymbol);
    SymExpr* sym = toSymExpr(get(1));
    if (!fn)
      INT_FATAL(this, "Return is not in a function.");
    if (fn->body->body.last() != this)
      INT_FATAL(this, "Return is in middle of function.");
    if (!sym)
      INT_FATAL(this, "Return does not return a symbol.");
  }
  for_actuals(actual, this) {
    if (actual->parentExpr != this)
      INT_FATAL(this, "Bad CallExpr::argList::parentExpr");
  }
  if (resolved) {
    if (FnSymbol* fn = isResolved()) {
      if (!fn->hasFlag(FLAG_EXTERN)) {
        for_formals_actuals(formal, actual, this) {
          if (formal->type != actual->typeInfo() && actual->typeInfo() != dtNil)
            INT_FATAL(this, "actual formal type mismatch");
        }
      }
    }
  }
  if (primitive) {
    if (!(PRIM_UNKNOWN <= primitive->tag && primitive->tag < NUM_KNOWN_PRIMS))
      INT_FATAL(this, "invalid primitive->tag");
    switch (primitive->tag) {
    case(PRIM_BLOCK_PARAM_LOOP):
    case(PRIM_BLOCK_WHILEDO_LOOP):
    case(PRIM_BLOCK_DOWHILE_LOOP):
    case(PRIM_BLOCK_FOR_LOOP):
    case(PRIM_BLOCK_BEGIN):
    case(PRIM_BLOCK_COBEGIN):
    case(PRIM_BLOCK_COFORALL):
    case(PRIM_BLOCK_XMT_PRAGMA_FORALL_I_IN_N):
    case(PRIM_BLOCK_XMT_PRAGMA_NOALIAS):
    case(PRIM_BLOCK_ON):
    case(PRIM_BLOCK_ON_NB):
    case(PRIM_BLOCK_LOCAL):
      if (toBlockStmt(parentExpr)) {
        // does not pass:
        //if (toBlockStmt(parentExpr)->blockInfo != this)
        //  INT_FATAL(this, "blockInfo-type CallExpr not parent's blockInfo");
      } else {
        INT_FATAL(this, "blockInfo-type CallExpr not in a BlockStmt");
      }
      break;
    case(PRIM_BLOCK_UNLOCAL):
      INT_FATAL("PRIM_BLOCK_UNLOCAL between passes");
      break;
    default:
      break; // do nothing
    }
  }
}


CallExpr*
CallExpr::copyInner(SymbolMap* map) {
  CallExpr *_this = 0;
  if (primitive)
    _this = new CallExpr(primitive);
  else
    _this = new CallExpr(COPY_INT(baseExpr));
  for_actuals(expr, this)
    _this->insertAtTail(COPY_INT(expr));
  _this->primitive = primitive;
  _this->partialTag = partialTag;
  _this->methodTag = methodTag;
  _this->square = square;
  return _this;
}


void CallExpr::replaceChild(Expr* old_ast, Expr* new_ast) {
  if (old_ast == baseExpr) {
    baseExpr = new_ast;
  } else {
    INT_FATAL(this, "Unexpected case in CallExpr::replaceChild");
  }
}


void
CallExpr::insertAtHead(BaseAST* ast) {
  if (Symbol* a = toSymbol(ast))
    argList.insertAtHead(new SymExpr(a));
  else
    argList.insertAtHead(toExpr(ast));
}


void
CallExpr::insertAtTail(BaseAST* ast) {
  if (Symbol* a = toSymbol(ast))
    argList.insertAtTail(new SymExpr(a));
  else
    argList.insertAtTail(toExpr(ast));
}


FnSymbol* CallExpr::isResolved(void) {
  SymExpr* base = toSymExpr(baseExpr);
  return base ? toFnSymbol(base->var) : NULL;
}


bool CallExpr::isNamed(const char* name) {
  if (SymExpr* base = toSymExpr(baseExpr))
    if (!strcmp(base->var->name, name))
      return true;
  if (UnresolvedSymExpr* base = toUnresolvedSymExpr(baseExpr))
    if (!strcmp(base->unresolved, name))
      return true;
  return false;
}


int CallExpr::numActuals() {
  return argList.length;
}


Expr* CallExpr::get(int index) {
  return argList.get(index);
}


FnSymbol* CallExpr::findFnSymbol(void) {
  FnSymbol* fn = NULL;
  if (SymExpr* variable = toSymExpr(baseExpr))
    fn = toFnSymbol(variable->var);
  if (!fn)
    INT_FATAL(this, "Cannot find FnSymbol in CallExpr");
  return fn;
}


Type* CallExpr::typeInfo(void) {
  if (primitive)
    return primitive->returnInfo(this);
  else if (isResolved())
    return isResolved()->retType;
  else
    return dtUnknown;
}

void CallExpr::prettyPrint(std::ofstream *o) {
  if (isResolved()) {
    if (isResolved()->hasFlag(FLAG_BEGIN_BLOCK))
      *o << "begin";
    else if (isResolved()->hasFlag(FLAG_ON_BLOCK))
      *o << "on";
  }
  bool array = false;
  bool unusual = false;
  if (baseExpr != NULL) {
    if (UnresolvedSymExpr *expr = toUnresolvedSymExpr(baseExpr)) {
      if (strcmp(expr->unresolved, "*") == 0){
        unusual = true;
        argList.first()->prettyPrint(o);
        *o << "*(";
        argList.last()->prettyPrint(o);
        *o << ")";
      } else if (strcmp(expr->unresolved, 
                        "_build_range") == 0) {
        argList.first()->prettyPrint(o);
        *o << "..";
        argList.last()->prettyPrint(o);
        unusual = true;
      } else if (strcmp(expr->unresolved,
                        "chpl__buildDomainExpr") == 0) {
        unusual = true;
        for_alist(expr, argList) {
          if (expr != argList.first()) {
            *o << ", ";
          }
          expr->prettyPrint(o);
        }
      } else if (strcmp(expr->unresolved, "chpl__buildArrayRuntimeType") == 0) {
        *o << "[";
        array = true;
      } else if (strcmp(expr->unresolved, 
                        "chpl__buildDomainRuntimeType") == 0) {
        *o << "domain(";
        argList.last()->prettyPrint(o);
        *o << ")";
        unusual = true;
      } else if (strcmp(expr->unresolved,
                        "_build_tuple") != 0) {
        baseExpr->prettyPrint(o);
      }
    } else {
      baseExpr->prettyPrint(o);
    }
  } else if (primitive != NULL) {
    if (primitive->tag == PRIM_INIT) {
      unusual = true;
      argList.head->prettyPrint(o);
    }
  }

  if (!array && !unusual)
    *o << "(";
  if (!unusual) {
    for_alist(expr, argList) {
      if (expr != argList.first()) {
        if (array && expr == argList.last()) {
          *o << "] ";
        } else {
          *o << ", "; 
        }     
      }
      expr->prettyPrint(o);         
    }
    if (array && argList.first() == argList.last())
      *o << "]"; 
  }
  if (!array && !unusual) {
    *o << ")";
  }
}

/* Notes about code generation:
 *  Intermediate expressions are returned from Expr::codegen
 *  Local variables, array elements, tuple elements, and fields
 *    are always generated as the address of that value (ie lvalues
 *    are pointers)
 *  Expressions may be actual values, not addresses
 *  Chapel includes class instances - which may be remote or local.
 *   Note that the variable in question for a class instance *is*
 *    a wide (or not) reference (to the allocated object), but these
 *    references are considered "values" rather than "lvalue pointers"
 *    by the code generator. Thus a "reference to CLASS_CLASS" is
 *    actually a reference to a reference.. Note also that an "ARRAY"
 *    in the code generator is actually an instance of the e.g. _ddata
 *    class (and so the pointer to the data is again treated as a value).
 *    Lastly, Chapel references are considered "values" rather
 *    than "lvalue pointers", similarly to class instances, so that
 *    the generated code can set what a reference is pointing to
 *    (even though that is not allowed in Chapel).
 */
GenRet CallExpr::codegen() {
  GenRet ret;
  GenInfo* info = gGenInfo;
  FILE* c = info->cfile;
  SET_LINENO(this);

  // Note (for debugging), function name is in parentSymbol->cname.

  if (getStmtExpr() && getStmtExpr() == this)
    codegenStmt(this);

  if (primitive) {
    switch (primitive->tag) {
    case PRIM_UNKNOWN:
      ret = codegenBasicPrimitiveExpr(this);
      break;
    case PRIM_ARRAY_SET:
    case PRIM_ARRAY_SET_FIRST:
      {
        // get(1): (wide?) base pointer
        // get(2): index
        // get(3): value
        // get(4): src-line
        // get(5): src-file

        // Used to handle FLAG_WIDE_CLASS/FLAG_STAR_TUPLE specially,
        // but these should be taken care of by codegenElementPtr and
        // codegenAssign now.
        GenRet elementPtr = codegenElementPtr(get(1), get(2));
        codegenAssign(elementPtr, get(3));
        break;
      }
    case PRIM_ARRAY_ALLOC:
    {
      GenRet dst = get(1);
      GenRet alloced;
      INT_ASSERT(dst.isLVPtr);
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        Type* eltType =
          getDataClassType(
              get(1)->typeInfo()->getField("addr")->type->symbol)->typeInfo();
        GenRet locale = codegenRlocale(dst);
        GenRet call = codegenCallExpr("chpl_wide_array_alloc",
                                      codegenRnode(dst),
                                      codegenValue(get(3)),
                                      codegenSizeof(eltType),
                                      get(4), get(5));
        call.chplType = get(1)->typeInfo(); 
        alloced = codegenAddrOf(codegenWideAddr(locale, call, call.chplType)); 
      } else {
        Type* eltType =
          getDataClassType(get(1)->typeInfo()->symbol)->typeInfo();
        alloced = codegenCallExpr("chpl_array_alloc", codegenValue(get(3)),
            codegenSizeof(eltType), get(4), get(5));
      }
      codegenAssign(dst, alloced);
      break;
    }
    case PRIM_ARRAY_FREE:
    {
      if (fNoMemoryFrees)
        break;
      GenRet data = get(1);
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        GenRet node = codegenRnode(data);
        GenRet ptr = codegenRaddr(data);
        codegenCall("chpl_wide_array_free", node, ptr, get(2), get(3));
      } else {
        codegenCall("chpl_array_free", data, get(2), get(3));
      }
      break;
    }
    case PRIM_ARRAY_FREE_ELTS:
    {
      if (fNoMemoryFrees)
        break;
      // This used to run a macro like this:
      // for(i = 0; i < (x)->size; i++) call
      INT_FATAL("PRIM_ARRAY_FREE_ELTS");
      break;
    }
    case PRIM_NOOP:
      break;
    case PRIM_MOVE:
    {
      if (get(1)->typeInfo() == dtVoid) {
        ret = get(2)->codegen();
        break;
      }
      if (CallExpr* call = toCallExpr(get(2))) {
       if (call->primitive)
       {
        // Assume that the RHS is a primitive we wish to handle specially
        // until proven otherwise.
        // If the RHS is not handled specially, we fall through and handle it
        // generally.
        bool handled = true;
        switch (call->primitive->tag)
        {
         case PRIM_WIDE_GET_LOCALE:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            if (call->get(1)->getValType()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
              // TODO: Is the remote get really necessary?
              // i.e. will the localeID field of the remotely fetched pointer
              // ever differ from that in our local copy of the pointer?
              // I hope not.  Otherwise my understanding of a wide pointer
              // is "incomplete". <hilde>
              
              // To implement the test, replace the result of a get with a
              // test that codegenRlocale(call->get(1)) == the current result,
              // and if false
              // print a runtime error.  If the error code never fires, we can
              // just codegen the "else" form, i.e. :
              //   codegenAssign(get(1), codegenRlocale(call->get(1)));

              // get locale field of wide class via wide reference

              // We have a wide reference to a wide class; we
              // want to load the wide class pointer and return its locale.
              //

              GenRet classPtr = codegenValue(codegenDeref(call->get(1)));
              codegenAssign(get(1), codegenRlocale(classPtr));
            } else {
              codegenAssign(get(1), codegenRlocale(call->get(1)));
            }
          } else if(call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)){
            codegenAssign(get(1), codegenRlocale(call->get(1)));
          } else {
            // Needs to assign both fields.
            codegenAssign(get(1), codegenGetLocaleID());
          }
          break;
         }
         case PRIM_WIDE_GET_NODE:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            if (call->get(1)->getValType()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
              // get locale field of wide class via wide reference
              GenRet classPtr = codegenValue(codegenDeref(call->get(1)));
              codegenAssign(get(1), codegenRnode(classPtr));
            } else {
              codegenAssign(get(1), codegenRnode(call->get(1)) );
            }
          } else if(call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)){
            codegenAssign(get(1), codegenRnode(call->get(1)) );
          } else {
            codegenAssign(get(1), codegenGetNodeID());
          }
          break;
         }
         case PRIM_WIDE_GET_SUBLOC:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            if (call->get(1)->getValType()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
              // get locale field of wide class via wide reference
              GenRet classPtr = codegenValue(codegenDeref(call->get(1)));
              codegenAssign(get(1), codegenRsubloc(classPtr));
            } else {
              codegenAssign(get(1), codegenRsubloc(call->get(1)) );
            }
          } else if(call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)){
            codegenAssign(get(1), codegenRsubloc(call->get(1)) );
          } else {
            codegenAssign(get(1), codegenGetSublocID());
          }
          break;
         }
         case PRIM_DEREF:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE) ||
              call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            Type* valueType;
            if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE))
              valueType = call->get(1)->getValType();
            else
              valueType = call->get(1)->typeInfo()->getField("addr")->type;
            INT_ASSERT(valueType == get(1)->typeInfo());
            if (valueType == dtString) {
              codegenCall(
                  "chpl_comm_wide_get_string",
                  codegenLocalAddrOf(get(1)),
                  codegenLocalAddrOf(call->get(1)), 
                  genTypeStructureIndex(valueType->symbol),
                  call->get(2),
                  call->get(3));
            } else {
              // set get(1) = *(call->get(1));
              codegenAssign(get(1),codegenDeref(call->get(1)));
            }
          } else if (get(1)->typeInfo()->symbol->hasFlag(FLAG_STAR_TUPLE) ||
                     get(1)->typeInfo()->symbol->hasFlag(FLAG_FIXED_STRING)) {
            // star tuple handled in codegenAssign
            // set get(1) = *(call->get(1));
            codegenAssign(get(1),codegenDeref(call->get(1)));
          } else if (call->get(1)->typeInfo() == dtString) { 
            // this should be illegal when wide strings are fixed
            // set get(1) = call->get(1);
            codegenAssign(get(1),call->get(1));
          } else {
            // set get(1) = *(call->get(1));
            codegenAssign(get(1),codegenDeref(call->get(1)));
          }
          break;
         }
         case PRIM_GET_MEMBER_VALUE:
         {
          SymExpr* se = toSymExpr(call->get(2));
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            INT_ASSERT(se);
            if (se->var->hasFlag(FLAG_SUPER_CLASS)) {
              // We're getting the super class pointer.
              GenRet srcwide = call->get(1);
              GenRet addr = codegenCast(
                              get(1)->typeInfo()->getField("addr")->type,
                              codegenRaddr(srcwide));
              GenRet ref =
                codegenAddrOf(codegenWideAddrWithAddr(srcwide, addr));
              codegenAssign(get(1), ref);
            } else {
              codegenAssign(get(1), codegenFieldPtr(call->get(1), se));
            }
          } else if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            INT_ASSERT(se);
            // codegenAssign will dereference.
            codegenAssign(get(1), codegenFieldPtr(call->get(1), se));
          } else if (get(2)->typeInfo()->symbol->hasFlag(FLAG_STAR_TUPLE)) {
            INT_ASSERT(se);
            // codegenAssign will handle star tuple
            codegenAssign(get(1), codegenFieldPtr(call->get(1), se));
          } else {
            INT_ASSERT(se);
            if (se->var->hasFlag(FLAG_SUPER_CLASS)) {
              // We're getting the super class pointer.
              GenRet ref = codegenFieldPtr(call->get(1), se);
              // Now we have a field pointer to object->super, but
              // the pointer to super *is* actually the value of
              // the super class. So we just set isPtr to Value.
              ref.isLVPtr = GEN_VAL; 
              codegenAssign(get(1), ref);
            } else {
              codegenAssign(get(1), codegenFieldPtr(call->get(1), se));
            }
          }
          break;
         }
         case PRIM_GET_MEMBER:
         {
          /* Get a pointer to a member */
          SymExpr* se = toSymExpr(call->get(2));
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) ||
              call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE) ||
              get(2)->typeInfo()->symbol->hasFlag(FLAG_STAR_TUPLE))
          {
            codegenAssign(
                get(1), codegenAddrOf(codegenFieldPtr(call->get(1), se))); 
          }
          else
            handled = false;
          break;
         }
         case PRIM_GET_SVEC_MEMBER:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            /* Get a pointer to the i'th element of a homogenous tuple */
            GenRet elemPtr =
              codegenElementPtr(call->get(1),codegenExprMinusOne(call->get(2)));
            INT_ASSERT( elemPtr.isLVPtr == GEN_WIDE_PTR );
            elemPtr = codegenAddrOf(elemPtr);
            codegenAssign(get(1), elemPtr);
          }
          else
            handled = false;
          break;
         }
         case PRIM_GET_SVEC_MEMBER_VALUE:
         {
          /* Get the i'th value from a homogeneous tuple */
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            // codegenElementPtr/codegenAssign handle wide pointers
            codegenAssign(
                get(1),
                codegenElementPtr(call->get(1),
                                  codegenExprMinusOne(call->get(2))));
          } else {
            codegenAssign(
                get(1),
                codegenElementPtr(call->get(1),
                                  codegenExprMinusOne(call->get(2))));
          }
          break;
         }
         case PRIM_ARRAY_GET:
         {
          /* Get a pointer to the i'th array element */
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            codegenAssign(get(1),
                codegenAddrOf(codegenElementPtr(call->get(1), call->get(2))));
          } else {
            codegenAssign(
                get(1),
                codegenAddrOf(codegenElementPtr(call->get(1), call->get(2))));
          }
          break;
         }
         case PRIM_ARRAY_GET_VALUE:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            codegenAssign(get(1),
                          codegenElementPtr(call->get(1), call->get(2)));
          } else {
            codegenAssign(get(1),
                          codegenElementPtr(call->get(1), call->get(2)));
          }
          break;
         }
         case PRIM_GET_UNION_ID:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            codegenAssign(get(1), codegenFieldUidPtr(call->get(1)));
          }
          else
            handled = false;
          break;
         }
         case PRIM_TESTCID:
         {
          // set get(1) to
          //   call->get(1)->chpl_class_id == chpl__cid_"call->get(2)"
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            GenRet tmp = codegenFieldCidPtr(call->get(1));
            codegenAssign(
                get(1),
                codegenEquals(tmp, codegenUseCid(call->get(2)->typeInfo())));
          }
          else
            handled = false;
          break;
         }
         case PRIM_GETCID:
         {
          if (call->get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            GenRet tmp = codegenFieldCidPtr(call->get(1));
            codegenAssign(get(1), tmp);
          }
          else
            handled = false;
          break;
         }
         case PRIM_CAST:
         {
          if (call->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) ||
              call->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
            GenRet tmp = call->get(2);
            tmp = codegenWideAddrWithAddr(tmp,
                                  codegenCast(call->get(1)->typeInfo(), 
                                              codegenRaddr(tmp)));
            codegenAssign(get(1), codegenAddrOf(tmp));
          }
          else
            handled = false;
          break;
         }
         case PRIM_DYNAMIC_CAST:
         {
          if (call->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            Type* type = call->typeInfo()->getField("addr")->type;
            GenRet wideFrom = codegenValue(call->get(2));
            GenRet wideFromAddr = codegenRaddr(wideFrom);
            GenRet value = codegenValue(codegenFieldCidPtr(wideFrom));
            GenRet ok = codegenDynamicCastCheck(value, type);
            // if ok, (typeInfo()->symbol->cname) (value)
            // otherwise, NULL
            GenRet cast = codegenCast(type, wideFromAddr);
            GenRet nul = codegenCast(type, codegenNullPointer());
            GenRet addr = codegenTernary(ok, cast, nul);
            GenRet wide = codegenAddrOf(
                    codegenWideAddrWithAddr(wideFrom, addr, call->typeInfo()));
            codegenAssign(get(1), wide);
          }
          else
            handled = false;
          break;
         }
         case PRIM_GET_PRIV_CLASS:
         {
          GenRet r = codegenCallExpr("chpl_getPrivatizedClass", call->get(2));
          if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
            r = codegenAddrOf(codegenWideHere(r, get(1)->typeInfo()));
          }
          codegenAssign(get(1), r);
          break;
         }
         default:
          // OK, we did not handle the RHS as a special case.
          handled = false;
          break;
        }
        // If the RHS was handled as a special case above, the entire PRIM_MOVE has
        // been codegenned, so we can skip the general cases appearing below.
        if (handled)
          break;
       }
      } // End of special-case handling for primitives in the RHS of MOVE.

      // Handle general cases of PRIM_MOVE.
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          !get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        if (get(2)->typeInfo() != dtString)
          codegenAssign(get(1), codegenAddrOf(codegenWideHere(get(2)))); 
        else
          codegenCall("chpl_string_widen", codegenAddrOf(get(1)), get(2)); 
        break;
      }
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE) &&
          get(2)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
        codegenAssign(get(1), codegenAddrOf(codegenWideHere(get(2))));
        break;
      }
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE) &&
          !get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE) &&
          !get(2)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
        GenRet to_ptr = codegenDeref(get(1));
        codegenAssign(to_ptr, get(2));
        break;
      }
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_REF) &&
          get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        // get(1) = Raddr(get(2));
        codegenAssign(get(1), codegenRaddr(get(2))); 
        break;
      }
      if (!get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          !get(1)->typeInfo()->symbol->hasFlag(FLAG_REF) &&
          get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        // get(1) = Raddr(get(2));
        codegenAssign(get(1), codegenRaddr(get(2))); 
        break;
      }
      if ((get(2)->typeInfo()->symbol->hasFlag(FLAG_STAR_TUPLE)) ||
          (get(2)->typeInfo()->symbol->hasFlag(FLAG_FIXED_STRING))) {
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          // codegenAssign handles tuple copy opt
          codegenAssign(codegenDeref(get(1)), get(2));
        } else {
          // codegenAssign handles tuple copy opt
          codegenAssign(get(1), get(2)); 
        }
        break;
      }
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_REF) &&
          !get(2)->typeInfo()->symbol->hasFlag(FLAG_REF))
        codegenAssign(codegenDeref(get(1)), get(2));
      else
        codegenAssign(get(1), get(2));
      break;
    } // End of PRIM_MOVE

    // We list the special cases handled in the PRIM_MOVE switch above, so we don't
    // trigger the "should these still be in the AST?" error.
    case PRIM_DEREF:
    case PRIM_GET_SVEC_MEMBER_VALUE:
    case PRIM_GET_MEMBER_VALUE:
    case PRIM_WIDE_GET_LOCALE:
    case PRIM_WIDE_GET_NODE:
    case PRIM_WIDE_GET_SUBLOC:
    case PRIM_GET_PRIV_CLASS:
    case PRIM_ARRAY_GET:
    case PRIM_ARRAY_GET_VALUE:
      // generated during generation of PRIM_MOVE
      break;

    case PRIM_ADDR_OF:
    {
      ret = codegenAddrOf(get(1));
      break;
    }
    case PRIM_REF_TO_STRING:
    {
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE) ||
          get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        GenRet wide = get(1);
        ret = codegenCallExpr("chpl_wideRefToString",
                              codegenRnode(wide), codegenRaddr(wide));
      } else {
        ret = codegenCallExpr("chpl_refToString", get(1));
      }
      break;
    }
    case PRIM_RETURN:
    {
      if (typeInfo() == dtVoid) {
        if( c ) ret.c = "return";
        else {
#ifdef HAVE_LLVM
          ret.val = info->builder->CreateRetVoid();
#endif
        }
      } else {
        ret = codegenValue(get(1));
        if( c ) ret.c = "return " + ret.c;
        else {
#ifdef HAVE_LLVM
          ret.val = info->builder->CreateRet(ret.val);
#endif
        }
      }
      break;
    }
    case PRIM_UNARY_MINUS:
      ret = codegenNeg(get(1));
      break;
    case PRIM_UNARY_PLUS:
      {
        GenRet tmp = codegenValue(get(1));
        if( c ) ret.c = "(+ " + tmp.c + ")";
        else ret = tmp; // nothing is necessary.
        break;
      }
    case PRIM_UNARY_NOT:
      {
        GenRet tmp = codegenValue(get(1));
        if( c ) ret.c = "(~ " + tmp.c + ")";
        else {
#ifdef HAVE_LLVM
          ret.val = info->builder->CreateNot(tmp.val);
#endif
        }
      }
      break;
    case PRIM_UNARY_LNOT:
      ret = codegenIsZero(get(1));
      break;
    case PRIM_ADD:
      ret = codegenAdd(get(1), get(2));
      break;
    case PRIM_SUBTRACT:
      ret = codegenSub(get(1), get(2));
      break;
    case PRIM_MULT:
      ret = codegenMul(get(1), get(2));
      break;
    case PRIM_DIV:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " / " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFDiv(values.a, values.b);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateUDiv(values.a, values.b);
            } else {
              ret.val = info->builder->CreateSDiv(values.a, values.b);
            }
          }
#endif
        }
      }
      break;
    case PRIM_MOD:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " % " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFRem(a.val, b.val);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateURem(a.val, b.val);
            } else {
              ret.val = info->builder->CreateSRem(a.val, b.val);
            }
          }
#endif
        }
      }
      break;
    case PRIM_LSH:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " << " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          ret.val = info->builder->CreateShl(
              a.val,
              convertValueToType(b.val, a.val->getType(),
                                 is_signed(get(2)->typeInfo())));
#endif
        }
      }
      break;
    case PRIM_RSH:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " >> " + b.c + ")";
        else {

#ifdef HAVE_LLVM
          if(!is_signed(get(1)->typeInfo())) {
            ret.val = info->builder->CreateLShr(
                a.val,
                convertValueToType(b.val, a.val->getType(),
                                   is_signed(get(2)->typeInfo())));
          } else {
            ret.val = info->builder->CreateAShr(
                a.val,
                convertValueToType(b.val, a.val->getType(),
                                   is_signed(get(2)->typeInfo())));
          }
#endif
        }
      }
      break;
    case PRIM_PTR_EQUAL:
    case PRIM_EQUAL:
     // TODO: Need subloc field as well.
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        GenRet a = get(1);
        GenRet b = get(2);
        GenRet addrNe = codegenNotEquals(codegenRaddr(a), codegenRaddr(b));
        GenRet locNe = codegenNotEquals(codegenRnode(a), codegenRnode(b));
        GenRet rh = codegenLogicalAnd(codegenIsNotZero(codegenRaddr(a)), locNe);
        GenRet ne = codegenLogicalOr(addrNe, rh);
        ret = codegenIsZero(ne);
      } else if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          get(2)->typeInfo() == dtNil) {
        ret = codegenIsZero(get(1));
      } else if (get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
                 get(1)->typeInfo() == dtNil) {
        ret = codegenIsZero(get(2));
      } else {
        ret = codegenEquals(get(1), get(2));
      }
      break;
    case PRIM_PTR_NOTEQUAL:
    case PRIM_NOTEQUAL:
     // TODO: Need subloc field as well.
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        GenRet a = get(1);
        GenRet b = get(2);
        GenRet addrNe = codegenNotEquals(codegenRaddr(a), codegenRaddr(b));
        GenRet locNe = codegenNotEquals(codegenRnode(a), codegenRnode(b));
        GenRet rh = codegenLogicalAnd(codegenIsNotZero(codegenRaddr(a)), locNe);
        ret = codegenLogicalOr(addrNe, rh);
      } else if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          get(2)->typeInfo() == dtNil) {
        ret = codegenIsNotZero(get(1));
      } else if (get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
                 get(1)->typeInfo() == dtNil) {
        ret = codegenIsNotZero(get(2));
      } else {
        ret = codegenNotEquals(get(1), get(2));
      }
      break;
    case PRIM_LESSOREQUAL:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " <= " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFCmpOLE(values.a, values.b);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateICmpULE(values.a, values.b);
            } else {
              ret.val = info->builder->CreateICmpSLE(values.a, values.b);
            }
          }
#endif
        }
      }
      break;
    case PRIM_GREATEROREQUAL:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " >= " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFCmpOGE(values.a, values.b);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateICmpUGE(values.a, values.b);
            } else {
              ret.val = info->builder->CreateICmpSGE(values.a, values.b);
            }
          }
#endif
        }
      }
      break;
    case PRIM_LESS:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " < " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFCmpOLT(values.a, values.b);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateICmpULT(values.a, values.b);
            } else {
              ret.val = info->builder->CreateICmpSLT(values.a, values.b);
            }
          }
#endif
        }
      }
      break;
    case PRIM_GREATER:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " > " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          if(values.a->getType()->isFPOrFPVectorTy()) {
            ret.val = info->builder->CreateFCmpOGT(values.a, values.b);
          } else {
            if(!values.isSigned) {
              ret.val = info->builder->CreateICmpUGT(values.a, values.b);
            } else {
              ret.val = info->builder->CreateICmpSGT(values.a, values.b);
            }
          }
#endif
        }
      }
      break;
    case PRIM_AND:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " & " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          ret.val = info->builder->CreateAnd(values.a, values.b);
#endif
        }
      }
      break;
    case PRIM_OR:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " | " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          ret.val = info->builder->CreateOr(values.a, values.b);
#endif
        }
      }
      break;
    case PRIM_XOR:
      {
        GenRet a = codegenValue(get(1));
        GenRet b = codegenValue(get(2));
        if( c ) ret.c = "(" + a.c + " ^ " + b.c + ")";
        else {
#ifdef HAVE_LLVM
          PromotedPair values =
            convertValuesToLarger(
                a.val, b.val,
                is_signed(get(1)->typeInfo()), is_signed(get(2)->typeInfo()));
          ret.val = info->builder->CreateXor(values.a, values.b);
#endif
        }
      }
      break;
    case PRIM_POW:
      ret = codegenCallExpr("pow", get(1), get(2));
      break;
    case PRIM_MIN: 
      {
        Type *t = get(1)->typeInfo();
        if (is_arithmetic_type( t)) {
          if (is_int_type( t)) {
            ret = codegenUseGlobal("MIN_INT" + numToString(get_width(t)));
          } else if (is_uint_type( t)) {
            ret = codegenUseGlobal("MIN_UINT" + numToString(get_width(t)));
          } else if (is_real_type( t)) {
            ret = codegenNeg(
                    codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t))));
          } else if (is_imag_type( t)) {
            ret = codegenNeg(
                    codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t))));
          } else if (is_complex_type( t)) {
            std::string fname = "_chpl_complex" + numToString(get_width(t));
            ret = codegenCallExpr(
                fname.c_str(),
                codegenNeg(
                  codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)/2))),
                codegenNeg(
                  codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)/2))));
          } else {
            INT_FATAL( t, "cannot do min on supplied type");
          }
        } else {
          INT_FATAL( t, "not arithmetic type");
        }
        break;
      }
    case PRIM_MAX: 
      {
        Type *t = get(1)->typeInfo();
        if (is_arithmetic_type( t)) {
          if (is_int_type( t)) {
            ret = codegenUseGlobal("MAX_INT" + numToString(get_width(t)));
          } else if (is_uint_type( t)) {
            ret = codegenUseGlobal("MAX_UINT" + numToString(get_width(t)));
          } else if (is_real_type( t)) {
            ret = codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)));
          } else if (is_imag_type( t)) {
            ret = codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)));
          } else if (is_complex_type( t)) {
            std::string fname = "_chpl_complex" + numToString(get_width(t));
            ret = codegenCallExpr(fname.c_str(),
                codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)/2)),
                codegenUseGlobal("MAX_FLOAT" + numToString(get_width(t)/2)));
          } else {
            INT_FATAL( t, "cannot do max on supplied type");
          }
        } else {
          INT_FATAL( t, "not arithmetic type");
        }
        break;
      }
    case PRIM_SETCID:
    {
      // get(1) is the object 
      // (type=chpl__class_id,
      //  tid=CHPL_TYPE_int32_t,
      //  wide=get(1),
      //  local=chpl__cid_<type>,
      //  stype=dtObject->typeInfo(),
      //  sfield=chpl__cid,
      //  ln=get(2),
      //  fn=get(3))
      //
      if(get(1)->typeInfo()->symbol->hasFlag(FLAG_NO_OBJECT) &&
         !get(1)->typeInfo()->symbol->hasFlag(FLAG_OBJECT_CLASS)) {
        // Don't set cid for an extern class.
        // This should probably be an error in the future.
      } else {
        Type* classType;
        if(get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
          classType = get(1)->typeInfo()->getField("addr")->type;
        } else {
          classType = get(1)->typeInfo();
        }
        GenRet ref = codegenFieldCidPtr(get(1));

        codegenAssign(ref, codegenUseCid(classType));
      }
      break;
    }
    case PRIM_GETCID:
    {
      INT_ASSERT(get(1)->typeInfo() != dtNil);
      if(get(1)->typeInfo()->symbol->hasFlag(FLAG_NO_OBJECT) &&
         !get(1)->typeInfo()->symbol->hasFlag(FLAG_OBJECT_CLASS)) {
        INT_ASSERT(0);
      }
      GenRet ref = codegenFieldCidPtr(get(1));
      ret = codegenValue(ref);
      break;
    }
    case PRIM_TESTCID:
    {
      // get(1) is an object to test, get(2) we just use the type of it.
      INT_ASSERT(get(1)->typeInfo() != dtNil);
      if(get(1)->typeInfo()->symbol->hasFlag(FLAG_NO_OBJECT) &&
         !get(1)->typeInfo()->symbol->hasFlag(FLAG_OBJECT_CLASS)) {
        INT_ASSERT(0);
      }
      GenRet ref = codegenFieldCidPtr(get(1));
      ret = codegenEquals(ref, codegenUseCid(get(2)->typeInfo()));
      break;
    }
    case PRIM_SET_UNION_ID:
    {
      // get(1)->_uid = get(2)
      GenRet ref = codegenFieldUidPtr(get(1));
      codegenAssign(ref, get(2));
      break;
    }
    case PRIM_GET_UNION_ID:
    {
      // returns uid from get(1)
      GenRet ref = codegenFieldUidPtr(get(1));
      ret = codegenValue(ref);
      break;
    }
    case PRIM_SET_SVEC_MEMBER: {
      // set tuple base=get(1) at index=get(2) to value=get(3)
      GenRet ptr = codegenElementPtr(get(1), codegenExprMinusOne(get(2)));
      codegenAssign(ptr, get(3));
      break;
    }
    case PRIM_GET_MEMBER: {
      // base=get(1) field symbol=get(2)
      ret = codegenFieldPtr(get(1), get(2));
      // Used to only do addrOf if
      // !get(2)->typeInfo()->symbol->hasFlag(FLAG_REF)
      // but that unnaturaly depends on the type of the field.
      ret = codegenAddrOf(ret);
      break; }
    case PRIM_GET_SVEC_MEMBER: {
      // get tuple base=get(1) at index=get(2)
      Type* tupleType = get(1)->getValType();
      ret = codegenElementPtr(get(1), codegenExprMinusOne(get(2)));
      if (!tupleType->getField("x1")->type->symbol->hasFlag(FLAG_REF))
        ret = codegenAddrOf(ret);
      break;
    }
    case PRIM_SET_MEMBER: {
      // base=get(1) field=get(2) value=get(3)
      GenRet ptr = codegenFieldPtr(get(1), get(2));
      GenRet val = get(3);
      codegenAssign(ptr, val);
      break; }
    case PRIM_CHECK_NIL: {
      GenRet ptr = get(1);
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        ptr = codegenRaddr(ptr);
      codegenCall("chpl_check_nil", ptr, info->lineno, info->filename); 
      break; }
    case PRIM_LOCAL_CHECK:
    {
      // arguments are (wide ptr, line, function/file, error string)
      const char *error;
      if (!get(1)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE, FLAG_WIDE_CLASS))
        break;
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) &&
          get(1)->typeInfo()->getField("addr")->typeInfo()->symbol->
            hasFlag(FLAG_EXTERN)) {
        error = "cannot pass non-local extern class to extern procedure";
      } else {
        error = "cannot access remote data in local block";
      }
      GenRet filename;
      if (get(3)->typeInfo() == wideStringType)
        // We expect that the filename string will always be local.
        filename = codegenRaddr(get(3));
      else
        filename = GenRet(get(3));
      codegenCall("chpl_check_local",
                  codegenRnode(get(1)), get(2), filename, error); 
      break;
    }
    case PRIM_SYNC_INIT:
    case PRIM_SYNC_DESTROY:{
      GenRet fieldPtr = codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux"));
      codegenCall(primitive->tag == PRIM_SYNC_INIT ?
                         "chpl_sync_initAux" : "chpl_sync_destroyAux",
                         fieldPtr);
      break;
      }
    case PRIM_SYNC_LOCK:
      codegenCall("chpl_sync_lock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")));
      break;
    case PRIM_SYNC_UNLOCK:
      codegenCall("chpl_sync_unlock", 
                  codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")));
      break;
    case PRIM_SYNC_WAIT_FULL:
      // sync var, lineno, filename
      codegenCall("chpl_sync_waitFullAndLock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")),
                   get(2), get(3));
      break;
    case PRIM_SYNC_WAIT_EMPTY:
      // sync var, lineno, filename
      codegenCall("chpl_sync_waitEmptyAndLock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")),
                   get(2), get(3));
      break;
    case PRIM_SYNC_SIGNAL_FULL:
      codegenCall("chpl_sync_markAndSignalFull",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")));
      break;
    case PRIM_SYNC_SIGNAL_EMPTY:
      codegenCall("chpl_sync_markAndSignalEmpty",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "sync_aux")));
      break;
    case PRIM_SINGLE_INIT:
    case PRIM_SINGLE_DESTROY:
      codegenCall( primitive->tag == PRIM_SINGLE_INIT ?
                  "chpl_single_initAux" : "chpl_single_destroyAux",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "single_aux")));
      break;
    case PRIM_SINGLE_LOCK:
      codegenCall( "chpl_single_lock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "single_aux")));
      break;
    case PRIM_SINGLE_UNLOCK:
      codegenCall( "chpl_single_unlock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "single_aux")));
      break;
    case PRIM_SINGLE_WAIT_FULL:
      // single, lineno, filename
      codegenCall( "chpl_single_waitFullAndLock",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "single_aux")),
                   get(2), get(3));
      break;
    case PRIM_SINGLE_SIGNAL_FULL:
      codegenCall( "chpl_single_markAndSignalFull",
                   codegenLocalAddrOf(codegenFieldPtr(get(1), "single_aux")));
      break;
    case PRIM_WRITEEF: {
      // get(1) is argument (class, wide or not), get(2) is what to write.
      GenRet s;
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        s = codegenRaddr(get(1));
      else
        s = get(1);
      codegenCall( "chpl_write_EF", s, get(2));
      break; }
    case PRIM_WRITEFF:
    case PRIM_WRITEXF:
      {
        const char *fn = NULL;
        if( primitive->tag == PRIM_WRITEFF ) fn = "chpl_write_FF";
        if( primitive->tag == PRIM_WRITEXF ) fn = "chpl_write_XF";
        // get(1) is argument (class, wide or not), get(2) is what to write.
        GenRet s;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
          s = codegenRaddr(get(1));
        else
          s = get(1);
        codegenCall( fn, s, get(2));
        break;
      }
    case PRIM_SYNC_RESET: {
      // get(1) is argument (class, wide or not)
      GenRet s;
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        s = codegenRaddr(get(1));
      else
        s = get(1);
      codegenCall( "chpl_sync_reset", s);
      break; }
    case PRIM_READFE:
    case PRIM_READFF:
    case PRIM_READXX:
      {
        const char *fn = NULL;
        if( primitive->tag == PRIM_READFE ) fn = "chpl_read_FE";
        if( primitive->tag == PRIM_READFF ) fn = "chpl_read_FF";
        if( primitive->tag == PRIM_READXX ) fn = "chpl_read_XX";
        GenRet s;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
          s = codegenRaddr(get(1));
        else
          s = get(1);
        ret = codegenCallExpr(fn, s);
        break;
      }
    case PRIM_SYNC_IS_FULL: {
      // get(1) is sync var get(2) is isSimpleSyncBaseType( arg )
      GenRet s = get(1);
      GenRet val_ptr = codegenLocalAddrOf(codegenFieldPtr(s, "value"));
      GenRet aux = codegenLocalAddrOf(codegenFieldPtr(s, "sync_aux"));
      ret = codegenCallExpr("chpl_sync_isFull",
                            val_ptr, aux, get(2));
      break; }
    case PRIM_SINGLE_WRITEEF:
      {
        // get(1) is argument (class, wide or not), get(2) is what to write.
        GenRet s;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
          s = codegenRaddr(get(1));
        else
          s = get(1);
        codegenCall( "chpl_single_write_EF", s, get(2));
        break;
      }
    case PRIM_SINGLE_RESET:
      {
        // get(1) is argument (class, wide or not)
        GenRet s;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
          s = codegenRaddr(get(1));
        else
          s = get(1);
        codegenCall( "chpl_single_reset", s);
        break;
      }
    case PRIM_SINGLE_READFF:
    case PRIM_SINGLE_READXX:
      {
        const char *fn = NULL;
        if( primitive->tag == PRIM_SINGLE_READFF ) fn = "chpl_single_read_FF";
        if( primitive->tag == PRIM_SINGLE_READXX ) fn = "chpl_single_read_XX";
        GenRet s;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
          s = codegenRaddr(get(1));
        else
          s = get(1);
        ret = codegenCallExpr(fn, s);
        break;
      }
    case PRIM_SINGLE_IS_FULL:
    {
      // get(1) is sync var get(2) is isSimpleSyncBaseType( arg )
      GenRet s = get(1);
      GenRet val_ptr = codegenLocalAddrOf(codegenFieldPtr(s, "value"));
      GenRet aux = codegenLocalAddrOf(codegenFieldPtr(s, "single_aux"));
      ret = codegenCallExpr("chpl_single_isFull",
                            val_ptr, aux, get(2));
      break; 
    }
    case PRIM_PROCESS_TASK_LIST: {
      GenRet taskListPtr = codegenFieldPtr(get(1), "taskList");
      codegenCall("chpl_task_processTaskList", codegenValue(taskListPtr));
      break; }
    case PRIM_EXECUTE_TASKS_IN_LIST:
      codegenCall("chpl_task_executeTasksInList", get(1));
      break;
    case PRIM_FREE_TASK_LIST:
    {
      if (fNoMemoryFrees)
        break;
      codegenCall("chpl_task_freeTaskList", get(1));
      break;
    }
    case PRIM_GET_SERIAL:
      ret = codegenCallExpr("chpl_task_getSerial");
      break;
    case PRIM_SET_SERIAL:
      codegenCall("chpl_task_setSerial", codegenValue(get(1)));
      break;
    case PRIM_GET_SUBLOC_ID:
      ret = codegenCallExpr("chpl_task_getSubLoc");
      break;
    case PRIM_SET_SUBLOC_ID:
      codegenCall("chpl_task_setSubLoc", codegenValue(get(1)));
      break;
    case PRIM_LOC_GET_NODE:
      ret = codegenValue(codegenFieldPtr(get(1), "node"));
      break;
    case PRIM_LOC_GET_SUBLOC:
      ret = codegenValue(codegenFieldPtr(get(1), "subloc"));
      break;
    case PRIM_CHPL_COMM_GET:
    case PRIM_CHPL_COMM_PUT: {
      // args are:
      //   localvar, locale, remote addr, eltSize, get(4)==length, line, file
      const char* fn;
      if (primitive->tag == PRIM_CHPL_COMM_GET) {
        fn = "chpl_gen_comm_get";
      } else {
        fn = "chpl_gen_comm_put";
      }
      TypeSymbol *dt;

      GenRet localAddr = codegenValuePtr(get(1));

      // destination data array
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        Symbol *sym = get(1)->typeInfo()->getField("addr", true);
        INT_ASSERT(sym);
        dt = sym->typeInfo()->getValType()->symbol;
        localAddr = codegenRaddr(localAddr);
      } else {
        dt = get(1)->typeInfo()->getValType()->symbol;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          localAddr = codegenDeref(localAddr);
        }
      }

      GenRet locale;
      if( get(2)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE,FLAG_REF) ) {
        locale = codegenValue(codegenDeref(get(2)));
      } else {
        locale = codegenValue(get(2));
      }

      // source data array
      GenRet remoteAddr = get(3);
      SymExpr *sym = toSymExpr(get(3));
      INT_ASSERT(sym);
      if( sym->typeInfo()->symbol->hasFlag(FLAG_WIDE) ) {
        remoteAddr = codegenRaddr(remoteAddr);
      } else {
        if( !sym->typeInfo()->symbol->hasFlag(FLAG_REF) ) {
          remoteAddr = codegenAddrOf(remoteAddr);
        }
      }
      /*if( remoteAddrArg.isLVPtr == GEN_WIDE_PTR ) {
        remoteAddr = codegenRaddr(remoteAddrArg);
      } else {
        remoteAddr = codegenValuePtr(remoteAddrArg);
      }*/
      GenRet eltSize = codegenSizeof(dt->typeInfo());
      GenRet len;
      if( get(4)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE,FLAG_REF) ) {
        len = codegenValue(codegenDeref(get(4)));
      } else {
        len = codegenValue(get(4));
      }
      if (!fLLVMWideOpt ){
        codegenCall(fn, codegenCastToVoidStar(localAddr), locale, remoteAddr, 
            eltSize,genTypeStructureIndex(dt), len,
            get(5), get(6));
      } else {
        // Figure out the locale-struct value to put into the wide address
        // (instead of just using the node number)
        GenRet lc = codegenLocaleForNode(locale);
        if( localAddr.isLVPtr == GEN_PTR )
          localAddr = codegenAddrOf( localAddr );
        if( localAddr.isLVPtr == GEN_WIDE_PTR )
          localAddr = codegenRaddr( localAddr );

        if (primitive->tag == PRIM_CHPL_COMM_GET) {
          codegenCallMemcpy(localAddr, 
                            codegenAddrOf(codegenWideAddr(lc, remoteAddr)),
                            codegenMul(eltSize, len), dt->typeInfo());
        } else {
          codegenCallMemcpy(codegenAddrOf(codegenWideAddr(lc, remoteAddr)),
                            localAddr, 
                            codegenMul(eltSize, len), dt->typeInfo());
        }
      }
      break;
    }
      //Strided versions of get and put
    case PRIM_CHPL_COMM_PUT_STRD: 
    case PRIM_CHPL_COMM_GET_STRD: {
      //args are: localvar, dststr addr, locale, remote addr, srcstr addr
      // count addr, strlevels, elem
      const char* fn;
      if (primitive->tag == PRIM_CHPL_COMM_GET_STRD) {
        fn = "chpl_gen_comm_get_strd";
      } else {
        fn = "chpl_gen_comm_put_strd";
      }
      TypeSymbol *dt;

      GenRet localAddr = codegenValuePtr(get(1));

      // destination data array
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        Symbol *sym = get(1)->typeInfo()->getField("addr", true);
        INT_ASSERT(sym);
        dt = sym->typeInfo()->getValType()->symbol;
        localAddr = codegenRaddr(localAddr);
      } else {
        dt = get(1)->typeInfo()->getValType()->symbol;
        if (get(1)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          localAddr = codegenDeref(localAddr);
        }
      }

      // destination strides local array
      GenRet dststr = codegenValuePtr(get(2));

      if (get(2)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        Symbol *sym = get(2)->typeInfo()->getField("addr", true);
        INT_ASSERT(sym);
        dststr = codegenRaddr(dststr);
      } else {
        if (get(2)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          dststr = codegenDeref(dststr);
        }
      }

      // locale id 
      GenRet locale;
      if( get(3)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE,FLAG_REF) ) {
        locale = codegenValue(codegenDeref(get(3)));
      } else {
        locale = codegenValue(get(3));
      } 

      // source data array
      GenRet remoteAddr = get(4);
      SymExpr *sym = toSymExpr(get(4));
      INT_ASSERT(sym);
      if( sym->typeInfo()->symbol->hasFlag(FLAG_WIDE) ) {
        remoteAddr = codegenRaddr(remoteAddr);
      } else {
        if( !sym->typeInfo()->symbol->hasFlag(FLAG_REF) ) {
          remoteAddr = codegenAddrOf(remoteAddr);
        }
      }

      // source strides local array
      GenRet srcstr = codegenValuePtr(get(5));

      if (get(5)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        Symbol *sym = get(5)->typeInfo()->getField("addr", true);
        INT_ASSERT(sym);
        srcstr = codegenRaddr(srcstr);
      } else {
        if (get(5)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          srcstr = codegenDeref(srcstr);
        }
      }

      // count local array
      GenRet count = codegenValuePtr(get(6));

      if (get(6)->typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        Symbol *sym = get(6)->typeInfo()->getField("addr", true);
        INT_ASSERT(sym);
        count = codegenRaddr(count);
      } else {
        if (get(6)->typeInfo()->symbol->hasFlag(FLAG_REF)) {
          count = codegenDeref(count);
        }
      }

      // stridelevels
      GenRet stridelevels;
      if( get(7)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE,FLAG_REF) ) {
        stridelevels = codegenValue(codegenDeref(get(7)));
      } else {
        stridelevels = codegenValue(get(7));
      } 

      // eltSize
      GenRet eltSize = codegenSizeof(dt->typeInfo());

      codegenCall(fn,
                  codegenCastToVoidStar(localAddr),
                  codegenCastToVoidStar(dststr),
                  locale, remoteAddr, codegenCastToVoidStar(srcstr),
                  codegenCastToVoidStar(count), stridelevels, eltSize,
                  genTypeStructureIndex(dt), get(8), get(9));

      break;
    }
    case PRIM_CHPL_ALLOC:
    {
      GenRet size;

      // If Chapel class or record
      if (ClassType* ct = toClassType(toTypeSymbol(typeInfo()->symbol)->type)) {
        size = codegenSizeof(ct->classStructName(true));
      } else {
        size = codegenSizeof(typeInfo());
      }

      GenRet description = codegenAdd(get(2), codegenUseGlobal("CHPL_RT_MD_NUM"));
      GenRet allocated;
      allocated = codegenCallExpr("chpl_mem_alloc", size,
                                  description, get(3), get(4));

      ret = codegenCast(typeInfo()->symbol->cname, allocated);
      break;
    }
    case PRIM_CHPL_FREE: {
      if (fNoMemoryFrees)
        break;
      INT_ASSERT(numActuals() == 3);
      Expr * ptrExpr = get(1);
      if (ptrExpr->typeInfo()->getValType() == dtString &&
          ! ptrExpr->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        // The representation of a local string may be shared among several
        // variables.  We can't (yet) tell who owns the data, so we can't
        // release it ever.  Otherwise, we'll get double-deletion errors.
        // This unfortunate situation will change when
        // string representations are reference-counted.
        break;
      if( ptrExpr->typeInfo()->symbol->hasFlag(FLAG_DATA_CLASS))
        INT_FATAL(this, "cannot delete data class");
      GenRet ptr; 
      ptr = codegenValue(ptrExpr);
      if (ptrExpr->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        ptr = codegenRaddr(ptr);
      }
      codegenCall("chpl_mem_free", codegenCastToVoidStar(ptr), get(2), get(3));
      // MPF - note we do not set the pointer to NULL here
      // because it would not change any copies of the pointer
      // and we're toast in any case of use-after-free.
      // Arguably, it could be put back in for earlier error detection.
      break;
    }
    case PRIM_CAST: 
    {
      if (typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS) ||
          typeInfo()->symbol->hasFlag(FLAG_WIDE)) {
        GenRet tmp = get(2);
        ret = codegenWideAddrWithAddr(tmp,
                              codegenCast(get(1)->typeInfo(), 
                                          codegenRaddr(tmp)));
        break;
      }

      Type* dst = get(1)->typeInfo();
      Type* src = get(2)->typeInfo();
      if (dst == src) {
        ret = get(2);
      } else if ((is_int_type(dst) || is_uint_type(dst)) && src == dtTaskID) {
        GenRet v = codegenValue(get(2));
        // cast like this: (type) (intptr_t) v
        ret = codegenCast(typeInfo(), codegenCast("intptr_t", v));
      } else if (dst == dtString || src == dtString) {
        const char* dst_cname = dst->symbol->cname;
        const char* src_cname = src->symbol->cname;
        std::string fn;
        if( dst->symbol->cname[0] == '_' ) {
          fn += src_cname;
          fn += "_to";
          fn += dst_cname;
        } else {
          fn += src_cname;
          fn += "_to_";
          fn += dst_cname;
        }
        if (src == dtString) {
          ret = codegenCallExpr(fn.c_str(), get(2), get(3), get(4)); // TODO?
        } else {
          ret = codegenCallExpr(fn.c_str(), codegenValue(get(2)));
        }
      } else {
        if (isRecord(typeInfo()) || isUnion(typeInfo())) {
          INT_FATAL("TODO - don't like type-punning record/union");
          /*fprintf(outfile, "(*((");
          typeInfo()->codegen(outfile);
          fprintf(outfile, "*)(&(");
          get(2)->codegen(outfile);
          fprintf(outfile, "))))");*/
        } else {
          GenRet v = codegenValue(get(2));
          ret = codegenCast(typeInfo()->symbol->cname, v);
        }
      }
      break;
    }
    case PRIM_DYNAMIC_CAST: 
    {
      if (typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        INT_FATAL(this, "wide class dynamic cast is not normal");
      GenRet tmp = codegenFieldCidPtr(get(2));
      GenRet value = codegenValue(tmp);
      GenRet ok = codegenDynamicCastCheck(value, typeInfo());
      // if ok, (typeInfo()->symbol->cname) (value)
      // otherwise, NULL
      GenRet cast = codegenCast(typeInfo(), codegenValue(get(2)));
      GenRet nul = codegenCast(typeInfo(), codegenNullPointer());

      ret = codegenTernary(ok, cast, nul);
      break;
    }
    case PRIM_GC_CC_INIT:
    case PRIM_GC_ADD_ROOT:
    case PRIM_GC_ADD_NULL_ROOT:
    case PRIM_GC_DELETE_ROOT:
    case PRIM_GC_CLEANUP:
      INT_FATAL("GC primitives not supported");
      break;
    case PRIM_NODE_ID:
      ret = codegenGetNodeID();
      break;
    case PRIM_ON_LOCALE_NUM:
      if (numActuals() < 2)
        ret = codegenLocaleID(codegenValue(get(1)), codegenZero());
      else
        ret = codegenLocaleID(codegenValue(get(1)), codegenValue(get(2)));
      break;
    case PRIM_ALLOC_GVR:
      codegenCall("chpl_comm_alloc_registry",
                  new_IntSymbol(numGlobalsOnHeap, INT_SIZE_32));
      break;
    case PRIM_HEAP_REGISTER_GLOBAL_VAR: 
    {
      GenRet idx = codegenValue(get(1));
      GenRet var = get(2);
      GenRet ptr_wide_ptr = codegenAddrOf(var); 

#ifdef HAVE_LLVM
      if( fLLVMWideOpt ) {
        // With fLLVMWideOpt, ptr_wide_ptr is actually pointing
        // to a global pointer at this point, and we can't call
        // a C function on a global type (since it won't exist
        // by the time we are linking - it will have been lowered
        // to a wide type). So add the call to convert it to
        // wide (which makes the IR type check but will just
        // get removed in the eventual code).
        llvm::Type* ptr_wide_ptr_ty = ptr_wide_ptr.val->getType();

        // call GLOBAL_FN_GLOBAL_TO_WIDE dummy function 
        llvm::Function* fn = getGlobalToWideFn(info->module,
                                               &info->globalToWideInfo,
                                               ptr_wide_ptr_ty);

        INT_ASSERT(fn);

        ptr_wide_ptr.val = info->builder->CreateCall(fn, ptr_wide_ptr.val);
      }
#endif

      codegenCall("chpl_heap_register_global_var", idx,
                  codegenCast("ptr_wide_ptr_t", ptr_wide_ptr));
      break;
    }
    case PRIM_HEAP_BROADCAST_GLOBAL_VARS:
      codegenCall("chpl_gen_comm_broadcast_global_vars", get(1)); 
      break;
    case PRIM_PRIVATE_BROADCAST:
      codegenCall("chpl_comm_broadcast_private", get(1), 
                   codegenSizeof(get(2)->typeInfo()),
                   genTypeStructureIndex(get(2)->typeInfo()->symbol));
      break;
    case PRIM_INT_ERROR:
      codegenCall("chpl_internal_error",
                  new_StringSymbol("compiler generated error"));
      break;
    case PRIM_STRING_COPY:
    {
      GenRet cpyFrom = get(1)->codegen();
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        cpyFrom.isLVPtr = GEN_VAL; // Prevent &(char*) syntax.
        ret = codegenCallExpr("chpl_wide_string_copy", cpyFrom, get(2), get(3));
      } else
        ret = codegenBasicPrimitiveExpr(this);
      break;
    }
    case PRIM_STRING_NORMALIZE:
     // string_normalize(ptr, len);
     // string_normalize(ptr);
    {
      // If this is a wide string, overwrite the size field with the second arg
      // (if present) or the internally-computed length.  Otherwise, do nothing.
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS)) {
        // This applies only to wide strings.
        GenRet ptr = get(1);
        GenRet size = codegenRsize(ptr);    // Get a pointer to the size field.
        GenRet len;

        // Use the len argument if present, otherwise use string_length().
        if (numActuals() > 1)
          len = codegenValue(get(2));
        else
        {
          GenRet strlen = codegenCallExpr("string_length", codegenRaddr(get(1)));
          len = codegenAdd(codegenOne(), strlen);
        }

        // Set the value of the size field.
        codegenAssign(size, len);
      }
      break;
    }
    case PRIM_RT_ERROR:
    case PRIM_RT_WARNING:
      ret = codegenBasicPrimitiveExpr(this);
      break;
    case PRIM_NEW_PRIV_CLASS: 
    {
      GenRet arg = get(1);
      if (get(1)->typeInfo()->symbol->hasFlag(FLAG_WIDE_CLASS))
        arg = codegenRaddr(arg);
      codegenCall("chpl_newPrivatizedClass", arg);
      break;
    }                          
    case PRIM_NUM_PRIV_CLASSES:
      ret = codegenCallExpr("chpl_numPrivatizedClasses");
      break;
    case PRIM_WARNING:
      // warning issued, continue codegen
      break;
    case PRIM_FTABLE_CALL: {
      //
      // indirect function call via a function pointer
      //
      GenRet index = codegenValue(get(1));
      GenRet fngen;
      // Generate a cast based upon the arguments.
      if (info->cfile){
        std::string str = "((void(*)(";
        str += get(2)->typeInfo()->symbol->cname;
        if( argMustUseCPtr(get(2)->typeInfo()) ) str += "*";
        str += ",";
        str += get(3)->typeInfo()->symbol->cname;
        if( argMustUseCPtr(get(3)->typeInfo()) ) str += "*";
        str += "))*chpl_ftable[" + index.c + "])";
        fngen.c = str;
      } else {
#ifdef HAVE_LLVM
        GenRet ftable = info->lvt->getValue("chpl_ftable");
        llvm::Value* fnPtrPtr;
        llvm::Instruction* fnPtr;
        llvm::Value* GEPLocs[2];
        GEPLocs[0] = llvm::Constant::getNullValue(
            llvm::IntegerType::getInt64Ty(info->module->getContext()));
        GEPLocs[1] = index.val;
        fnPtrPtr = info->builder->CreateInBoundsGEP(ftable.val, GEPLocs);
        fnPtr = info->builder->CreateLoad(fnPtrPtr);
        // Tell TBAA ftable ptrs don't alias other things, are constant
        fnPtr->setMetadata(llvm::LLVMContext::MD_tbaa, info->tbaaFtableNode);
        // Generate an LLVM function type based upon the arguments.
        llvm::Type *returnType;
        std::vector<llvm::Type *> argumentTypes;
        returnType = llvm::Type::getVoidTy(info->module->getContext());
        llvm::Type* argt;
        argt = get(2)->typeInfo()->codegen().type;
        if( argMustUseCPtr(get(2)->typeInfo()) ) argt = argt->getPointerTo();
        argumentTypes.push_back(argt);
        argt = get(3)->typeInfo()->codegen().type;
        if( argMustUseCPtr(get(3)->typeInfo()) ) argt = argt->getPointerTo();
        argumentTypes.push_back(argt);

        llvm::FunctionType* fnType =
          llvm::FunctionType::get(
              returnType, argumentTypes, /* is var arg */ false);
        // OK, now cast to the fnType.
        fngen.val = info->builder->CreateBitCast(fnPtr, fnType->getPointerTo());
#endif
      }

      std::vector<GenRet> args;
      GenRet arg;
      arg = get(2);
      if( argMustUseCPtr(get(2)->typeInfo()) ) arg = codegenLocalAddrOf(arg);
      args.push_back(arg);
      arg = get(3);
      if( argMustUseCPtr(get(3)->typeInfo()) ) arg = codegenLocalAddrOf(arg);
      args.push_back(arg);

      ret = codegenCallExpr(fngen, args, NULL, true);
      break;
    }
    case PRIM_VMT_CALL: {
      GenRet fnPtr;
      GenRet index;
      FnSymbol* fn = NULL;
      int startArgs = 3;    // Where actual arguments begin.
      SymExpr* se = toSymExpr(get(1));  // the function symbol
      INT_ASSERT(se);
      fn = toFnSymbol(se->var);
      INT_ASSERT(fn);
      {
        GenRet i = codegenValue(get(2));    // the cid
        GenRet j = new_IntSymbol(virtualMethodMap.get(fn), INT_SIZE_64);
        INT_ASSERT(gMaxVMT >= 0);
        GenRet maxVMTConst = new_IntSymbol(gMaxVMT, INT_SIZE_64);
        // indexExpr = maxVMT * i + j
        index = codegenAdd(codegenMul(maxVMTConst, i), j);
      }
      if (info->cfile){
        fnPtr.c = std::string("chpl_vmtable") + "[" + index.c + "]";
      } else {
#ifdef HAVE_LLVM
        GenRet table = info->lvt->getValue("chpl_vmtable");
        llvm::Value* fnPtrPtr;
        llvm::Value* GEPLocs[1];
        //GEPLocs[0] = llvm::Constant::getNullValue(
        //    llvm::IntegerType::getInt64Ty(info->module->getContext()));
        GEPLocs[0] = index.val;
        fnPtrPtr = info->builder->CreateInBoundsGEP(table.val, GEPLocs);
        llvm::Instruction* fnPtrV = info->builder->CreateLoad(fnPtrPtr);
        // Tell TBAA vmtable loads don't alias anything else, are constant 
        fnPtrV->setMetadata(llvm::LLVMContext::MD_tbaa, info->tbaaVmtableNode);
        fnPtr.val = fnPtrV;
#endif
      }
      // the function expression to call.
      GenRet fngen = fn->codegenCast(fnPtr);

      std::vector<GenRet> args;
      int i = startArgs;
      for_formals(arg, fn) {
        args.push_back(get(i++));
      }

      ret = codegenCallExpr(fngen, args, fn, true);
      break;
    }
    case NUM_KNOWN_PRIMS:
      INT_FATAL(this, "impossible");
      break;
    case PRIM_BLOCK_XMT_PRAGMA_NOALIAS:
      // This case allows __primitive("noalias pragma") to generate 
      // _Pragma("mta assert noalias <variable list>")
      // EVERY variable within a block is added to the variable list,
      // which means several variables will spawn warnings (ignored in
      // Makefile.cray-mta). Eventually, we'll only generate invariant 
      // pointer variables.
      if (!(strcmp(CHPL_TARGET_PLATFORM, "cray-xmt"))) {
        INT_ASSERT(c);
        Vec<SymExpr*> se;
        Vec<DefExpr*> de_parent;
        Vec<VarSymbol*> vs_parent;
        collectSymExprs(this->next, se);
        collectDefExprs(this->parentExpr, de_parent);
        forv_Vec(DefExpr*, def, de_parent) {
          VarSymbol* vs = toVarSymbol(def->sym);
          if (vs) vs_parent.add(vs);
        }
        forv_Vec(SymExpr*, sym, se) {
          if (isVarSymbol(sym->var) && (!(isPrimitiveType(sym->var->type)))) {
            VarSymbol* vs = toVarSymbol(sym->var);
            if (vs_parent.in(vs)) {
              std::string stmt;
              stmt += "_Pragma(\"mta assert noalias *";
              stmt += sym->var->cname;
              stmt += "\")";
              info->cStatements.push_back(stmt);
            }
          }
        }
        this->remove();
      }
      break;
    default:
      INT_FATAL(this, "primitive codegen fail; should it still be in the AST?");
      if( c ) {
        std::string stmt;
        stmt += "/* ERR ";
        stmt += primitive->name;
        stmt += "*/";
        info->cStatements.push_back(stmt);
      }
    }
    if (c && getStmtExpr() && getStmtExpr() == this)
      if( ret.c.length() > 0 ) info->cStatements.push_back(ret.c + ";\n");
    return ret;
  }

  FnSymbol* fn = isResolved();
  INT_ASSERT(fn);

  if (fn->hasFlag(FLAG_BEGIN_BLOCK)) {
    // get(1) is a class containing bundled arguments
    std::vector<GenRet> args(7);
    args[0] = new_IntSymbol(ftableMap.get(fn), INT_SIZE_64);

    if (Expr *actuals = get(1)) {
      args[1] = codegenCastToVoidStar(codegenValue(actuals));
    } else {
      args[1] = codegenNullPointer();
    }
    ClassType *bundledArgsType = toClassType(toSymExpr(get(1))->typeInfo());
    // lastField is the _endCount field.
    int lastField = bundledArgsType->fields.length;
    GenRet endCountPtr = codegenValue(
        codegenFieldPtr(get(1), bundledArgsType->getField(lastField)));
    // endCount is either an address or {locale, ptr}
    GenRet endCountValue = codegenValue(endCountPtr);
    GenRet taskList;

    if (bundledArgsType->getField(lastField)->typeInfo()->symbol->
        hasFlag(FLAG_WIDE_CLASS)) {
      GenRet node = codegenRnode(endCountValue);
      endCountValue = codegenRaddr(endCountValue);
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList"));
      taskList = codegenTernary(
                      codegenNotEquals(node, codegenGetNodeID()),
                      codegenNullPointer(),
                      taskList);
    } else {
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList"));
    }
    args[2] = taskList;
    if (bundledArgsType->getField(lastField)->typeInfo()->symbol->
        hasFlag(FLAG_WIDE_CLASS)) {
      args[3] = codegenRnode(endCountPtr);
    } else {
      args[3] = codegenGetNodeID();
    }

    args[4] = new_BoolSymbol(true, BOOL_SIZE_8);
    args[5] = fn->linenum();
    args[6] = fn->fname();


    genComment(fn->cname, true);
    codegenCall("chpl_task_addToTaskList", args);
    return ret;
  } else if (fn->hasFlag(FLAG_COBEGIN_OR_COFORALL_BLOCK)) {
    // get(1) is a class containing bundled arguments
    std::vector<GenRet> args(7);
    args[0] = new_IntSymbol(ftableMap.get(fn), INT_SIZE_64);

    if (Expr *actuals = get(1)) {
      args[1] = codegenCastToVoidStar(codegenValue(actuals));
    } else {
      args[1] = codegenNullPointer();
    }
    ClassType *bundledArgsType = toClassType(toSymExpr(get(1))->typeInfo());
    int endCountField = 0;
    for (int i = 1; i <= bundledArgsType->fields.length; i++) {
      if (!strcmp(bundledArgsType->getField(i)->typeInfo()->symbol->name,
                  "_ref(_EndCount)")
          || !strcmp(bundledArgsType->getField(i)->typeInfo()->symbol->name,
                     "__wide__ref__EndCount")
          || !strcmp(bundledArgsType->getField(i)->typeInfo()->symbol->name,
                     "_EndCount")
          || !strcmp(bundledArgsType->getField(i)->typeInfo()->symbol->name,
                     "__wide__EndCount")) {
        endCountField = i;
        break;
      }
    }
    if (endCountField == 0)
      INT_FATAL(this, "cobegin/codegen codegen - _EndCount field not found");

    GenRet endCountPtr =
      codegenValue(
          codegenFieldPtr(get(1), bundledArgsType->getField(endCountField)));
    Type *endCountType = bundledArgsType->getField(endCountField)->typeInfo();
    // endCount is either an address or {locale, ptr} -- it is a class.
    GenRet endCountValue = codegenValue(endCountPtr);
    GenRet taskList;

    if (endCountType->symbol->hasFlag(FLAG_WIDE)) {
      GenRet node = codegenRnode(endCountValue);
      while(endCountValue.chplType->symbol->hasEitherFlag(FLAG_WIDE,FLAG_REF)){
        endCountValue = codegenLocalDeref(endCountValue);
      }
      // Now, we should have a wide pointer to a class
      // make it into a local pointer to a class.
      endCountValue = codegenRaddr(endCountValue);
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList")); 
      taskList = codegenTernary(
                       codegenNotEquals(node, codegenGetNodeID()),
                       codegenNullPointer(),
                       taskList);
    } else if (endCountType->symbol->hasFlag(FLAG_WIDE_CLASS)) {
      GenRet node = codegenRnode(endCountValue);
      endCountValue = codegenRaddr(endCountValue);
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList"));
      taskList = codegenTernary(
                     codegenNotEquals(node, codegenGetNodeID()),
                     codegenNullPointer(),
                     taskList);
    } else if (endCountType->symbol->hasFlag(FLAG_REF)) {
      endCountValue = codegenDeref(endCountValue);
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList"));
    } else {
      taskList = codegenLocalAddrOf(codegenFieldPtr(endCountValue, "taskList"));
    }
    args[2] = taskList;
    args[3] = codegenGetNodeID(),
    args[4] = new_BoolSymbol(false, BOOL_SIZE_8);
    args[5] = fn->linenum();
    args[6] = fn->fname();


    genComment(fn->cname, true);
    codegenCall("chpl_task_addToTaskList", args);
    return ret;
  } else if (fn->hasFlag(FLAG_ON_BLOCK)) {
    const char* fname = NULL;
    if (fn->hasFlag(FLAG_NON_BLOCKING))
      fname = "chpl_comm_nonblocking_on";
    else if (fn->hasFlag(FLAG_FAST_ON))
      fname = "chpl_comm_fork_fast";
    else
      fname = "chpl_comm_fork";

    TypeSymbol* argType = toTypeSymbol(get(2)->typeInfo()->symbol);
    if (argType == NULL) {
      INT_FATAL("could not get a type symbol");
    }
    
    ClassType* ct = toClassType(argType->typeInfo());
    if (!ct) {
      INT_FATAL("Expected a class type in %s argument", fname);
    }
    std::string ctype = ct->classStructName(true);

    genComment(fn->cname, true);
    GenRet nodeVal = codegenValue(codegenFieldPtr(get(1), "node"));
    codegenCall(fname,
    // Note that we select just the node portion of the localeID.
    // The comm routines only care about nodes, not sublocales.
    // The sublocale ID is passed to the called routine via the bundled args.
               nodeVal,
               new_IntSymbol(ftableMap.get(fn), INT_SIZE_32),
               get(2),
               codegenSizeof(ctype.c_str()),
               genTypeStructureIndex(argType) );
    return ret;
  }

  // Do not code generate calls to functions marked FLAG_NO_CODEGEN.
  if (fn->hasFlag(FLAG_NO_CODEGEN)) return ret;

  GenRet base = baseExpr->codegen();

  std::vector<GenRet> args(numActuals());

  int i = 0;
  for_formals_actuals(formal, actual, this) {
    Type* actualType = actual->typeInfo();

    GenRet arg;

    arg = actual;

    SymExpr* se = toSymExpr(actual);
    if (se && isFnSymbol(se->var))
      arg = codegenCast("chpl_fn_p", arg);

    // Handle passing strings to externs
    if (fn->hasFlag(FLAG_EXTERN)) {
      if (actualType == dtString)
        arg = codegenCastToCharStar(codegenValue(arg));
      else if (passingWideStringToExtern(actualType))// checks for ref(widestr)
        arg = codegenAddrOf(codegenWideStringField(codegenDeref(arg),"addr"));
      else if( actualType->symbol->hasFlag(FLAG_WIDE) ||
               arg.isLVPtr == GEN_WIDE_PTR) {
        arg = codegenRaddr(codegenValue(arg));
      } else if (formal->type->symbol->hasFlag(FLAG_REF) &&
                 formal->type->symbol->getValType()->symbol->
                    hasFlag(FLAG_STAR_TUPLE) &&
                 actualType->symbol->hasFlag(FLAG_REF) ) {
        // In C, a fixed-size-array lvalue is already a pointer,
        // so we deref here. But for LLVM, if we deref we will
        // end up passing the tuple by value, which is not right.
        if( c ) arg = codegenDeref(arg);
      }
    }
 
    args[i] = arg;
    i++;
  }

  // handle any special cases for which 
  // bool isBuiltinExternCFunction(const char* cname) returns true.
  //
  // special case: for CallExpr sizeof(..)
  FnSymbol* fsym = isResolved();
  if (!c && fsym && fsym->hasFlag(FLAG_EXTERN) &&
      (0 == strcmp(fsym->name, "sizeof"))){
#ifdef HAVE_LLVM
    if( args[0].type ) return codegenSizeof(args[0].type);
    else return codegenSizeof(codegenValue(args[0]).val->getType());
#endif
  }

  //INT_ASSERT(base);
  ret = codegenCallExpr(base, args, fn, true);

  if(!c) {
#ifdef HAVE_LLVM
    // We might have to convert the return from the function
    // if clang did some structure-expanding.
    if( this->typeInfo() != dtVoid ) {
      GenRet ty = this->typeInfo();
      INT_ASSERT(ty.type); 
      llvm::Value* converted = convertValueToType(ret.val, ty.type);
      INT_ASSERT(converted);
      ret.val = converted;
    }
#endif
  }

  if (c && getStmtExpr() && getStmtExpr() == this)
    info->cStatements.push_back(ret.c + ";\n");

  return ret;
}


bool CallExpr::isPrimitive(PrimitiveTag primitiveTag) {
  return primitive && primitive->tag == primitiveTag;
}


bool CallExpr::isPrimitive(const char* primitiveName) {
  return primitive && !strcmp(primitive->name, primitiveName);
}


NamedExpr::NamedExpr(const char* init_name, Expr* init_actual) : 
  Expr(E_NamedExpr),
  name(init_name),
  actual(init_actual)
{
  gNamedExprs.add(this);
}


void NamedExpr::verify() {
  Expr::verify();
  if (astTag != E_NamedExpr) {
    INT_FATAL(this, "Bad NamedExpr::astTag");
  }
  if (actual && actual->parentExpr != this)
    INT_FATAL(this, "Bad NamedExpr::actual::parentExpr");
}


NamedExpr*
NamedExpr::copyInner(SymbolMap* map) {
  return new NamedExpr(astr(name), COPY_INT(actual));
}


void NamedExpr::replaceChild(Expr* old_ast, Expr* new_ast) {
  if (old_ast == actual) {
    actual = new_ast;
  } else {
    INT_FATAL(this, "Unexpected case in NamedExpr::replaceChild");
  }
}


Type* NamedExpr::typeInfo(void) {
  return actual->typeInfo();
}


GenRet NamedExpr::codegen() {
  GenRet ret;
  INT_FATAL(this, "NamedExpr::codegen not implemented");
  return ret;
}


void NamedExpr::prettyPrint(std::ofstream *o) {
  *o << "<NamedExprType>";
}


bool 
get_int(Expr *e, int64_t *i) {
  if (e) {
    if (SymExpr *l = toSymExpr(e)) {
      if (VarSymbol *v = toVarSymbol(l->var)) {
        if (v->immediate) {
          if (v->immediate->const_kind == NUM_KIND_INT) {
            *i = v->immediate->int_value();
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool 
get_uint(Expr *e, uint64_t *i) {
  if (e) {
    if (SymExpr *l = toSymExpr(e)) {
      if (VarSymbol *v = toVarSymbol(l->var)) {
        if (v->immediate) {
          if (v->immediate->const_kind == NUM_KIND_UINT) {
            *i = v->immediate->uint_value();
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool 
get_string(Expr *e, const char **s) {
  if (e) {
    if (SymExpr *l = toSymExpr(e)) {
      if (VarSymbol *v = toVarSymbol(l->var)) {
        if (v->immediate && v->immediate->const_kind == CONST_KIND_STRING) {
          *s = v->immediate->v_string;
          return true;
        }
      }
    }
  }
  return false;
}

const char*
get_string(Expr* e) {
  const char* s = NULL;
  if (!get_string(e, &s))
    INT_FATAL(e, "string literal expression expected");
  return s;
}

VarSymbol * 
get_constant(Expr *e) {
  if (e) {
    if (SymExpr *l = toSymExpr(e)) {
      if (VarSymbol *v = toVarSymbol(l->var)) {
        if (v->immediate != 0)
          return v;
      }
    }
  }
  return 0;
}


// getNextExpr(expr) returns the lexically next expr in a normalized
// tree
#define AST_RET_CHILD(_t, _m) \
  if (((_t*)expr)->_m) return getFirstExpr(((_t*)expr)->_m)
#define AST_RET_LIST(_t, _m) \
  if (((_t*)expr)->_m.head) return getFirstExpr(((_t*)expr)->_m.head)

Expr* getFirstExpr(Expr* expr) {
  switch (expr->astTag) {
  default:
    INT_FATAL(expr, "unexpected expr in getFirstExpr");
    return NULL;
  case E_SymExpr:
  case E_UnresolvedSymExpr:
  case E_DefExpr:
    return expr;
  case E_BlockStmt:
    AST_RET_CHILD(BlockStmt, blockInfo);
    AST_RET_LIST(BlockStmt, body);
    break;
  case E_CondStmt:
    AST_RET_CHILD(CondStmt, condExpr);
    break;
  case E_GotoStmt:
    AST_RET_CHILD(GotoStmt, label);
    break;
  case E_CallExpr:
    AST_RET_CHILD(CallExpr, baseExpr);
    AST_RET_LIST(CallExpr, argList);
    break;
  case E_NamedExpr:
    AST_RET_CHILD(NamedExpr, actual);
    break;
  }
  return expr;
}

Expr* getNextExpr(Expr* expr) {
  if (expr->next)
    return getFirstExpr(expr->next);
  if (CallExpr* parent = toCallExpr(expr->parentExpr)) {
    if (expr == parent->baseExpr && parent->argList.head)
      return getFirstExpr(parent->argList.head);
  } else if (CondStmt* parent = toCondStmt(expr->parentExpr)) {
    if (expr == parent->condExpr && parent->thenStmt)
      return getFirstExpr(parent->thenStmt);
    else if (expr == parent->thenStmt && parent->elseStmt)
      return getFirstExpr(parent->elseStmt);
  } else if (BlockStmt* parent = toBlockStmt(expr->parentExpr)) {
    if (expr == parent->blockInfo && parent->body.head)
      return getFirstExpr(parent->body.head);
  }
  if (expr->parentExpr)
    return expr->parentExpr;
  return NULL;
}

static bool
isIdentifierChar(const char c) {
  return ((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          (c == '$') ||
          (c == '_') || (c == '.'));
}

Expr*
new_Expr(const char* format, ...) {
  va_list vl;
  va_start(vl, format);
  Expr* result = new_Expr(format, vl);
  va_end(vl);
  return result;
}

Expr*
new_Expr(const char* format, va_list vl) {
  Vec<Expr*> stack;

  for (int i = 0; format[i] != '\0'; i++) {
    if (isIdentifierChar(format[i])) {
      int n = 1;
      while (isIdentifierChar(format[i+n]))
        n++;
      const char* str = asubstr(&format[i], &format[i+n]);
      i += n-1;
      if (!strcmp(str, "TYPE")) {
        BlockStmt* block = toBlockStmt(stack.v[stack.n-1]);
        INT_ASSERT(block);
        block->blockTag = BLOCK_TYPE;
      } else {
        stack.add(new UnresolvedSymExpr(str));
      }
    } else if (format[i] == '\'') {
      int n = 1;
      while (format[i+n] != '\'')
        n++;
      const char* str = asubstr(&format[i+1], &format[i+n]);
      i += n;
      if (format[i+1] == '(') {
        PrimitiveOp* prim = primitives_map.get(str);
        INT_ASSERT(prim);
        stack.add(new CallExpr(prim));
        i++;
      } else {
        stack.add(new SymExpr(new_StringSymbol(str)));
      }
    } else if (format[i] == '%') {
      i++;
      if (format[i] == 'S')
        stack.add(new SymExpr(va_arg(vl, Symbol*)));
      else if (format[i] == 'E')
        stack.add(va_arg(vl, Expr*));
      else
        INT_FATAL("unknown format specifier in new_Expr");
    } else if (format[i] == '(') {
      Expr* expr = stack.pop();
      INT_ASSERT(expr);
      stack.add(new CallExpr(expr));
      if (format[i+1] == ')') // handle empty calls
        i++;
    } else if (format[i] == ',') {
      Expr* expr = stack.pop();
      INT_ASSERT(expr);
      CallExpr* call = toCallExpr(stack.v[stack.n-1]);
      INT_ASSERT(call);
      call->insertAtTail(expr);
    } else if (format[i] == ')') {
      Expr* expr = stack.pop();
      INT_ASSERT(expr);
      CallExpr* call = toCallExpr(stack.v[stack.n-1]);
      INT_ASSERT(call);
      call->insertAtTail(expr);
    } else if (format[i] == '{') {
      stack.add(new BlockStmt());
    } else if (format[i] == ';') {
      Expr* expr = stack.pop();
      INT_ASSERT(expr);
      BlockStmt* block = toBlockStmt(stack.v[stack.n-1]);
      INT_ASSERT(block);
      block->insertAtTail(expr);
    } else if (format[i] == '}') {
      Expr* expr = stack.pop();
      INT_ASSERT(expr);
      BlockStmt* block = toBlockStmt(stack.v[stack.n-1]);
      INT_ASSERT(block);
      block->insertAtTail(expr);
    }
  }

  INT_ASSERT(stack.n == 1);
  return stack.v[0];
}
