#include "reach.h"
#include "../codegen/genname.h"
#include "../type/assemble.h"
#include "../type/lookup.h"
#include "../type/reify.h"
#include "../type/subtype.h"
#include "../../libponyrt/ds/stack.h"
#include "../../libponyrt/mem/pool.h"
#include <stdio.h>
#include <assert.h>

DECLARE_STACK(reachable_method_stack, reachable_method_t);
DEFINE_STACK(reachable_method_stack, reachable_method_t);

static reachable_method_stack_t* reach_method(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* type, const char* name, ast_t* typeargs);

static reachable_method_stack_t* reach_expr(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* ast);

static uint64_t reachable_method_hash(reachable_method_t* m)
{
  return hash_ptr(m->name);
}

static bool reachable_method_cmp(reachable_method_t* a, reachable_method_t* b)
{
  return a->name == b->name;
}

static void reachable_method_free(reachable_method_t* m)
{
  ast_free(m->typeargs);
  ast_free(m->r_fun);
  POOL_FREE(reachable_method_t, m);
}

DEFINE_HASHMAP(reachable_methods, reachable_method_t, reachable_method_hash,
  reachable_method_cmp, pool_alloc_size, pool_free_size, reachable_method_free,
  NULL);

static uint64_t reachable_method_name_hash(reachable_method_name_t* m)
{
  return hash_ptr(m->name);
}

static bool reachable_method_name_cmp(reachable_method_name_t* a,
  reachable_method_name_t* b)
{
  return a->name == b->name;
}

static void reachable_method_name_free(reachable_method_name_t* m)
{
  reachable_methods_destroy(&m->r_methods);
  POOL_FREE(reachable_method_name_t, m);
}

DEFINE_HASHMAP(reachable_method_names, reachable_method_name_t,
  reachable_method_name_hash, reachable_method_name_cmp, pool_alloc_size,
  pool_free_size, reachable_method_name_free, NULL);

static uint64_t reachable_type_hash(reachable_type_t* t)
{
  return hash_ptr(t->name);
}

static bool reachable_type_cmp(reachable_type_t* a, reachable_type_t* b)
{
  return a->name == b->name;
}

static void reachable_type_free(reachable_type_t* t)
{
  ast_free(t->type);
  reachable_method_names_destroy(&t->methods);
  reachable_type_cache_destroy(&t->subtypes);
  POOL_FREE(reachable_type_t, t);
}

DEFINE_HASHMAP(reachable_types, reachable_type_t, reachable_type_hash,
  reachable_type_cmp, pool_alloc_size, pool_free_size, reachable_type_free,
  NULL);

DEFINE_HASHMAP(reachable_type_cache, reachable_type_t, reachable_type_hash,
  reachable_type_cmp, pool_alloc_size, pool_free_size, NULL, NULL);

static reachable_method_stack_t* add_rmethod(reachable_method_stack_t* s,
  reachable_type_t* t, reachable_method_name_t* m, ast_t* typeargs)
{
  const char* name = genname_fun(NULL, m->name, typeargs);
  reachable_method_t m1 = {name, NULL, NULL, 0};
  reachable_method_t* m2 = reachable_methods_get(&m->r_methods, &m1);

  if(m2 == NULL)
  {
    m2 = POOL_ALLOC(reachable_method_t);
    m2->name = name;
    m2->typeargs = ast_dup(typeargs);
    m2->vtable_index = (uint32_t)-1;

    ast_t* fun = lookup(NULL, NULL, t->type, m->name);

    if(typeargs != NULL)
    {
      // Reify the method with its typeargs, if it has any.
      AST_GET_CHILDREN(fun, cap, id, typeparams, params, result, can_error,
        body);

      ast_t* r_fun = reify(fun, typeparams, typeargs);
      ast_free_unattached(fun);
      fun = r_fun;
    }

    m2->r_fun = ast_dup(fun);
    ast_free_unattached(fun);

    reachable_methods_put(&m->r_methods, m2);

    // Put on a stack of reachable methods to trace.
    s = reachable_method_stack_push(s, m2);
  }

  return s;
}

static reachable_method_stack_t* add_method(reachable_method_stack_t* s,
  reachable_type_t* r, const char* name, ast_t* typeargs)
{
  reachable_method_name_t m1;
  m1.name = name;
  reachable_method_name_t* m2 = reachable_method_names_get(&r->methods, &m1);

  if(m2 == NULL)
  {
    m2 = POOL_ALLOC(reachable_method_name_t);
    m2->name = name;
    reachable_methods_init(&m2->r_methods, 0);
    reachable_method_names_put(&r->methods, m2);
    s = add_rmethod(s, r, m2, typeargs);

    // Add to subtypes if we're an interface or trait.
    ast_t* def = (ast_t*)ast_data(r->type);

    switch(ast_id(def))
    {
      case TK_INTERFACE:
      case TK_TRAIT:
      {
        size_t i = HASHMAP_BEGIN;
        reachable_type_t* t;

        while((t = reachable_type_cache_next(&r->subtypes, &i)) != NULL)
          s = add_method(s, t, name, typeargs);
      }

      default: {}
    }
  }

  return s;
}

static reachable_method_stack_t* add_methods_to_type(
  reachable_method_stack_t* s, reachable_type_t* from, reachable_type_t* to)
{
  size_t i = HASHMAP_BEGIN;
  reachable_method_name_t* m;

  while((m = reachable_method_names_next(&from->methods, &i)) != NULL)
  {
    size_t j = HASHMAP_BEGIN;
    reachable_method_t* m2;

    while((m2 = reachable_methods_next(&m->r_methods, &j)) != NULL)
      s = add_method(s, to, m->name, m2->typeargs);
  }

  return s;
}

static reachable_method_stack_t* add_types_to_trait(
  reachable_method_stack_t* s, reachable_types_t* r, reachable_type_t* t)
{
  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t2;

  while((t2 = reachable_types_next(r, &i)) != NULL)
  {
    ast_t* def = (ast_t*)ast_data(t2->type);

    switch(ast_id(def))
    {
      case TK_PRIMITIVE:
      case TK_CLASS:
      case TK_ACTOR:
        if(is_subtype(t2->type, t->type))
        {
          reachable_type_cache_put(&t->subtypes, t2);
          s = add_methods_to_type(s, t, t2);
        }
        break;

      default: {}
    }
  }

  return s;
}

static reachable_method_stack_t* add_traits_to_type(
  reachable_method_stack_t* s, reachable_types_t* r, reachable_type_t* t)
{
  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t2;

  while((t2 = reachable_types_next(r, &i)) != NULL)
  {
    ast_t* def = (ast_t*)ast_data(t2->type);

    switch(ast_id(def))
    {
      case TK_INTERFACE:
      case TK_TRAIT:
        if(is_subtype(t->type, t2->type))
        {
          reachable_type_cache_put(&t->subtypes, t2);
          s = add_methods_to_type(s, t2, t);
        }
        break;

      default: {}
    }
  }

  return s;
}

static reachable_method_stack_t* add_fieldinit(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* type)
{
  ast_t* def = (ast_t*)ast_data(type);
  ast_t* members = ast_childidx(def, 4);
  ast_t* member = ast_child(members);

  while(member != NULL)
  {
    switch(ast_id(member))
    {
      case TK_FVAR:
      case TK_FLET:
      {
        AST_GET_CHILDREN(member, id, ftype, body);

        if(ast_id(body) != TK_NONE)
        {
          ast_t* var = lookup(NULL, NULL, type, ast_name(id));
          body = ast_childidx(var, 2);
          s = reach_expr(s, r, body);
        }
        break;
      }

      default: {}
    }

    member = ast_sibling(member);
  }

  return s;
}

static reachable_method_stack_t* add_type(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* type, const char* name, ast_t* typeargs)
{
  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    case TK_TUPLETYPE:
    {
      assert(name == NULL);
      assert(typeargs == NULL);
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        s = add_type(s, r, child, NULL, NULL);
        child = ast_sibling(child);
      }

      return s;
    }

    case TK_NOMINAL:
      break;

    default:
      assert(0);
      return s;
  }

  const char* type_name = genname_type(type);
  reachable_type_t t1;
  t1.name = type_name;
  reachable_type_t* t2 = reachable_types_get(r, &t1);

  if(t2 == NULL)
  {
    t2 = POOL_ALLOC(reachable_type_t);
    t2->name = type_name;
    t2->type = ast_dup(type);
    reachable_method_names_init(&t2->methods, 0);
    reachable_type_cache_init(&t2->subtypes, 0);
    t2->vtable_size = 0;
    reachable_types_put(r, t2);

    ast_t* def = (ast_t*)ast_data(type);

    switch(ast_id(def))
    {
      case TK_INTERFACE:
      case TK_TRAIT:
        s = add_types_to_trait(s, r, t2);
        break;

      case TK_PRIMITIVE:
      case TK_CLASS:
      case TK_ACTOR:
        s = add_fieldinit(s, r, type);
        s = add_traits_to_type(s, r, t2);
        break;

      default: {}
    }
  }

  const char* notify = stringtab("_event_notify");
  ast_t* find = lookup_try(NULL, NULL, type, notify);

  if(find != NULL)
    s = add_method(s, t2, notify, NULL);

  if(name != NULL)
    s = add_method(s, t2, name, typeargs);

  AST_GET_CHILDREN(type, pkg, id, typeparams);
  ast_t* typeparam = ast_child(typeparams);

  while(typeparam != NULL)
  {
    s = add_type(s, r, typeparam, NULL, NULL);
    typeparam = ast_sibling(typeparam);
  }

  return s;
}

static reachable_method_stack_t* reach_pattern(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* ast)
{
  // TODO: look here for pattern matching on type when looking for interfaces
  // could build a set of fully reified interfaces that get pattern matched
  // later, we could go through all our reified types, and if they are that
  // inteface, we can add the interface to the provides list
  switch(ast_id(ast))
  {
    case TK_DONTCARE:
      break;

    case TK_VAR:
    case TK_LET:
      // TODO: matching type
      break;

    case TK_TUPLE:
    case TK_SEQ:
    {
      ast_t* child = ast_child(ast);

      while(child != NULL)
      {
        s = reach_pattern(s, r, child);
        child = ast_sibling(child);
      }
      break;
    }

    default:
    {
      s = reach_method(s, r, ast_type(ast), stringtab("eq"), NULL);
      s = reach_expr(s, r, ast);
      break;
    }
  }

  return s;
}

static reachable_method_stack_t* reach_call(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* ast)
{
  AST_GET_CHILDREN(ast, positional, named, postfix);
  AST_GET_CHILDREN(postfix, receiver, method);
  ast_t* typeargs = NULL;

  // Dig through function qualification.
  switch(ast_id(receiver))
  {
    case TK_NEWREF:
    case TK_BEREF:
    case TK_FUNREF:
      typeargs = method;
      AST_GET_CHILDREN_NO_DECL(receiver, receiver, method);
      break;

    default: {}
  }

  ast_t* type = ast_type(receiver);
  const char* method_name = ast_name(method);

  return reach_method(s, r, type, method_name, typeargs);
}

static reachable_method_stack_t* reach_ffi(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* ast)
{
  AST_GET_CHILDREN(ast, name, return_typeargs, args, namedargs, question);
  ast_t* return_type = ast_child(return_typeargs);

  return add_type(s, r, return_type, NULL, NULL);
}

static reachable_method_stack_t* reach_expr(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* ast)
{
  // If this is a method call, mark the method as reachable.
  switch(ast_id(ast))
  {
    case TK_TRUE:
    case TK_FALSE:
    case TK_INT:
    case TK_FLOAT:
    case TK_STRING:
    {
      ast_t* type = ast_type(ast);

      if(type != NULL)
        s = reach_method(s, r, type, stringtab("create"), NULL);
      break;
    }

    case TK_CASE:
    {
      AST_GET_CHILDREN(ast, pattern, guard, body);
      s = reach_pattern(s, r, pattern);
      s = reach_expr(s, r, guard);
      return reach_expr(s, r, body);
    }

    case TK_CALL:
      s = reach_call(s, r, ast);
      break;

    case TK_FFICALL:
      s = reach_ffi(s, r, ast);
      break;

    default: {}
  }

  // Traverse all child expressions looking for calls.
  ast_t* child = ast_child(ast);

  while(child != NULL)
  {
    s = reach_expr(s, r, child);
    child = ast_sibling(child);
  }

  return s;
}

static reachable_method_stack_t* reach_body(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* fun)
{
  AST_GET_CHILDREN(fun, cap, id, typeparams, params, result, can_error, body);
  return reach_expr(s, r, body);
}

static reachable_method_stack_t* reach_method(reachable_method_stack_t* s,
  reachable_types_t* r, ast_t* type, const char* name, ast_t* typeargs)
{
  switch(ast_id(type))
  {
    case TK_NOMINAL:
      break;

    case TK_ISECTTYPE:
    {
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        ast_t* find = lookup_try(NULL, NULL, child, name);

        if(find != NULL)
          s = reach_method(s, r, child, name, typeargs);

        child = ast_sibling(child);
      }

      return s;
    }

    default:
      assert(0);
      return NULL;
  }

  return add_type(s, r, type, name, typeargs);
}

reachable_types_t* reach_new()
{
  reachable_types_t* r = POOL_ALLOC(reachable_types_t);
  reachable_types_init(r, 8);
  return r;
}

void reach_free(reachable_types_t* r)
{
  if(r == NULL)
    return;

  reachable_types_destroy(r);
  POOL_FREE(reachable_types_t, r);
}

void reach(reachable_types_t* r, ast_t* type, const char* name,
  ast_t* typeargs)
{
  reachable_method_stack_t* s = reach_method(NULL, r, type, name, typeargs);

  while(s != NULL)
  {
    reachable_method_t* m;
    s = reachable_method_stack_pop(s, &m);
    s = reach_body(s, r, m->r_fun);
  }
}

void reach_dump(reachable_types_t* r)
{
  printf("REACH\n");

  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t;

  while((t = reachable_types_next(r, &i)) != NULL)
  {
    printf("  %s vtable size %d\n", t->name, t->vtable_size);
    size_t j = HASHMAP_BEGIN;
    reachable_method_name_t* m;

    while((m = reachable_method_names_next(&t->methods, &j)) != NULL)
    {
      size_t k = HASHMAP_BEGIN;
      reachable_method_t* p;

      while((p = reachable_methods_next(&m->r_methods, &k)) != NULL)
      {
        printf("    %s vtable index %d (%p)\n", p->name, p->vtable_index, p);
      }
    }
  }
}
