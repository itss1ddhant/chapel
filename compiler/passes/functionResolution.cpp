#include "astutil.h"
#include "build.h"
#include "expr.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"
#include "symscope.h"
#include "runtime.h"
#include "../ifa/prim_data.h"

static char* _init;
static char* _pass;
static char* _copy;
static char* _this;
static char* _assign;

static Expr* preFold(Expr* expr);
static Expr* postFold(Expr* expr);

static FnSymbol* instantiate(FnSymbol* fn, ASTMap* subs);

static void setFieldTypes(FnSymbol* fn);


static Vec<FnSymbol*> resolvedFns;
Vec<CallExpr*> callStack;

static Map<FnSymbol*,Vec<FnSymbol*>*> ddf; // map of functions to
                                           // virtual children

// types contains the types of the actuals
// names contains the name if it is a named argument, otherwise NULL
// e.g.  foo(arg1=12, "hi");
//  types = int, string
//  names = arg1, NULL
enum resolve_call_error_type {
  CALL_NO_ERROR,
  CALL_PARTIAL,
  CALL_AMBIGUOUS,
  CALL_UNKNOWN
};
static resolve_call_error_type resolve_call_error;
static Vec<FnSymbol*> resolve_call_error_candidates;
static FnSymbol* resolve_call(CallExpr* call,
                              char *name,
                              Vec<Type*>* actual_types,
                              Vec<Symbol*>* actual_params,
                              Vec<char*>* actual_names);
static Type* resolve_type_expr(Expr* expr);

static void resolveCall(CallExpr* call);
static void resolveFns(FnSymbol* fn);

static bool canDispatch(Type* actualType,
                        Symbol* actualParam,
                        Type* formalType,
                        FnSymbol* fn = NULL,
                        bool* require_scalar_promotion = NULL);

static void pruneResolvedTree();

static void
resolveFormals(FnSymbol* fn) {
  static Vec<FnSymbol*> done;

  if (!fn->isGeneric) {
    if (done.set_in(fn))
      return;
    done.set_add(fn);

    for_formals(formal, fn) {
      if (formal->defPoint->exprType) {
        formal->type = resolve_type_expr(formal->defPoint->exprType);
        formal->defPoint->exprType->remove();
      }
    }
    if (fn->retExprType) {
      fn->retType = resolve_type_expr(fn->retExprType);
      fn->retExprType->remove();
    }
    if (fn->fnClass == FN_CONSTRUCTOR)
      setFieldTypes(fn);
  }
}

static bool
fits_in_int(int width, Immediate* imm) {
  if (imm->const_kind == NUM_KIND_INT && imm->num_index == INT_SIZE_32) {
    int64 i = imm->int_value();
    switch (width) {
    default: INT_FATAL("bad width in fits_in_int");
    case 8:
      return (i >= -128 && i <= 127);
    case 16:
      return (i >= -32768 && i <= 32767);
    case 32:
      return (i >= -2147483648ll && i <= 2147483647ll);
    case 64:
      return (i >= -9223372036854775807ll-1 && i <= 9223372036854775807ll);
    }
  }
  return false;
}

static bool
fits_in_uint(int width, Immediate* imm) {
  if (imm->const_kind == NUM_KIND_INT && imm->num_index == INT_SIZE_32) {
    int64 i = imm->int_value();
    if (i < 0)
      return false;
    uint64 u = (uint64)i;
    switch (width) {
    default: INT_FATAL("bad width in fits_in_uint");
    case 8:
      return (u <= 255);
    case 16:
      return (u <= 65535);
    case 32:
      return (u <= 2147483647ull);
    case 64:
      return true;
    }
  } else if (imm->const_kind == NUM_KIND_INT && imm->num_index == INT_SIZE_64) {
    int64 i = imm->int_value();
    if (i > 0 && width == 64)
      return true;
  }
  return false;
}


// Returns true iff dispatching the actualType to the formalType
// results in an instantiation.
static bool
canInstantiate(Type* actualType, Type* formalType) {
  if (formalType == dtAny)
    return true;
  if (formalType == dtIntegral && (is_int_type(actualType) || is_uint_type(actualType)))
    return true;
  if (formalType == dtNumeric && (is_int_type(actualType) || is_uint_type(actualType) || is_imag_type(actualType) || is_real_type(actualType) || is_complex_type(actualType)))
    return true;
  if (actualType == formalType)
    return true;
  if (actualType->instantiatedFrom &&
      canInstantiate(actualType->instantiatedFrom, formalType))
    return true;
  return false;
}

// Returns true iff dispatching the actualType to the formalType
// results in a coercion.
static bool
canCoerce(Type* actualType, Symbol* actualParam, Type* formalType) {
  if (actualType->symbol->hasPragma( "synchronization primitive")) {
    if (actualType->isGeneric) {
      return false;
    } else {
      Type *base_type = (Type*)(actualType->substitutions.v[0].value);
      return canDispatch(base_type, actualParam, formalType);
    }
  }

  if (is_int_type(formalType) && dynamic_cast<EnumType*>(actualType)) {
    return true;
  }
  if (is_int_type(formalType)) {
    if (actualType == dtBool)
      return true;
    if (is_int_type(actualType) &&
        get_width(actualType) < get_width(formalType))
      return true;
    if (is_uint_type(actualType) &&
        get_width(actualType) < get_width(formalType))
      return true;
    if (get_width(formalType) < 64)
      if (VarSymbol* var = dynamic_cast<VarSymbol*>(actualParam))
        if (var->immediate)
          if (fits_in_int(get_width(formalType), var->immediate))
            return true;
  }
  if (is_uint_type(formalType)) {
    if (actualType == dtBool)
      return true;
    if (is_uint_type(actualType) &&
        get_width(actualType) < get_width(formalType))
      return true;
    if (VarSymbol* var = dynamic_cast<VarSymbol*>(actualParam))
      if (var->immediate)
        if (fits_in_uint(get_width(formalType), var->immediate))
          return true;
  }
  if (is_real_type(formalType)) {
    if (is_int_type(actualType))
      return true;
    if (is_uint_type(actualType))
      return true;
    if (is_real_type(actualType) && 
        get_width(actualType) < get_width(formalType))
      return true;
  }
  if (is_complex_type(formalType)) {
    if (is_int_type(actualType))
      return true;
    if (is_uint_type(actualType))
      return true;
    if (is_real_type(actualType) && 
        (get_width(actualType) <= get_width(formalType)/2))
      return true;
    if (is_imag_type(actualType) && 
        (get_width(actualType) <= get_width(formalType)/2))
      return true;
    if (is_complex_type(actualType) && 
        (get_width(actualType) < get_width(formalType)))
      return true;
  }
  if (formalType == dtString) {
    if (is_int_type(actualType) || is_uint_type(actualType) || 
        is_real_type(actualType) || is_imag_type(actualType) ||
        is_complex_type(actualType) ||
        actualType == dtBool)
      return true;
  }
  return false;
}

// Returns true iff the actualType can dispatch to the formalType.
// The function symbol is used to avoid scalar promotion on =.
// param is set if the actual is a parameter (compile-time constant).
static bool
canDispatch(Type* actualType, Symbol* actualParam, Type* formalType, FnSymbol* fn, bool* require_scalar_promotion) {
  if (require_scalar_promotion)
    *require_scalar_promotion = false;
  if (actualType == formalType)
    return true;
  if (actualType == dtNil && formalType == dtObject)
    return true;
  if (actualType == dtNil)
    if (ClassType* ct = dynamic_cast<ClassType*>(formalType))
      if (ct->classTag == CLASS_CLASS)
        return true;
  if (canCoerce(actualType, actualParam, formalType))
    return true;
  forv_Vec(Type, parent, actualType->dispatchParents) {
    if (parent == formalType || canDispatch(parent, actualParam, formalType, fn)) {
      return true;
    }
  }
  if (fn &&
      strcmp(fn->name, "=") && 
      actualType->scalarPromotionType && 
      (canDispatch(actualType->scalarPromotionType, actualParam, formalType, fn))) {
    if (require_scalar_promotion)
      *require_scalar_promotion = true;
    return true;
  }
  return false;
}

static bool
isDispatchParent(Type* t, Type* pt) {
  forv_Vec(Type, p, t->dispatchParents)
    if (p == pt || isDispatchParent(p, pt))
      return true;
  return false;
}

static bool
moreSpecific(FnSymbol* fn, Type* actualType, Type* formalType) {
  if (canDispatch(actualType, NULL, formalType, fn))
    return true;
  if (canInstantiate(actualType, formalType)) {
    return true;
  }
  return false;
}

static bool
computeActualFormalMap(FnSymbol* fn,
                       Vec<Type*>* formal_actuals,
                       Vec<Symbol*>* formal_params,
                       Vec<ArgSymbol*>* actual_formals,
                       int num_actuals,
                       int num_formals,
                       Vec<Type*>* actual_types,
                       Vec<Symbol*>* actual_params,
                       Vec<char*>* actual_names) {
  for (int i = 0; i < num_formals; i++) {
    formal_actuals->add(NULL);
    formal_params->add(NULL);
  }
  for (int i = 0; i < num_actuals; i++)
    actual_formals->add(NULL);
  for (int i = 0; i < num_actuals; i++) {
    if (actual_names->v[i]) {
      bool match = false;
      int j = -1;
      for_formals(formal, fn) {
        j++;
        if (!strcmp(actual_names->v[i], formal->name)) {
          match = true;
          actual_formals->v[i] = formal;
          formal_actuals->v[j] = actual_types->v[i];
          formal_params->v[j] = actual_params->v[i];
          if (formal->type == dtSetterToken &&
              actual_types->v[i] != dtSetterToken ||
              formal->type != dtSetterToken &&
              actual_types->v[i] == dtSetterToken)
            return false;
          break;
        }
      }
      if (!match)
        return false;
    }
  }
  for (int i = 0; i < num_actuals; i++) {
    if (!actual_names->v[i]) {
      bool match = false;
      int j = -1;
      for_formals(formal, fn) {
        if (formal->variableExpr)
          return (fn->isGeneric) ? true : false;
        j++;
        if (!formal_actuals->v[j]) {
          match = true;
          actual_formals->v[i] = formal;
          formal_actuals->v[j] = actual_types->v[i];
          formal_params->v[j] = actual_params->v[i];
          if (formal->type == dtSetterToken &&
              actual_types->v[i] != dtSetterToken ||
              formal->type != dtSetterToken &&
              actual_types->v[i] == dtSetterToken)
            return false;
          break;
        }
      }
      if (!match && !fn->isGeneric)
        return false;
    }
  }
  return true;
}


static void
computeGenericSubs(ASTMap &subs,
                   FnSymbol* fn,
                   int num_formals,
                   Vec<Type*>* formal_actuals,
                   Vec<Symbol*>* formal_params) {
  int i = 0;
  for_formals(formal, fn) {
    if (formal->intent == INTENT_PARAM) {
      if (formal_params->v[i] && formal_params->v[i]->isParam()) {
        subs.put(formal, formal_params->v[i]);
      }
    } else if (formal->type->isGeneric) {
      if (formal_actuals->v[i]) {
        if (canInstantiate(formal_actuals->v[i], formal->type)) {
          subs.put(formal, formal_actuals->v[i]);
        }
      } else if (formal->defaultExpr) {
        Type* defaultType = resolve_type_expr(formal->defaultExpr);
        if (canInstantiate(defaultType, formal->type)) {
          subs.put(formal, defaultType);
        }
      }
    }
    i++;
  }
}


static Map<FnSymbol*,Vec<FnSymbol*>*> varArgsCache;

static FnSymbol*
expandVarArgs(FnSymbol* fn, int numActuals) {
  for_formals(arg, fn) {
    if (!fn->isGeneric && arg->variableExpr &&
        !dynamic_cast<DefExpr*>(arg->variableExpr))
      resolve_type_expr(arg->variableExpr);

    // handle unspecified variable number of arguments
    if (DefExpr* def = dynamic_cast<DefExpr*>(arg->variableExpr)) {

      // check for cached stamped out function
      if (Vec<FnSymbol*>* cfns = varArgsCache.get(fn)) {
        forv_Vec(FnSymbol, cfn, *cfns) {
          if (cfn->formals->length() == numActuals)
            return cfn;
        }
      }

      int numCopies = numActuals - fn->formals->length() + 1;
      if (numCopies <= 0)
        return NULL;

      ASTMap map;
      FnSymbol* newFn = fn->copy(&map);
      newFn->visible = false;
      fn->defPoint->insertBefore(new DefExpr(newFn));
      DefExpr* newDef = dynamic_cast<DefExpr*>(map.get(def));
      newDef->replace(new SymExpr(new_IntSymbol(numCopies)));

      ASTMap update;
      update.put(newDef->sym, new_IntSymbol(numCopies));
      update_symbols(newFn, &update);

      // add new function to cache
      Vec<FnSymbol*>* cfns = varArgsCache.get(fn);
      if (!cfns)
        cfns = new Vec<FnSymbol*>();
      cfns->add(newFn);
      varArgsCache.put(fn, cfns);

      return expandVarArgs(newFn, numActuals);
    } else if (SymExpr* sym = dynamic_cast<SymExpr*>(arg->variableExpr)) {

      // handle specified number of variable arguments
      if (VarSymbol* n_var = dynamic_cast<VarSymbol*>(sym->var)) {
        if (n_var->type == dtInt[INT_SIZE_32] && n_var->immediate) {
          int n = n_var->immediate->int_value();
          CallExpr* tupleCall = new CallExpr("_construct__tuple");
          for (int i = 0; i < n; i++) {
            DefExpr* new_arg_def = arg->defPoint->copy();
            ArgSymbol* new_arg = dynamic_cast<ArgSymbol*>(new_arg_def->sym);
            new_arg->variableExpr = NULL;
            tupleCall->insertAtTail(new SymExpr(new_arg));
            new_arg->name = astr("_e", intstring(i), "_", arg->name);
            new_arg->cname = stringcat("_e", intstring(i), "_", arg->cname);
            arg->defPoint->insertBefore(new_arg_def);
          }
          VarSymbol* var = new VarSymbol(arg->name);
          tupleCall->insertAtHead(new_IntSymbol(n));
          fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, var, tupleCall));
          fn->insertAtHead(new DefExpr(var));
          arg->defPoint->remove();
          ASTMap update;
          update.put(arg, var);
          update_symbols(fn, &update);
          normalize(fn);
        }
      }
    } else if (!fn->isGeneric && arg->variableExpr)
      INT_FATAL("bad variableExpr");
  }
  return fn;
}


// Return actual-formal map if FnSymbol is viable candidate to call
static void
addCandidate(Vec<FnSymbol*>* candidateFns,
             Vec<Vec<ArgSymbol*>*>* candidateActualFormals,
             FnSymbol* fn,
             Vec<Type*>* actual_types,
             Vec<Symbol*>* actual_params,
             Vec<char*>* actual_names,
             bool inst = false) {
  fn = expandVarArgs(fn, actual_types->n);

  if (!fn)
    return;

  Vec<ArgSymbol*>* actual_formals = new Vec<ArgSymbol*>();

  int num_actuals = actual_types->n;
  int num_formals = fn->formals ? fn->formals->length() : 0;

  Vec<Type*> formal_actuals;
  Vec<Symbol*> formal_params;
  bool valid = computeActualFormalMap(fn, &formal_actuals, &formal_params,
                                      actual_formals, num_actuals,
                                      num_formals, actual_types,
                                      actual_params, actual_names);
  if (!valid) {
    delete actual_formals;
    return;
  }

  if (fn->isGeneric) {
    ASTMap subs;
    computeGenericSubs(subs, fn, num_formals, &formal_actuals, &formal_params);
    if (subs.n && !fn->isPartialInstantiation(&subs)) {
      FnSymbol* inst_fn = instantiate(fn, &subs);
      if (inst_fn)
        addCandidate(candidateFns, candidateActualFormals, inst_fn, actual_types, actual_params, actual_names, true);
    }
    delete actual_formals;
    return;
  }

  if (fn->isGeneric)
    INT_FATAL(fn, "unexpected generic function");

  resolveFormals(fn);

  int j = -1;
  for_formals(formal, fn) {
    j++;
    if (!strcmp(fn->name, "=")) {
      if (j == 0) {
        if (formal_actuals.v[j] != formal->type)
          return;
      }
    }
    if (formal_actuals.v[j] &&
        !canDispatch(formal_actuals.v[j], formal_params.v[j], formal->type, fn)) {
      delete actual_formals;
      return;
    }
    if (formal_params.v[j] && formal_params.v[j]->isTypeVariable && !formal->isTypeVariable) {
      delete actual_formals;
      return;
   }
//     if (formal_params.v[j] && !formal_params.v[j]->isTypeVariable && formal->isTypeVariable) {
//       delete actual_formals;
//       return;
//     }
    if (!formal_actuals.v[j] && !formal->defaultExpr) {
      delete actual_formals;
      return;
    }
  }
  candidateFns->add(fn);
  candidateActualFormals->add(actual_formals);
}


static FnSymbol*
build_default_wrapper(FnSymbol* fn,
                      Vec<ArgSymbol*>* actual_formals) {
  FnSymbol* wrapper = fn;
  int num_actuals = actual_formals->n;
  int num_formals = fn->formals ? fn->formals->length() : 0;
  if (num_formals > num_actuals) {
    Vec<Symbol*> defaults;
    for_formals(formal, fn) {
      bool used = false;
      forv_Vec(ArgSymbol, arg, *actual_formals) {
        if (arg == formal)
          used = true;
      }
      if (!used)
        defaults.add(formal);
    }
    wrapper = fn->default_wrapper(&defaults);

    // update actual_formals for use in build_order_wrapper
    int j = 1;
    for_formals(formal, fn) {
      for (int i = 0; i < actual_formals->n; i++) {
        if (actual_formals->v[i] == formal) {
          ArgSymbol* newFormal = wrapper->getFormal(j);
          actual_formals->v[i] = newFormal;
          j++;
        }
      }
    }
  }
  return wrapper;
}


static FnSymbol*
build_order_wrapper(FnSymbol* fn,
                    Vec<ArgSymbol*>* actual_formals) {
  bool order_wrapper_required = false;
  Map<Symbol*,Symbol*> formals_to_formals;
  int i = 0;
  for_formals(formal, fn) {
    i++;

    int j = 0;
    forv_Vec(ArgSymbol, af, *actual_formals) {
      j++;
      if (af == formal) {
        if (i != j)
          order_wrapper_required = true;
        formals_to_formals.put(formal, actual_formals->v[i-1]);
      }
    }
  }
  if (order_wrapper_required) {
    fn = fn->order_wrapper(&formals_to_formals);
  }
  return fn;
}


static FnSymbol*
build_coercion_wrapper(FnSymbol* fn,
                       Vec<Type*>* actual_types,
                       Vec<Symbol*>* actual_params) {
  ASTMap subs;
  int j = -1;
  for_formals(formal, fn) {
    j++;
    Type* actual_type = actual_types->v[j];
    Symbol* actual_param = actual_params->v[j];
    if (actual_type != formal->type)
      if (canCoerce(actual_type, actual_param, formal->type) || isDispatchParent(actual_type, formal->type))
        subs.put(formal, actual_type->symbol);
  }
  if (subs.n)
    fn = fn->coercion_wrapper(&subs);
  return fn;  
}


static FnSymbol*
build_promotion_wrapper(FnSymbol* fn,
                        Vec<Type*>* actual_types,
                        Vec<Symbol*>* actual_params,
                        bool isSquare) {
  if (!strcmp(fn->name, "="))
    return fn;
  bool promotion_wrapper_required = false;
  Map<Symbol*,Symbol*> promoted_subs;
  int j = -1;
  for_formals(formal, fn) {
    j++;
    Type* actual_type = actual_types->v[j];
    Symbol* actual_param = actual_params->v[j];
    bool require_scalar_promotion = false;
    if (canDispatch(actual_type, actual_param, formal->type, fn, &require_scalar_promotion)){
      if (require_scalar_promotion) {
        promotion_wrapper_required = true;
        promoted_subs.put(formal, actual_type->symbol);
      }
    }
  }
  if (promotion_wrapper_required)
    fn = fn->promotion_wrapper(&promoted_subs, isSquare);
  return fn;
}


static int
visibility_distance(SymScope* scope, FnSymbol* fn,
                    int d = 1, Vec<SymScope*>* alreadyVisited = NULL) {
  Vec<SymScope*> localAlreadyVisited;

  if (!alreadyVisited)
    alreadyVisited = &localAlreadyVisited;

  if (alreadyVisited->set_in(scope))
    return 0;

  alreadyVisited->set_add(scope);

  if (Symbol* sym = scope->lookupLocal(fn->name)) {
    for (Symbol* tmp = sym; tmp; tmp = tmp->overload) {
      if (tmp == fn)
        return d;
    }
  }

  if (scope->getModuleUses()) {
    forv_Vec(ModuleSymbol, module, *scope->getModuleUses()) {
      int dd = visibility_distance(module->modScope, fn, d, alreadyVisited);
      if (dd > 0)
        return dd;
    }
  }

  if (scope->parent)
    return visibility_distance(scope->parent, fn, d+1, alreadyVisited);

  return 0;
}


static void
disambiguate_by_scope(SymScope* scope, Vec<FnSymbol*>* candidateFns) {
  Vec<int> vds;
  forv_Vec(FnSymbol, fn, *candidateFns) {
    vds.add(visibility_distance(scope, fn));
  }
  int md = 0;
  for (int i = 0; i < vds.n; i++) {
    if (vds.v[i] != 0) {
      if (md) {
        if (vds.v[i] < md)
          md = vds.v[i];
      } else
        md = vds.v[i];
    }
  }
  for (int i = 0; i < vds.n; i++) {
    if (vds.v[i] != md)
      candidateFns->v[i] = 0;
  }
}


static FnSymbol*
disambiguate_by_match(Vec<FnSymbol*>* candidateFns,
                      Vec<Vec<ArgSymbol*>*>* candidateActualFormals,
                      Vec<Type*>* actual_types,
                      Vec<Symbol*>* actual_params,
                      Vec<ArgSymbol*>** ret_afs) {
  FnSymbol* best = NULL;
  Vec<ArgSymbol*>* actual_formals = 0;
  for (int i = 0; i < candidateFns->n; i++) {
    if (candidateFns->v[i]) {
      best = candidateFns->v[i];
      actual_formals = candidateActualFormals->v[i];
      for (int j = 0; j < candidateFns->n; j++) {
        if (i != j && candidateFns->v[j]) {
          bool better = false;
          bool as_good = true;
          Vec<ArgSymbol*>* actual_formals2 = candidateActualFormals->v[j];
          for (int k = 0; k < actual_formals->n; k++) {
            ArgSymbol* arg = actual_formals->v[k];
            ArgSymbol* arg2 = actual_formals2->v[k];
            if (arg->type == arg2->type && arg->instantiatedParam && !arg2->instantiatedParam)
              as_good = false;
            else if (arg->type == arg2->type && !arg->instantiatedParam && arg2->instantiatedParam)
              better = true;
            else {
              bool require_scalar_promotion1;
              bool require_scalar_promotion2;
              canDispatch(actual_types->v[k], actual_params->v[k], arg->type, best, &require_scalar_promotion1);
              canDispatch(actual_types->v[k], actual_params->v[k], arg2->type, best, &require_scalar_promotion2);
              if (require_scalar_promotion1 && !require_scalar_promotion2)
                better = true;
              else if (!require_scalar_promotion1 && require_scalar_promotion2)
                as_good = false;
              else {
                if (arg->instantiatedFrom==dtAny &&
                    arg2->instantiatedFrom!=dtAny) {
                  better = true;
                } else if (arg->instantiatedFrom!=dtAny &&
                           arg2->instantiatedFrom==dtAny) {
                  as_good = false;
                } else {
                  if (actual_types->v[k] == arg2->type &&
                      actual_types->v[k] != arg->type) {
                    better = true;
                  } else if (actual_types->v[k] == arg->type &&
                             actual_types->v[k] != arg2->type) {
                    as_good = false;
                  } else if (moreSpecific(best, arg2->type, arg->type) && 
                      arg2->type != arg->type) {
                    better = true;
                  } else if (moreSpecific(best, arg->type, arg2->type) &&
                      arg2->type != arg->type) {
                    as_good = false;
                  } else if (is_int_type(arg2->type) && is_uint_type(arg->type)) {
                    better = true;
                  } else if (is_int_type(arg->type) && is_uint_type(arg2->type)) {
                    as_good = false;
                  }
                }
              }
            }
          }
          if (better || as_good) {
            best = NULL;
            break;
          }
        }
      }
      if (best)
        break;
    }
  }
  *ret_afs = actual_formals;
  return best;
}


char* call2string(CallExpr* call,
                  char* name,
                  Vec<Type*>& atypes,
                  Vec<Symbol*>& aparams,
                  Vec<char*>& anames) {
  bool method = false;
  bool _this = false;
  char *str = "";
  if (atypes.n > 1)
    if (atypes.v[0] == dtMethodToken)
      method = true;
  if (method) {
    if (aparams.v[1] && aparams.v[1]->isTypeVariable)
      str = stringcat(str, atypes.v[1]->symbol->name, ".");
    else
      str = stringcat(str, ":", atypes.v[1]->symbol->name, ".");
  }
  if (!strcmp("this", name))
    _this = true;
  if (!strncmp("_construct_", name, 11)) {
    str = stringcat(str, name+11);
  } else if (!_this) {
    str = stringcat(str, name);
  }
  if (!call->methodTag)
    str = stringcat(str, "(");
  bool first = false;
  bool setter = false;
  int start = 0;
  if (method)
    start = 2;
  if (_this)
    start = 1;
  for (int i = start; i < atypes.n; i++) {
    if (aparams.v[i] == gSetterToken) {
      str = stringcat(str, ") = ");
      setter = true;
      first = false;
      continue;
    }
    if (!first)
      first = true;
    else
      str = stringcat(str, ", ");
    if (anames.v[i])
      str = stringcat(str, anames.v[i], "=");
    VarSymbol* var = dynamic_cast<VarSymbol*>(aparams.v[i]);
    char buff[512];
    if (aparams.v[i] && aparams.v[i]->isTypeVariable)
      str = stringcat(str, atypes.v[i]->symbol->name);
    else if (var && var->immediate) {
      sprint_imm(buff, *var->immediate);
      str = stringcat(str, buff);
    } else
      str = stringcat(str, ":", atypes.v[i]->symbol->name);
  }
  if (!call->methodTag && !setter)
    str = stringcat(str, ")");
  return str;
}


char* fn2string(FnSymbol* fn) {
  char* str;
  int start = 0;
  if (fn->instantiatedFrom)
    fn = fn->instantiatedFrom;
  if (fn->isMethod) {
    if (!strcmp(fn->name, "this")) {
      str = stringcat(":", fn->getFormal(1)->type->symbol->name);
      start = 1;
    } else {
      str = stringcat(":", fn->getFormal(2)->type->symbol->name, ".", fn->name);
      start = 2;
    }
  } else if (!strncmp("_construct_", fn->name, 11))
    str = stringcat(fn->name+11);
  else
    str = stringcat(fn->name);
  if (!fn->noParens)
    str = stringcat(str, "(");
  bool first = false;
  for (int i = start; i < fn->formals->length(); i++) {
    ArgSymbol* arg = fn->getFormal(i+1);
    if (!first)
      first = true;
    else
      str = stringcat(str, ", ");
    if (arg->intent == INTENT_PARAM)
      str = stringcat(str, "param ");
    if (arg->isTypeVariable)
      str = stringcat(str, "type ", arg->name);
    else if (arg->type == dtUnknown) {
      if (SymExpr* sym = dynamic_cast<SymExpr*>(arg->defPoint->exprType))
        str = stringcat(str, arg->name, ": ", sym->var->name);
      else
        str = stringcat(str, arg->name);
    } else
      str = stringcat(str, arg->name, ": ", arg->type->symbol->name);
  }
  if (!fn->noParens)
    str = stringcat(str, ")");
  return str;
}


static FnSymbol*
resolve_call(CallExpr* call,
             char *name,
             Vec<Type*>* actual_types,
             Vec<Symbol*>* actual_params,
             Vec<char*>* actual_names) {
  Vec<FnSymbol*> visibleFns;                    // visible functions

  Vec<FnSymbol*> candidateFns;
  Vec<Vec<ArgSymbol*>*> candidateActualFormals; // candidate functions

  if (!call->isResolved())
    call->parentScope->getVisibleFunctions(&visibleFns, canonicalize_string(name));
  else
    visibleFns.add(call->isResolved());

  forv_Vec(FnSymbol, visibleFn, visibleFns) {
    if (call->methodTag && !visibleFn->noParens)
      continue;
    addCandidate(&candidateFns, &candidateActualFormals, visibleFn,
                 actual_types, actual_params, actual_names);
  }

  FnSymbol* best = NULL;
  Vec<ArgSymbol*>* actual_formals = 0;
  best = disambiguate_by_match(&candidateFns, &candidateActualFormals,
                               actual_types, actual_params,
                               &actual_formals);

  // use visibility and then look for best again
  if (!best && candidateFns.n > 1) {
    disambiguate_by_scope(call->parentScope, &candidateFns);
    best = disambiguate_by_match(&candidateFns, &candidateActualFormals,
                                 actual_types, actual_params,
                                 &actual_formals);
  }

  if (!best && candidateFns.n > 0) {
    for (int i = 0; i < candidateFns.n; i++) {
      if (candidateFns.v[i]) {
        resolve_call_error_candidates.add(candidateFns.v[i]);
      }
    }
    resolve_call_error = CALL_AMBIGUOUS;
    best = NULL;
  } else if (call->partialTag && (!best || !best->noParens)) {
    resolve_call_error = CALL_PARTIAL;
    best = NULL;
  } else if (!best) {
    for (int i = 0; i < visibleFns.n; i++) {
      if (visibleFns.v[i]) {
        resolve_call_error_candidates.add(visibleFns.v[i]);
      }
    }
    resolve_call_error = CALL_UNKNOWN;
    best = NULL;
  } else {
    best = build_default_wrapper(best, actual_formals);
    best = build_order_wrapper(best, actual_formals);

    FnSymbol* promoted = build_promotion_wrapper(best, actual_types, actual_params, call->square);
    if (promoted != best) {
      if (fWarnPromotion) {
        char* str = call2string(call, name, *actual_types, *actual_params, *actual_names);
        USR_WARN(call, "promotion on %s", str);
      }
      best = promoted;
    }
    best = build_coercion_wrapper(best, actual_types, actual_params);
  }

  for (int i = 0; i < candidateActualFormals.n; i++)
    delete candidateActualFormals.v[i];

  return best;
}

static void
computeActuals(CallExpr* call,
               Vec<Type*>* atypes,
               Vec<Symbol*>* aparams,
               Vec<char*>* anames) {
  for_actuals(actual, call) {
    atypes->add(actual->typeInfo());
    SymExpr* symExpr;
    if (NamedExpr* named = dynamic_cast<NamedExpr*>(actual)) {
      anames->add(named->name);
      symExpr = dynamic_cast<SymExpr*>(named->actual);
    } else {
      anames->add(NULL);
      symExpr = dynamic_cast<SymExpr*>(actual);
    }
    if (symExpr)
      aparams->add(symExpr->var);
    else
      aparams->add(NULL);
  }
}

static Type*
resolve_type_expr(Expr* expr) {
  bool stop = false;
  for_exprs_postorder(e, expr) {
    if (expr == e)
      stop = true;
    e = preFold(e);
    if (CallExpr* call = dynamic_cast<CallExpr*>(e)) {
      if (call->parentSymbol) {
        callStack.add(call);
        resolveCall(call);
        FnSymbol* fn = call->isResolved();
        if (fn && call->parentSymbol) {
          resolveFormals(fn);
          if (call->typeInfo() == dtUnknown)
            resolveFns(fn);
        }
        callStack.pop();
      }
    }
    e = postFold(e);
    if (stop) {
      expr = e;
      break;
    }
  }
  Type* t = expr->typeInfo();
  if (t == dtUnknown)
    INT_FATAL(expr, "Unable to resolve type expression");
  return t;
}


static void
checkUnaryOp(CallExpr* call, Vec<Type*>* atypes, Vec<Symbol*>* aparams) {
  if (call->primitive || call->argList->length() != 1)
    return;
  if (call->isNamed("-")) {
    if (atypes->v[0] == dtUInt[INT_SIZE_64]) {
      USR_FATAL(call, "illegal use of '-' on operand of type %s",
                atypes->v[0]->symbol->name);
    }
  }
}


static void
checkBinaryOp(CallExpr* call, Vec<Type*>* atypes, Vec<Symbol*>* aparams) {
  if (call->primitive || call->argList->length() != 2)
    return;
  if (call->isNamed("+") ||
      call->isNamed("-") ||
      call->isNamed("*") ||
      call->isNamed("/") ||
      call->isNamed("**") ||
      call->isNamed("%") ||
      call->isNamed("&") ||
      call->isNamed("|") ||
      call->isNamed("^") ||
      call->isNamed("==") ||
      call->isNamed("!=") ||
      call->isNamed(">") ||
      call->isNamed("<") ||
      call->isNamed(">=") ||
      call->isNamed("<=")) {
    if ((is_int_type(atypes->v[0]) && atypes->v[1] == dtUInt[INT_SIZE_64]) ||
        (is_int_type(atypes->v[1]) && atypes->v[0] == dtUInt[INT_SIZE_64])) {
      VarSymbol* var;
      if (atypes->v[1] == dtUInt[INT_SIZE_64]) {
        var = dynamic_cast<VarSymbol*>(aparams->v[0]);
      } else {
        var = dynamic_cast<VarSymbol*>(aparams->v[1]);
      }
      if (var && var->immediate && var->immediate->const_kind == NUM_KIND_INT) {
        int64 iconst = var->immediate->int_value();
        if (iconst >= 0)
          return;
      }
      SymExpr* base = dynamic_cast<SymExpr*>(call->baseExpr);
      if (!base)
        INT_FATAL(call, "bad call baseExpr");
      USR_FATAL(call, "illegal use of '%s' on operands of type %s and %s",
                base->var->name, atypes->v[0]->symbol->name,
                atypes->v[1]->symbol->name);
    }
  }
}


static CallExpr*
userCall(CallExpr* call) {
  if (call->getModule()->modtype == MOD_STANDARD) {
    for (int i = callStack.n-1; i >= 0; i--) {
      if (callStack.v[i]->getModule()->modtype != MOD_STANDARD)
        return callStack.v[i];
    }
  }
  return call;
}


static void
makeNoop(CallExpr* call) {
  if (call->baseExpr)
    call->baseExpr->remove();
  while (call->argList->length())
    call->get(1)->remove();
  call->primitive = primitives[PRIMITIVE_NOOP];
}


static void
resolveCall(CallExpr* call) {
  if (!call->primitive) {

    //
    // special case cast of class w/ type variables that is not generic
    //   i.e. type variables are type definitions (have default types)
    //
    if (call->isNamed("_cast")) {
      if (SymExpr* te = dynamic_cast<SymExpr*>(call->get(1))) {
        if (TypeSymbol* ts = dynamic_cast<TypeSymbol*>(te->var)) {
          if (ClassType* ct = dynamic_cast<ClassType*>(ts->type)) {
            if (ct->classTag == CLASS_CLASS && ct->isGeneric) {
              CallExpr* cc = new CallExpr(ct->defaultConstructor->name);
              te->replace(cc);
              resolveCall(cc);
              cc->replace(new SymExpr(cc->typeInfo()->symbol));
            }
          }
        }
      }
    }

    if (SymExpr* sym = dynamic_cast<SymExpr*>(call->baseExpr)) {
      if (dynamic_cast<VarSymbol*>(sym->var) ||
          dynamic_cast<ArgSymbol*>(sym->var)) {
        Expr* base = call->baseExpr;
        base->replace(new SymExpr("this"));
        call->insertAtHead(base);
      }
    }

    if (CallExpr* base = dynamic_cast<CallExpr*>(call->baseExpr)) {
      if (base->partialTag) {
        for_actuals_backward(actual, base) {
          actual->remove();
          call->insertAtHead(actual);
        }
        base->replace(base->baseExpr->remove());
      } else {
        VarSymbol* this_temp = new VarSymbol("this_temp");
        this_temp->isCompilerTemp = true;
        this_temp->canReference = true;
        base->replace(new SymExpr("this"));
        CallExpr* move = new CallExpr(PRIMITIVE_MOVE, this_temp, base);
        call->insertAtHead(new SymExpr(this_temp));
        call->getStmtExpr()->insertBefore(new DefExpr(this_temp));
        call->getStmtExpr()->insertBefore(move);
        resolveCall(move);
      }
    }

    Vec<Type*> atypes;
    Vec<Symbol*> aparams;
    Vec<char*> anames;
    computeActuals(call, &atypes, &aparams, &anames);

    checkUnaryOp(call, &atypes, &aparams);
    checkBinaryOp(call, &atypes, &aparams);


    // automatically replace calls with iterator arg with calls to _to_seq
    // if (SymExpr *se = dynamic_cast<SymExpr*>(call->baseExpr)) {
    // se->var
    if (dynamic_cast<SymExpr*>(call->baseExpr)) {
      if (!(call->isNamed( "_to_seq") ||
            call->isNamed( "_copy") ||
            //            call->isNamed( "=") ||
            call->isNamed( "_cast") ||
            call->isNamed( "_init") ||
            call->isNamed( "_pass") ||
            call->isNamed( "getNextCursor") ||
            call->isNamed( "getHeadCursor") ||
            call->isNamed( "getValue") ||
            call->isNamed( "isValidCursor?"))) {
        ASTMap subs;
        int    pos = 0;
        forv_Vec( Type, argtype, atypes) {
          ClassType *ct = dynamic_cast<ClassType*>( argtype);
          if (ct && ct->isIterator) {  // replace use with call to _to_seq
            // YAH, skip if method on self
            if (pos==1 && (atypes.v[0] == dtMethodToken))
              continue;

            VarSymbol *temp = new VarSymbol( stringcat( stringcat( "_to_seq_temp", intstring( call->id)), stringcat( "_", intstring( pos))));
            call->getStmtExpr()->insertBefore( new DefExpr( temp));
            subs.put( aparams.v[pos], temp);
            CallExpr  *toseq = new CallExpr( "_to_seq", 
                                             aparams.v[pos]);
            CallExpr  *toseqass = new CallExpr( PRIMITIVE_MOVE,
                                                temp,
                                                toseq);
            // CallExpr  *toseqass = new CallExpr( "=", temp, toseq);
            call->getStmtExpr()->insertBefore( toseqass);
            resolveCall( toseq);
            resolveFns( toseq->isResolved());
            resolveCall( toseqass);
          }
          pos++;
        }

        if (subs.n > 0) {
          update_symbols( call, &subs);
          resolveCall( call);
          return;
        }
      }
    }
    
    SymExpr* base = dynamic_cast<SymExpr*>(call->baseExpr);
    char* name = base->var->name;
    FnSymbol* resolvedFn = resolve_call(call, name, &atypes, &aparams, &anames);
    if (call->partialTag) {
      if (!resolvedFn)
        return;
      call->partialTag = false;
    }
    if (resolvedFn && resolvedFn->hasPragma("data set error")) {
      Type* elt_type = dynamic_cast<Type*>(resolvedFn->getFormal(1)->type->substitutions.v[0].value);
      if (!elt_type)
        INT_FATAL(call, "Unexpected substitution of ddata class");
      USR_FATAL(userCall(call), "type mismatch in assignment from %s to %s",
                atypes.v[3]->symbol->name, elt_type->symbol->name);
    }
    if (!resolvedFn) {
      if (resolve_call_error == CALL_UNKNOWN || resolve_call_error == CALL_AMBIGUOUS) {
        if (!strcmp("=", name)) {
          if (atypes.v[1] == dtNil) {
            USR_FATAL(userCall(call), "type mismatch in assignment of nil to %s",
                      atypes.v[0]->symbol->name);
          } else {
            USR_FATAL(userCall(call), "type mismatch in assignment from %s to %s",
                      atypes.v[1]->symbol->name,
                      atypes.v[0]->symbol->name);
          }
        } else if (!strcmp("this", name)) {
          USR_FATAL_CONT(userCall(call), "%s access of '%s' by '%s'",
                         (resolve_call_error == CALL_AMBIGUOUS) ? "ambiguous" : "unresolved",
                         atypes.v[0]->symbol->name,
                         call2string(call, name, atypes, aparams, anames));
          USR_STOP();
        } else {
          char* str = call2string(call, name, atypes, aparams, anames);
          USR_FATAL_CONT(userCall(call), "%s call '%s'",
                         (resolve_call_error == CALL_AMBIGUOUS) ? "ambiguous" : "unresolved",
                         str);
          if (resolve_call_error_candidates.n > 0) {
            if (developer) {
              for (int i = callStack.n-1; i>=0; i--) {
                CallExpr* cs = callStack.v[i];
                FnSymbol* f = cs->getFunction();
                if (f->instantiatedFrom)
                  USR_PRINT(callStack.v[i], "  instantiated from %s", f->name);
                else
                  break;
              }
            }
            bool printed_one = false;
            forv_Vec(FnSymbol, fn, resolve_call_error_candidates) {
              if (fn->isSetter) 
                continue;
              if (!developer && fn->getModule()->modtype == MOD_STANDARD)
                continue;
              USR_PRINT(fn, "%s %s",
                        printed_one ? "               " : "candidates are:",
                        fn2string(fn));
              printed_one = true;
            }
          }
          USR_STOP();
        }
      } else {
        INT_FATAL(call, "Error in resolve_call");
      }
    }
    if (call->parentSymbol) {
      call->baseExpr->replace(new SymExpr(resolvedFn));
    }
  } else if (call->isPrimitive(PRIMITIVE_TUPLE_EXPAND)) {
    SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1));
    Symbol* var = dynamic_cast<Symbol*>(sym->var);
    int size = 0;
    for (int i = 0; i < var->type->substitutions.n; i++) {
      if (var->type->substitutions.v[i].key) {
        if (!strcmp("size", dynamic_cast<Symbol*>(var->type->substitutions.v[i].key)->name)) {
          size = dynamic_cast<VarSymbol*>(var->type->substitutions.v[i].value)->immediate->int_value();
          break;
        }
      }
    }
    if (size == 0)
      INT_FATAL(call, "Invalid tuple expand primitive");
    CallExpr* noop = new CallExpr(PRIMITIVE_NOOP);
    call->getStmtExpr()->insertBefore(noop);
    for (int i = 1; i <= size; i++) {
      VarSymbol* tmp = new VarSymbol("_expand_temp");
      DefExpr* def = new DefExpr(tmp);
      call->getStmtExpr()->insertBefore(def);
      CallExpr* e = new CallExpr(sym->copy(), new_IntSymbol(i));
      CallExpr* move = new CallExpr(PRIMITIVE_MOVE, tmp, e);
      call->getStmtExpr()->insertBefore(move);
      call->insertBefore(new SymExpr(tmp));
    }
    call->remove();
    noop->replace(call); // put call back in ast for function resolution
    makeNoop(call);
  } else if (call->isPrimitive(PRIMITIVE_CAST)) {
    Type* t= call->get(1)->typeInfo();
    if (t == dtUnknown)
      INT_FATAL(call, "Unable to resolve type");
    if (t->scalarPromotionType) {
      // ignore for now to handle translation of A op= B of arrays
      // should be an error in general
      //   can't cast to an array type or a sequence type, ...
      Expr* castee = call->get(2);
      castee->remove();
      call->replace(castee);
    } else {
      call->get(1)->replace(new SymExpr(t->symbol));
    }
  } else if (call->isPrimitive(PRIMITIVE_SET_MEMBER)) {
    SymExpr* sym = dynamic_cast<SymExpr*>(call->get(2));
    if (!sym)
      INT_FATAL(call, "bad set member primitive");
    VarSymbol* var = dynamic_cast<VarSymbol*>(sym->var);
    if (!var || !var->immediate)
      INT_FATAL(call, "bad set member primitive");
    char* name = var->immediate->v_string;
    ClassType* ct = dynamic_cast<ClassType*>(call->get(1)->typeInfo());
    if (!ct)
      INT_FATAL(call, "bad set member primitive");
    bool found = false;
    for_fields(field, ct) {
      if (!strcmp(field->name, name)) {
        Type* t = call->get(3)->typeInfo();
        if (t == dtUnknown)
          INT_FATAL(call, "Unable to resolve field type");
        if (t != field->type && t != dtNil && t != dtObject)
          USR_FATAL(userCall(call), "cannot assign expression of type %s to field of type %s", t->symbol->name, field->type->symbol->name);
        found = true;
      }
    }
    if (!found)
      INT_FATAL(call, "bad set member primitive");
  } else if (call->isPrimitive(PRIMITIVE_MOVE)) {
    if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {
      Type* t = call->get(2)->typeInfo();
      if (sym->var->type == dtUnknown)
        sym->var->type = t;
      if (sym->var->type == dtNil)
        sym->var->type = t;
      if (t == dtVoid) {
        USR_FATAL(call->get(2), "illegal use of function that does not return a value");
      }
      if (t == dtUnknown) {
        if (CallExpr* rhs = dynamic_cast<CallExpr*>(call->get(2))) {
          if (FnSymbol* rhsfn = rhs->isResolved()) {
            USR_FATAL_CONT(rhsfn, "unable to resolve return type of function '%s'", rhsfn->name);
            USR_FATAL(rhs, "called recursively at this point");
          }
        }
      }
      if (call->get(2)->isRef() && sym->var->canReference) {
        sym->var->isReference = true;
        call->primitive = primitives[PRIMITIVE_REF];
      }
      if (sym->var->isReference && !strncmp(sym->var->name, "_ret_", 5))
        call->primitive = primitives[PRIMITIVE_REF];
      if (t == dtUnknown)
        INT_FATAL(call, "Unable to resolve type");

      // do not resolve function return type yet
      if (FnSymbol* fn = dynamic_cast<FnSymbol*>(call->parentSymbol)) {
        if (ReturnStmt* last = dynamic_cast<ReturnStmt*>(fn->body->body->last())) {
          if (SymExpr* ret = dynamic_cast<SymExpr*>(last->expr)) {
            if (ret->var == sym->var) {
              if (ret->var->isCompilerTemp)
                ret->var->type = dtUnknown;
              return;
            }
          }
        }
      }

      ClassType* ct = dynamic_cast<ClassType*>(sym->var->type);
      if (t == dtNil && sym->var->type != dtNil && (!ct || ct->classTag != CLASS_CLASS))
        USR_FATAL(userCall(call), "type mismatch in assignment from nil to %s",
                  sym->var->type->symbol->name);
      if (t != dtNil && t != sym->var->type && !isDispatchParent(t, sym->var->type))
        USR_FATAL(userCall(call), "type mismatch in assignment from %s to %s",
                  t->symbol->name, sym->var->type->symbol->name);
      if (t != sym->var->type && isDispatchParent(t, sym->var->type)) {
        Expr* rhs = call->get(2);
        rhs->remove();
        call->insertAtTail(new CallExpr(PRIMITIVE_CAST, sym->var->type->symbol, rhs));
      }
    }
  }
}

static bool
formalRequiresTemp(ArgSymbol* formal) {
  if (formal->intent == INTENT_PARAM ||
      formal->intent == INTENT_TYPE ||
      formal->intent == INTENT_REF ||
      formal->name == _this ||
      formal->isTypeVariable ||
      formal->instantiatedParam ||
      formal->type == dtSetterToken ||
      formal->type == dtMethodToken)
    return false;
  return true;
}

static void
insertFormalTemps(FnSymbol* fn) {
  if (fn->name == _pass || fn->name == _init ||
      fn->name == _assign || fn->name == _copy)
    return;
  ASTMap formals2vars;
  for_formals(formal, fn) {
    if (formalRequiresTemp(formal)) {
      VarSymbol* tmp = new VarSymbol(stringcat("_formal_tmp_", formal->name));
      if ((formal->intent == INTENT_BLANK ||
           formal->intent == INTENT_CONST) &&
          !formal->type->symbol->hasPragma("array"))
        tmp->consClass = VAR_CONST;
      tmp->isCompilerTemp = true;
      formals2vars.put(formal, tmp);
    }
  }
  if (formals2vars.n > 0) {
    update_symbols(fn, &formals2vars);
    Vec<BaseAST*> formals;
    formals2vars.get_keys(formals);
    forv_Vec(BaseAST, ast, formals) {
      ArgSymbol* formal = dynamic_cast<ArgSymbol*>(ast);
      VarSymbol* tmp = dynamic_cast<VarSymbol*>(formals2vars.get(formal));

      // hack for constant assignment checking
      // remove when constant checking is improved
      fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, tmp));

      if (formal->intent == INTENT_OUT) {
        if (formal->defaultExpr && formal->defaultExpr->typeInfo() != dtNil)
          fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, formal->defaultExpr->copy()));
        else
          fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, new CallExpr("_init", formal)));
      } else if (formal->intent == INTENT_INOUT || formal->intent == INTENT_IN)
        fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, new CallExpr("_copy", formal)));
      else
        fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, tmp, new CallExpr("_pass", formal)));
      fn->insertAtHead(new DefExpr(tmp));
      if (formal->intent == INTENT_INOUT || formal->intent == INTENT_OUT) {
        formal->intent = INTENT_REF;
        ReturnStmt* last = dynamic_cast<ReturnStmt*>(fn->body->body->last());
        last->insertBefore(new CallExpr(PRIMITIVE_MOVE, formal, new CallExpr("=", formal, tmp)));
      }
    }
  }
}

static Map<Symbol*,Symbol*> paramMap;

static bool
isType(Expr* expr) {
  if (SymExpr* sym = dynamic_cast<SymExpr*>(expr)) {
    if (sym->var->isTypeVariable)
      return true;
    if (dynamic_cast<TypeSymbol*>(sym->var))
      return true;
  } else if (CallExpr* call = dynamic_cast<CallExpr*>(expr)) {
    if (call->isPrimitive(PRIMITIVE_TYPEOF))
      return true;
  }
  return false;
}

static void fold_param_for(CallExpr* loop) {
  BlockStmt* block = dynamic_cast<BlockStmt*>(loop->next);
  if (!block || block->blockTag != BLOCK_PARAM_FOR)
    INT_FATAL(loop, "bad param loop primitive");
  if (loop && loop->isPrimitive(PRIMITIVE_LOOP_PARAM)) {
    if (SymExpr* lse = dynamic_cast<SymExpr*>(loop->get(2))) {
      if (SymExpr* hse = dynamic_cast<SymExpr*>(loop->get(3))) {
        if (SymExpr* sse = dynamic_cast<SymExpr*>(loop->get(4))) {
          if (VarSymbol* lvar = dynamic_cast<VarSymbol*>(lse->var)) {
            if (VarSymbol* hvar = dynamic_cast<VarSymbol*>(hse->var)) {
              if (VarSymbol* svar = dynamic_cast<VarSymbol*>(sse->var)) {
                if (lvar->immediate && hvar->immediate && svar->immediate) {
                  int64 low = lvar->immediate->int_value();
                  int64 high = hvar->immediate->int_value();
                  int64 stride = svar->immediate->int_value();
                  Expr* index_expr = loop->get(1);
                  block->blockTag = BLOCK_NORMAL;
                  Symbol* index = dynamic_cast<SymExpr*>(index_expr)->var;
                  if (stride <= 0)
                    INT_FATAL("fix this");
                  for (int i = low; i <= high; i += stride) {
                    ASTMap map;
                    map.put(index, new_IntSymbol(i));
                    block->insertBefore(block->copy(&map));
                  }
                  block->remove();
                }
              }
            }
          }
        }
      }
    }
  }
}

static Expr* fold_cond_stmt(CondStmt* if_stmt) {
  Expr* result = NULL;
  if (SymExpr* cond = dynamic_cast<SymExpr*>(if_stmt->condExpr)) {
    if (VarSymbol* var = dynamic_cast<VarSymbol*>(cond->var)) {
      if (var->immediate &&
          var->immediate->const_kind == NUM_KIND_UINT &&
          var->immediate->num_index == INT_SIZE_1) {
        result = new CallExpr(PRIMITIVE_NOOP);
        if_stmt->insertBefore(result);
        if (var->immediate->v_bool == gTrue->immediate->v_bool) {
          Expr* then_stmt = if_stmt->thenStmt;
          then_stmt->remove();
          if_stmt->replace(then_stmt);
        } else if (var->immediate->v_bool == gFalse->immediate->v_bool) {
          Expr* else_stmt = if_stmt->elseStmt;
          if (else_stmt) {
            else_stmt->remove();
            if_stmt->replace(else_stmt);
          } else {
            if_stmt->remove();
          }
        }
      }
    }
  }
  return result;
}

static Expr*
preFold(Expr* expr) {
  Expr* result = expr;
  if (CallExpr* call = dynamic_cast<CallExpr*>(expr)) {
    if (SymExpr* sym = dynamic_cast<SymExpr*>(call->baseExpr)) {
      if (TypeSymbol* type = dynamic_cast<TypeSymbol*>(sym->var)) {
        if (call->argList->length() == 1) {
          if (SymExpr* arg = dynamic_cast<SymExpr*>(call->get(1))) {
            if (VarSymbol* var = dynamic_cast<VarSymbol*>(arg->var)) {
              if (var->immediate) {
                if (NUM_KIND_INT == var->immediate->const_kind ||
                    NUM_KIND_UINT == var->immediate->const_kind) {
                  int size;
                  if (NUM_KIND_INT == var->immediate->const_kind) {
                    size = var->immediate->int_value();
                  } else {
                    size = (int)var->immediate->uint_value();
                  }
                  TypeSymbol* tsize;
                  if (type == dtInt[INT_SIZE_32]->symbol) {
                    switch (size) {
                    case 8: tsize = dtInt[INT_SIZE_8]->symbol; break;
                    case 16: tsize = dtInt[INT_SIZE_16]->symbol; break;
                    case 32: tsize = dtInt[INT_SIZE_32]->symbol; break;
                    case 64: tsize = dtInt[INT_SIZE_64]->symbol; break;
                    default:
                      USR_FATAL( call, "illegal size %d for int", size);
                    }
                    result = new SymExpr(tsize);
                    call->replace(result);
                  } else if (type == dtUInt[INT_SIZE_32]->symbol) {
                    switch (size) {
                    case  8: tsize = dtUInt[INT_SIZE_8]->symbol;  break;
                    case 16: tsize = dtUInt[INT_SIZE_16]->symbol; break;
                    case 32: tsize = dtUInt[INT_SIZE_32]->symbol; break;
                    case 64: tsize = dtUInt[INT_SIZE_64]->symbol; break;
                    default:
                      USR_FATAL( call, "illegal size %d for uint", size);
                    }
                    result = new SymExpr(tsize);
                    call->replace(result);
                  } else if (type == dtReal[FLOAT_SIZE_64]->symbol) {
                    switch (size) {
                    case 32:  tsize = dtReal[FLOAT_SIZE_32]->symbol;  break;
                    case 64:  tsize = dtReal[FLOAT_SIZE_64]->symbol;  break;
                    case 128: tsize = dtReal[FLOAT_SIZE_128]->symbol; break;
                    default:
                      USR_FATAL( call, "illegal size %d for imag", size);
                    }
                    result = new SymExpr(tsize);
                    call->replace(result);
                  } else if (type == dtImag[FLOAT_SIZE_64]->symbol) {
                    switch (size) {
                    case 32:  tsize = dtImag[FLOAT_SIZE_32]->symbol;  break;
                    case 64:  tsize = dtImag[FLOAT_SIZE_64]->symbol;  break;
                    case 128: tsize = dtImag[FLOAT_SIZE_128]->symbol; break;
                    default:
                      USR_FATAL( call, "illegal size %d for imag", size);
                    }
                    result = new SymExpr(tsize);
                    call->replace(result);
                  } else if (type == dtComplex[COMPLEX_SIZE_128]->symbol) {
                    switch (size) {
                    case 64:  tsize = dtComplex[COMPLEX_SIZE_64]->symbol;  break;
                    case 128: tsize = dtComplex[COMPLEX_SIZE_128]->symbol; break;
                    case 256: tsize = dtComplex[COMPLEX_SIZE_256]->symbol; break;
                    default:
                      USR_FATAL( call, "illegal size %d for complex", size);
                    }
                    result = new SymExpr(tsize);
                    call->replace(result);
                  }
                }
              }
            }
          }
        }
      }
    }

    if (call->argList->length() == 2) {
      if (SymExpr* symExpr = dynamic_cast<SymExpr*>(call->get(1))) {
        if (symExpr->var == gMethodToken) {
          Type* type = call->get(2)->typeInfo();
          Vec<BaseAST*> keys;
          type->substitutions.get_keys(keys);
          forv_Vec(BaseAST, key, keys) {
            if (Symbol* var = dynamic_cast<Symbol*>(key)) {
              if (call->isNamed(var->name)) {
                if (Symbol* value = dynamic_cast<Symbol*>(type->substitutions.get(key))) {
                  result = new SymExpr(value);
                  call->replace(result);
                } else if (Type* value = dynamic_cast<Type*>(type->substitutions.get(key))) {
                  if (var->isTypeVariable) {
                    result = new SymExpr(value->symbol);
                    call->replace(result);
                  }
                }
              }
            } else if (Type* var = dynamic_cast<Type*>(key)) {
              INT_FATAL("type key encountered");
              if (call->isNamed(var->symbol->name)) {
                if (Type* value = dynamic_cast<Type*>(type->substitutions.get(key))) {
                  result = new SymExpr(value->symbol);
                  call->replace(result);
                }
              }
            }
          }
        }
      }
    }

    if (call->isNamed("_init")) {
      if (CallExpr* construct = dynamic_cast<CallExpr*>(call->get(1))) {
        if (construct->isNamed("_build_array_type") ||
            construct->isNamed("_build_sparse_domain_type") ||
            construct->isNamed("_build_domain_type") ||
            construct->isNamed("_build_index_type")) {
          result = construct->remove();
          call->replace(result);
        } else if (FnSymbol* fn = dynamic_cast<FnSymbol*>(construct->isResolved())) {
          if (ClassType* ct = dynamic_cast<ClassType*>(fn->retType)) {
            if (!ct->isGeneric) {
              if (ct->defaultValue)
                result = new CallExpr("_cast", fn->retType->symbol, gNil);
              else
                result = construct->remove();
              call->replace(result);
            }
          }
        }
      } else if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {
        TypeSymbol* ts = dynamic_cast<TypeSymbol*>(sym->var);
        if (!ts && sym->var->isTypeVariable)
          ts = sym->var->type->symbol;
        if (ts) {
          if (ts->type->defaultValue)
            result = new CallExpr("_cast", ts, ts->type->defaultValue);
          else if (ts->type->defaultConstructor)
            result = new CallExpr(ts->type->defaultConstructor);
          else
            INT_FATAL(ts, "type has neither defaultValue nor defaultConstructor");
          call->replace(result);
        }
      }
    } else if (call->isNamed("_copy")) {
      if (call->argList->length() == 1) {
        if (SymExpr* symExpr = dynamic_cast<SymExpr*>(call->get(1))) {
          if (VarSymbol* var = dynamic_cast<VarSymbol*>(symExpr->var)) {
            if (var->immediate) {
              result = new SymExpr(var);
              call->replace(result);
            }
          }
        }
      }
    } else if (call->isNamed("_cast")) {
      if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(2))) {
        if (VarSymbol* var = dynamic_cast<VarSymbol*>(sym->var)) {
          if (var->immediate) {
            if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {
              TypeSymbol* ts = sym->var->type->symbol;
              if (!is_imag_type(ts->type) && 
                  !is_complex_type(ts->type) && ts->type != dtString) {
                VarSymbol* typevar = dynamic_cast<VarSymbol*>(ts->type->defaultValue);
                if (!typevar || !typevar->immediate)
                  INT_FATAL("unexpected case in cast_fold");
                Immediate coerce = *typevar->immediate;
                coerce_immediate(var->immediate, &coerce);
                result = new SymExpr(new_ImmediateSymbol(&coerce));
                call->replace(result);
              }
            }
          }
        }
      }
    } else if (call->isNamed("==")) {
      if (isType(call->get(1)) || isType(call->get(2))) {
        Type* lt = call->get(1)->typeInfo();
        Type* rt = call->get(2)->typeInfo();
        if (lt != dtUnknown && rt != dtUnknown &&
            !lt->isGeneric && !rt->isGeneric) {
          result = (lt == rt) ? new SymExpr(gTrue) : new SymExpr(gFalse);
          call->replace(result);
        }
      }
    } else if (call->isNamed("!=")) {
      if (isType(call->get(1)) || isType(call->get(2))) {
        Type* lt = call->get(1)->typeInfo();
        Type* rt = call->get(2)->typeInfo();
        if (lt != dtUnknown && rt != dtUnknown &&
            !lt->isGeneric && !rt->isGeneric) {
          result = (lt != rt) ? new SymExpr(gTrue) : new SymExpr(gFalse);
          call->replace(result);
        }
      }
    } else if (call->isNamed("_construct__tuple") && !call->isResolved()) {
      if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {
        if (VarSymbol* var = dynamic_cast<VarSymbol*>(sym->var)) {
          if (var->immediate) {
            int rank = var->immediate->int_value();
            if (rank != call->argList->length() - 1) {
              if (call->argList->length() != 2)
                INT_FATAL(call, "bad homogeneous tuple");
              Expr* actual = call->get(2);
              for (int i = 1; i < rank; i++) {
                call->insertAtTail(actual->copy());
              }
            }
          }
        }
      }
    } else if (call->isPrimitive(PRIMITIVE_LOOP_PARAM)) {
      fold_param_for(call);
      makeNoop(call);
    }
  }
  return result;
}

#define FOLD_CALL1(prim)                                                \
  if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {            \
    if (VarSymbol* lhs = dynamic_cast<VarSymbol*>(sym->var)) {          \
      if (lhs->immediate) {                                             \
        Immediate i3;                                                   \
        fold_constant(prim, lhs->immediate, NULL, &i3);                 \
        result = new SymExpr(new_ImmediateSymbol(&i3));                 \
        call->replace(result);                                          \
      }                                                                 \
    }                                                                   \
  }

#define FOLD_CALL2(prim)                                                \
  if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {            \
    if (VarSymbol* lhs = dynamic_cast<VarSymbol*>(sym->var)) {          \
      if (lhs->immediate) {                                             \
        if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(2))) {      \
          if (VarSymbol* rhs = dynamic_cast<VarSymbol*>(sym->var)) {    \
            if (rhs->immediate) {                                       \
              Immediate i3;                                             \
              fold_constant(prim, lhs->immediate, rhs->immediate, &i3); \
              result = new SymExpr(new_ImmediateSymbol(&i3));           \
              call->replace(result);                                    \
            }                                                           \
          }                                                             \
        }                                                               \
      }                                                                 \
    }                                                                   \
  }

static bool
isSubType(Type* sub, Type* super) {
  if (sub == super)
    return true;
  forv_Vec(Type, parent, sub->dispatchParents) {
    if (isSubType(parent, super))
      return true;
  }
  return false;
}

static Expr*
postFold(Expr* expr) {
  Expr* result = expr;
  if (CallExpr* call = dynamic_cast<CallExpr*>(expr)) {
    if (FnSymbol* fn = call->isResolved()) {
      if (fn->isParam) {
        VarSymbol* ret = dynamic_cast<VarSymbol*>(fn->getReturnSymbol());
        if (ret->immediate) {
          result = new SymExpr(ret);
          expr->replace(result);
        } else {
          USR_FATAL(call, "param function does not resolve to a param symbol");
        }
      }
    } else if (call->isPrimitive(PRIMITIVE_MOVE)) {
      bool set = false;
      if (SymExpr* lhs = dynamic_cast<SymExpr*>(call->get(1))) {
        if (lhs->var->canParam || lhs->var->isParam()) {
          if (paramMap.get(lhs->var))
            INT_FATAL(call, "parameter set multiple times");
          if (SymExpr* rhs = dynamic_cast<SymExpr*>(call->get(2))) {
            if (VarSymbol* rhsVar = dynamic_cast<VarSymbol*>(rhs->var)) {
              if (rhsVar->immediate) {
                paramMap.put(lhs->var, rhsVar);
                lhs->var->defPoint->remove();
                makeNoop(call);
                set = true;
              }
            }
          }
          if (!set && lhs->var->isParam())
            USR_FATAL(call, "Initializing parameter '%s' to value not known at compile time", lhs->var->name);
        }
        if (!set && lhs->var->canType) {
          if (SymExpr* rhs = dynamic_cast<SymExpr*>(call->get(2))) {
            if (rhs->var->isTypeVariable)
              lhs->var->isTypeVariable = true;
          }
        }
      }
    } else if (call->isPrimitive(PRIMITIVE_GET_MEMBER)) {
      Type* baseType = call->get(1)->typeInfo();
      char* memberName = get_string(call->get(2));
      Symbol* sym = baseType->getField(memberName);
      if (sym->isParam()) {
        Vec<BaseAST*> keys;
        baseType->substitutions.get_keys(keys);
        forv_Vec(BaseAST, key, keys) {
          if (Symbol* var = dynamic_cast<Symbol*>(key)) {
            if (!strcmp(sym->name, var->name)) {
              if (Symbol* value = dynamic_cast<Symbol*>(baseType->substitutions.get(key))) {
                result = new SymExpr(value);
                call->replace(result);
              }
            }
          }
        }
      }
    } else if (call->isPrimitive(PRIMITIVE_ISSUBTYPE)) {
      if (isType(call->get(1)) || isType(call->get(2))) {
        Type* lt = call->get(2)->typeInfo(); // a:t cast is cast(t,a)
        Type* rt = call->get(1)->typeInfo();
        if (lt != dtUnknown && rt != dtUnknown && lt != dtAny &&
            rt != dtAny && !lt->isGeneric) {
          bool is_true = false;
          if (lt->instantiatedFrom == rt)
            is_true = true;
          if (isSubType(lt, rt))
            is_true = true;
          result = (is_true) ? new SymExpr(gTrue) : new SymExpr(gFalse);
          call->replace(result);
        }
      }
    } else if (call->isPrimitive(PRIMITIVE_UNARY_MINUS)) {
      FOLD_CALL1(P_prim_minus);
    } else if (call->isPrimitive(PRIMITIVE_UNARY_PLUS)) {
      FOLD_CALL1(P_prim_plus);
    } else if (call->isPrimitive(PRIMITIVE_UNARY_NOT)) {
      FOLD_CALL1(P_prim_not);
    } else if (call->isPrimitive(PRIMITIVE_UNARY_LNOT)) {
      FOLD_CALL1(P_prim_lnot);
    } else if (call->isPrimitive(PRIMITIVE_ADD)) {
      FOLD_CALL2(P_prim_add);
    } else if (call->isPrimitive(PRIMITIVE_SUBTRACT)) {
      FOLD_CALL2(P_prim_subtract);
    } else if (call->isPrimitive(PRIMITIVE_MULT)) {
      FOLD_CALL2(P_prim_mult);
    } else if (call->isPrimitive(PRIMITIVE_DIV)) {
      FOLD_CALL2(P_prim_div);
    } else if (call->isPrimitive(PRIMITIVE_MOD)) {
      FOLD_CALL2(P_prim_mod);
    } else if (call->isPrimitive(PRIMITIVE_EQUAL)) {
      FOLD_CALL2(P_prim_equal);
    } else if (call->isPrimitive(PRIMITIVE_NOTEQUAL)) {
      FOLD_CALL2(P_prim_notequal);
    } else if (call->isPrimitive(PRIMITIVE_LESSOREQUAL)) {
      FOLD_CALL2(P_prim_lessorequal);
    } else if (call->isPrimitive(PRIMITIVE_GREATEROREQUAL)) {
      FOLD_CALL2(P_prim_greaterorequal);
    } else if (call->isPrimitive(PRIMITIVE_LESS)) {
      FOLD_CALL2(P_prim_less);
    } else if (call->isPrimitive(PRIMITIVE_GREATER)) {
      FOLD_CALL2(P_prim_greater);
    } else if (call->isPrimitive(PRIMITIVE_AND)) {
      FOLD_CALL2(P_prim_and);
    } else if (call->isPrimitive(PRIMITIVE_OR)) {
      FOLD_CALL2(P_prim_or);
    } else if (call->isPrimitive(PRIMITIVE_XOR)) {
      FOLD_CALL2(P_prim_xor);
    } else if (call->isPrimitive(PRIMITIVE_POW)) {
      FOLD_CALL2(P_prim_pow);
    } else if (call->isPrimitive(PRIMITIVE_LSH)) {
      FOLD_CALL2(P_prim_lsh);
    } else if (call->isPrimitive(PRIMITIVE_RSH)) {
      FOLD_CALL2(P_prim_rsh);
    }
  } else if (SymExpr* sym = dynamic_cast<SymExpr*>(expr)) {
    if (paramMap.get(sym->var))
      sym->var = paramMap.get(sym->var);
  }
  if (CondStmt* cond = dynamic_cast<CondStmt*>(result->parentExpr)) {
    if (cond->condExpr == result) {
      if (Expr* expr = fold_cond_stmt(cond)) {
        result = expr;
      }
    }
  }
  return result;
}

static void
resolveBody(Expr* body) {
  for_exprs_postorder(expr, body) {
    expr = preFold(expr);
    if (CallExpr* call = dynamic_cast<CallExpr*>(expr)) {
      if (call->isPrimitive(PRIMITIVE_ERROR)) {
        CallExpr* from;
        for (int i = callStack.n-1; i >= 0; i--) {
          from = callStack.v[i];
          if (from->lineno > 0)
            break;
        }
        USR_FATAL(from, "%s", get_string(call->get(1)));
      }
      callStack.add(call);
      resolveCall(call);
      if (call->isResolved())
        resolveFns(call->isResolved());
      callStack.pop();
    } else if (SymExpr* sym = dynamic_cast<SymExpr*>(expr)) {
      if (ClassType* ct = dynamic_cast<ClassType*>(sym->var->type)) {
        if (!ct->isGeneric) {
          resolveFormals(ct->defaultConstructor);
          resolveFns(ct->defaultConstructor);
        }
      }
    }
    expr = postFold(expr);
  }
}

static void
resolveFns(FnSymbol* fn) {
  if (resolvedFns.set_in(fn))
    return;
  resolvedFns.set_add(fn);

  insertFormalTemps(fn);

  resolveBody(fn->body);

  Symbol* ret = fn->getReturnSymbol();
  Type* retType = ret->type;

  Vec<Type*> retTypes;
  Vec<Symbol*> retParams;

  for_exprs_postorder(expr, fn->body) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(expr)) {
      if (call->isPrimitive(PRIMITIVE_MOVE) ||
          call->isPrimitive(PRIMITIVE_REF)) {
        if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(1))) {
          if (sym->var == ret) {
            if (SymExpr* sym = dynamic_cast<SymExpr*>(call->get(2)))
              retParams.add(sym->var);
            else
              retParams.add(NULL);
            retTypes.add(call->get(2)->typeInfo());
          }
        }
      }
    }
  }

  if (ret->isReference)
    fn->retRef = true;

  if (retType == dtUnknown) {
    if (retTypes.n == 1)
      retType = retTypes.v[0];
    if (retTypes.n > 1) {
      for (int i = 0; i < retTypes.n; i++) {
        bool best = true;
        for (int j = 0; j < retTypes.n; j++) {
          if (retTypes.v[i] != retTypes.v[j]) {
            if (!canCoerce(retTypes.v[j], retParams.v[j], retTypes.v[i]))
              best = false;
          }
        }
        if (best) {
          retType = retTypes.v[i];
          break;
        }
      }
    }
  }

  ret->type = retType;
  if (fn->retType == dtUnknown)
    fn->retType = retType;
  if (retType == dtUnknown)
    INT_FATAL(fn, "Unable to resolve return type");

  if (fn->fnClass == FN_CONSTRUCTOR) {
    forv_Vec(Type, parent, fn->retType->dispatchParents) {
      if (dynamic_cast<ClassType*>(parent) && parent != dtValue && parent != dtObject && parent->defaultConstructor) {
        resolveFormals(parent->defaultConstructor);
        resolveFns(parent->defaultConstructor);
      }
    }
    if (ClassType* ct = dynamic_cast<ClassType*>(fn->retType)) {
      for_fields(field, ct) {
        if (ClassType* fct = dynamic_cast<ClassType*>(field->type)) {
          if (fct->defaultConstructor) {
            resolveFormals(fct->defaultConstructor);
            resolveFns(fct->defaultConstructor);
          }
        }
      }
    }
  }
}


static bool
possible_signature_match(FnSymbol* fn, FnSymbol* gn) {
  if (fn->name != gn->name)
    return false;
  if (fn->formals->length() != gn->formals->length())
    return false;
  for (int i = 3; i <= fn->formals->length(); i++) {
    ArgSymbol* fa = fn->getFormal(i);
    ArgSymbol* ga = gn->getFormal(i);
    if (strcmp(fa->name, ga->name))
      return false;
  }
  return true;
}


static bool
signature_match(FnSymbol* fn, FnSymbol* gn) {
  if (fn->name != gn->name)
    return false;
  if (fn->formals->length() != gn->formals->length())
    return false;
  for (int i = 3; i <= fn->formals->length(); i++) {
    ArgSymbol* fa = fn->getFormal(i);
    ArgSymbol* ga = gn->getFormal(i);
    if (strcmp(fa->name, ga->name))
      return false;
    if (fa->type != ga->type)
      return false;
  }
  return true;
}


static void
add_to_ddf(FnSymbol* pfn, ClassType* pt, ClassType* ct) {
  forv_Vec(FnSymbol, cfn, ct->methods) {
    if (cfn && possible_signature_match(pfn, cfn)) {
      if (ct->isGeneric) {
        ASTMap subs;
        forv_Vec(FnSymbol, cons, *ct->defaultConstructor->instantiatedTo) {
          subs.put(cfn->getFormal(2), cons->retType);
          for (int i = 3; i <= cfn->formals->length(); i++) {
            ArgSymbol* arg = cfn->getFormal(i);
            if (arg->intent == INTENT_PARAM) {
              INT_FATAL(arg, "unhandled case");
            } else if (arg->type->isGeneric) {
              if (!pfn->getFormal(i))
              subs.put(arg, pfn->getFormal(i)->type);
            }
          }
          FnSymbol* icfn = instantiate(cfn, &subs);
          resolveFormals(icfn);
          if (signature_match(pfn, icfn)) {
            resolveFns(icfn);
            Vec<FnSymbol*>* fns = ddf.get(pfn);
            if (!fns) fns = new Vec<FnSymbol*>();
            fns->add(icfn);
            ddf.put(pfn, fns);
          }
        }
      } else {
        ASTMap subs;
        for (int i = 3; i <= cfn->formals->length(); i++) {
          ArgSymbol* arg = cfn->getFormal(i);
          if (arg->intent == INTENT_PARAM) {
            INT_FATAL(arg, "unhandled case");
          } else if (arg->type->isGeneric) {
            subs.put(arg, pfn->getFormal(i)->type);
          }
        }
        if (subs.n)
          cfn = instantiate(cfn, &subs);
        resolveFormals(cfn);
        if (signature_match(pfn, cfn)) {
          resolveFns(cfn);
          Vec<FnSymbol*>* fns = ddf.get(pfn);
          if (!fns) fns = new Vec<FnSymbol*>();
          fns->add(cfn);
          ddf.put(pfn, fns);
        }
      }
    }
  }
}


static void
add_all_children_ddf_help(FnSymbol* fn, ClassType* pt, ClassType* ct) {
  if (ct->defaultConstructor->instantiatedTo ||
      resolvedFns.set_in(ct->defaultConstructor))
    add_to_ddf(fn, pt, ct);
  forv_Vec(Type, t, ct->dispatchChildren) {
    ClassType* ct = dynamic_cast<ClassType*>(t);
    if (!ct->instantiatedFrom)
      add_all_children_ddf_help(fn, pt, ct);
  }
}


static void
add_all_children_ddf(FnSymbol* fn, ClassType* pt) {
  forv_Vec(Type, t, pt->dispatchChildren) {
    ClassType* ct = dynamic_cast<ClassType*>(t);
    if (!ct->instantiatedFrom)
      add_all_children_ddf_help(fn, pt, ct);
  }
}


static void
build_ddf() {
  forv_Vec(FnSymbol, fn, gFns) {
    if (fn->isWrapper || !resolvedFns.set_in(fn))
      continue;
    if (fn->formals->length() > 1) {
      if (fn->getFormal(1)->type == dtMethodToken) {
        if (ClassType* pt = dynamic_cast<ClassType*>(fn->getFormal(2)->type)) {
          if (pt->classTag == CLASS_CLASS && !pt->isGeneric) {
            add_all_children_ddf(fn, pt);
          }
        }
      }
    }
  }
}


void
resolve() {
  _init = astr("_init");
  _pass = astr("_pass");
  _copy = astr("_copy");
  _this = astr("this");
  _assign = astr("=");

  resolveFns(chpl_main);

  int num_types;
  do {
    num_types = gTypes.n;
    Vec<FnSymbol*> keys;
    ddf.get_keys(keys);
    forv_Vec(FnSymbol, key, keys) {
      delete ddf.get(key);
    }
    ddf.clear();
    build_ddf();
  } while (num_types != gTypes.n);

  if (fPrintDispatch) {
    printf("dynamic dispatch functions:\n");
    for (int i = 0; i < ddf.n; i++) {
      if (ddf.v[i].key) {
        printf("  %s\n", fn2string(ddf.v[i].key));
        for (int j = 0; j < ddf.v[i].value->n; j++) {
          printf("    %s\n", fn2string(ddf.v[i].value->v[j]));
        }
      }
    }
  }

  Vec<CallExpr*> calls;
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(ast))
      calls.add(call);
  }
  forv_Vec(CallExpr, call, calls) {
    if (FnSymbol* key = call->isResolved()) {
      if (Vec<FnSymbol*>* fns = ddf.get(key)) {
        forv_Vec(FnSymbol, fn, *fns) {
          Type* type = fn->getFormal(2)->type;
          CallExpr* subcall = call->copy();
          SymExpr* tmp = new SymExpr(gNil);
          FnSymbol* if_fn = build_if_expr(new CallExpr(PRIMITIVE_GETCID,
                                                       call->get(2)->copy(),
                                                       new_IntSymbol(type->id)),
                                          subcall, tmp);
          if_fn->retRef = false;
          if_fn->buildSetter = false;
          if_fn->retType = key->retType;
          if (key->retType == dtUnknown)
            INT_FATAL(call, "bad parent virtual function return type");
          call->getStmtExpr()->insertBefore(new DefExpr(if_fn));
          call->replace(new CallExpr(if_fn));
          tmp->replace(call);
          subcall->baseExpr->replace(new SymExpr(fn));
          if (SymExpr* se = dynamic_cast<SymExpr*>(subcall->get(2)))
            se->replace(new CallExpr(PRIMITIVE_CAST, type->symbol, se->var));
          else if (CallExpr* ce = dynamic_cast<CallExpr*>(subcall->get(2)))
            if (ce->isPrimitive(PRIMITIVE_CAST))
              ce->get(1)->replace(new SymExpr(type->symbol));
            else
              INT_FATAL(subcall, "unexpected");
          else
            INT_FATAL(subcall, "unexpected");
          normalize(if_fn);
          resolvedFns.set_add(if_fn);
        }
      }
    }
  }

  Vec<FnSymbol*> keys;
  ddf.get_keys(keys);
  forv_Vec(FnSymbol, key, keys) {
    delete ddf.get(key);
  }

  pruneResolvedTree();
}


//
// pruneResolvedTree -- prunes and cleans the AST after all of the
// function calls and types have been resolved
//
static void
pruneResolvedTree() {
  // Remove unused functions
  forv_Vec(FnSymbol, fn, gFns) {
    if (fn->defPoint && fn->defPoint->parentSymbol) {
      if (!resolvedFns.set_in(fn) || fn->isParam)
        fn->defPoint->remove();
    }
  }

  // Remove unused types
  forv_Vec(TypeSymbol, type, gTypes) {
    if (type->defPoint && type->defPoint->parentSymbol)
      if (ClassType* ct = dynamic_cast<ClassType*>(type->type))
        if (!resolvedFns.set_in(ct->defaultConstructor))
          ct->symbol->defPoint->remove();
  }

  Vec<BaseAST*> asts;
  collect_asts_postorder(&asts);
  forv_Vec(BaseAST, ast, asts) {
    if (CallExpr* call = dynamic_cast<CallExpr*>(ast)) {
      if (call->isPrimitive(PRIMITIVE_TYPEOF)) {
        // Replace PRIMITIVE_TYPEOF with argument
        call->replace(call->get(1)->remove());
      } else if (call->isPrimitive(PRIMITIVE_SET_MEMBER) ||
                 call->isPrimitive(PRIMITIVE_GET_MEMBER)) {
        // Remove member accesses of types
        // Replace string literals with field symbols in member primitives
        Type* baseType = call->get(1)->typeInfo();
        char* memberName = get_string(call->get(2));
        Symbol* sym = baseType->getField(memberName);
        if (sym->isTypeVariable && call->isPrimitive(PRIMITIVE_GET_MEMBER)) {
          if (sym->type->defaultValue)
            call->replace(new SymExpr(sym->type->defaultValue));
          else
            call->replace(new CallExpr(sym->type->defaultConstructor));
        } else if (sym->isTypeVariable)
          call->remove();
        else
          call->get(2)->replace(new SymExpr(sym));
      } else if (call->isNamed("_init")) {
        // Special handling of array constructors via array pragma
        if (CallExpr* construct = dynamic_cast<CallExpr*>(call->get(1))) {
          if (FnSymbol* fn = construct->isResolved()) {
            if (ClassType* ct = dynamic_cast<ClassType*>(fn->retType)) {
              if (!ct->symbol->hasPragma("array") && ct->defaultValue) {
                call->replace(new CallExpr(PRIMITIVE_CAST, ct->symbol, gNil));
              } else if (!ct->symbol->hasPragma("array")) {
                call->replace(construct->remove());
              }
            }
          }
        }
      } else if (FnSymbol* fn = call->isResolved()) {
        // Remove method and setter token actuals
        for (int i = fn->formals->length(); i >= 1; i--) {
          ArgSymbol* formal = fn->getFormal(i);
          if (formal->type == dtMethodToken ||
              formal->type == dtSetterToken ||
              formal->isTypeVariable)
            call->get(i)->remove();
        }
      }
    } else if (NamedExpr* named = dynamic_cast<NamedExpr*>(ast)) {
      // Remove names of named actuals
      Expr* actual = named->actual;
      actual->remove();
      named->replace(actual);
    } else if (BlockStmt* block = dynamic_cast<BlockStmt*>(ast)) {
      // Remove type blocks--code that exists only to determine types
      if (block->blockTag == BLOCK_TYPE)
        block->remove();
    }
  }

  forv_Vec(FnSymbol, fn, gFns) {
    if (fn->defPoint && fn->defPoint->parentSymbol) {
      for_formals(formal, fn) {
        // Remove formal default values
        if (formal->defaultExpr)
          formal->defaultExpr->remove();
        // Remove formal type expressions
        if (formal->defPoint->exprType)
          formal->defPoint->exprType->remove();
        // Remove method and setter token formals
        if (formal->type == dtMethodToken ||
            formal->type == dtSetterToken)
          formal->defPoint->remove();
        if (formal->isTypeVariable) {
          formal->defPoint->remove();
          VarSymbol* tmp = new VarSymbol("_removed_formal_tmp", formal->type);
          tmp->isCompilerTemp = true;
          fn->insertAtHead(new DefExpr(tmp));
          ASTMap map;
          map.put(formal, tmp);
          update_symbols(fn->body, &map);
        }
      }
    }
  }

  // Remove type fields
  forv_Vec(TypeSymbol, type, gTypes) {
    if (type->defPoint && type->defPoint->parentSymbol) {
      if (ClassType* ct = dynamic_cast<ClassType*>(type->type)) {
        for_fields(field, ct) {
          if (field->isTypeVariable) {
            field->defPoint->remove();
          }
        }
      }
    }
  }
}


static bool
is_array_type(Type* type) {
  forv_Vec(Type, t, type->dispatchParents) {
    if (t->symbol->hasPragma("abase"))
      return true;
    else if (is_array_type(t))
      return true;
  }
  return false;
}


static void
fixTypeNames(ClassType* ct) {
  if (is_array_type(ct)) {
    char* domain_type = ct->getField(4)->type->symbol->name;
    char* elt_type = ct->getField(1)->type->symbol->name;
    ct->symbol->defPoint->parentScope->undefine(ct->symbol);
    ct->symbol->name = astr("[", domain_type, "] ", elt_type);
    ct->symbol->defPoint->parentScope->define(ct->symbol);
  }
  if (ct->instantiatedFrom &&
      !strcmp(ct->instantiatedFrom->symbol->name, "_adomain")) {
    ct->symbol->defPoint->parentScope->undefine(ct->symbol);
    ct->symbol->name = astr(ct->symbol->name+2);
    ct->symbol->defPoint->parentScope->define(ct->symbol);
  }
  if (ct->symbol->hasPragma("array") || ct->symbol->hasPragma("domain")) {
    char* name = ct->getField(1)->type->symbol->name;
    ct->symbol->defPoint->parentScope->undefine(ct->symbol);
    ct->symbol->name = name;
    ct->symbol->defPoint->parentScope->define(ct->symbol);
  }
}


static void
setFieldTypes(FnSymbol* fn) {
  ClassType* ct = dynamic_cast<ClassType*>(fn->retType);
  if (!ct)
    INT_FATAL(fn, "Constructor has no class type");
  for_formals(formal, fn) {
    Type* t = formal->type;
    if (t == dtUnknown && formal->defPoint->exprType)
      t = formal->defPoint->exprType->typeInfo();
    if (t == dtUnknown)
      INT_FATAL(formal, "Unable to resolve field type");
    if (t == dtNil)
      USR_FATAL(formal, "unable to determine type of field from nil");
    bool found = false;
    for_fields(field, ct) {
      if (!strcmp(field->name, formal->name)) {
        field->type = t;
        found = true;
      }
    }
    if (!found)
      INT_FATAL(formal, "Unable to find field in constructor");
  }
  fixTypeNames(ct);
}


static FnSymbol*
instantiate(FnSymbol* fn, ASTMap* subs) {
  FnSymbol* ifn = fn->instantiate_generic(subs);
  if (!ifn->isGeneric && ifn->where) {
    resolveBody(ifn->where);
    normalize(ifn->where); // temporary call to normalize until parameter folding is fully folded
    SymExpr* symExpr = dynamic_cast<SymExpr*>(ifn->where->body->last());
    if (!symExpr)
      USR_FATAL(ifn->where, "Illegal where clause");
    if (!strcmp(symExpr->var->name, "false"))
      return NULL;
    if (strcmp(symExpr->var->name, "true"))
      USR_FATAL(ifn->where, "Illegal where clause");
  }
  return ifn;
}
