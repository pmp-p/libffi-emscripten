/* -----------------------------------------------------------------------
   ffi.c - Copyright (c) 2018  Brion Vibber

   wasm32/emscripten Foreign Function Interface

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#include <ffi.h>
#include <ffi_common.h>

#include <stdlib.h>
#include <stdint.h>

#include <emscripten/emscripten.h>

#ifdef DEBUG_F
#define LOG_DEBUG(args...)  \
  console.warn(`====LIBFFI(line __LINE__)`, args)
#else
#define LOG_DEBUG(args...)
#endif

#define EM_JS_MACROS(ret, name, args, body...) EM_JS(ret, name, args, body)

#define DEREF_U8(addr, offset) HEAPU8[addr + offset]
#define DEREF_S8(addr, offset) HEAP8[addr + offset]
#define DEREF_U16(addr, offset) HEAPU16[(addr >> 1) + offset]
#define DEREF_S16(addr, offset) HEAP16[(addr >> 1) + offset]
#define DEREF_U32(addr, offset) HEAPU32[(addr >> 2) + offset]
#define DEREF_S32(addr, offset) HEAP32[(addr >> 2) + offset]

#define DEREF_F32(addr, offset) HEAPF32[(addr >> 2) + offset]
#define DEREF_F64(addr, offset) HEAPF64[(addr >> 3) + offset]

#if WASM_BIGINT
// We have HEAPU64 in this case.
#define DEREF_U64(addr, offset) HEAPU64[(addr >> 3) + offset]
#define LOAD_U64(addr,offset) \
    DEREF_U64(addr, offset)

#define STORE_U64(addr, offset, val) \
    (DEREF_U64(addr, offset) = val)

#else
// No BigUint64Array, have to manually split / join lower and upper byte
#define BIGINT_LOWER(x) (Number((x) & BigInt(0xffffffff)) | 0)
#define BIGINT_UPPER(x) (Number((x) >> BigInt(32)) | 0)
#define BIGINT_FROM_PAIR(lower, upper) \
    (BigInt(lower) | (BigInt(upper) << BigInt(32)))

#define LOAD_U64(addr, offset) \
    BIGINT_FROM_PAIR(DEREF_U32(addr, offset*2), DEREF_U32(addr, offset*2 + 1))

#define STORE_U64(addr, offset, val) (  \
  (DEREF_U32(addr, offset*2) = BIGINT_LOWER(val)), \
  (DEREF_U32(addr, offset*2+1) = BIGINT_UPPER(val)) \
)
#endif

#define CIF__ABI(addr) DEREF_U32(addr, 0)
#define CIF__NARGS(addr) DEREF_U32(addr, 1)
#define CIF__ARGTYPES(addr) DEREF_U32(addr, 2)
#define CIF__RTYPE(addr) DEREF_U32(addr, 3)
#define CIF__NFIXEDARGS(addr) DEREF_U32(addr, 6)

#define FFI_TYPE__SIZE(addr) DEREF_U32(addr, 0)
#define FFI_TYPE__ALIGN(addr) DEREF_U16(addr + 4, 0)
#define FFI_TYPE__TYPEID(addr) DEREF_U16(addr + 6, 0)
#define FFI_TYPE__ELEMENTS(addr) DEREF_U32(addr + 8, 0)

#define ALIGN_ADDRESS(addr, align) (addr &= (~((align) - 1)))
#define STACK_ALLOC(stack, size, align) ((stack -= (size)), ALIGN_ADDRESS(stack, align))

// Pyodide needs to redefine this to support fpcast emulation
#define CALL_FUNC_PTR_DEFAULT(func, args...) \
  wasmTable.get(func).apply(null, args)

#ifndef CALL_FUNC_PTR
#if PYODIDE_FPCAST
#define CALL_FUNC_PTR(func, args...) \
  CALL_FUNC_PTR_DEFAULT(dyncallInvokeMap[func] || func, args)
#else
#define CALL_FUNC_PTR CALL_FUNC_PTR_DEFAULT
#endif
#endif

// Experimentation shows Emscripten supports at most 122 Js trampoline args.
#define MAX_ARGS 122

#include <stddef.h>

#define MAX_ALIGN 8
_Static_assert(MAX_ALIGN == __alignof__(max_align_t), "max_align_t must be 8");

#define VARARGS_FLAG 1

ffi_status FFI_HIDDEN
ffi_prep_cif_machdep(ffi_cif *cif)
{
  // This is called after ffi_prep_cif_machdep_var so we need to avoid
  // overwriting cif->nfixedargs.
  if (!(cif->flags & VARARGS_FLAG))
    cif->nfixedargs = cif->nargs;
  if(cif->nargs > MAX_ARGS) {
    return FFI_BAD_TYPEDEF;
  }
  if(cif->rtype->type == FFI_TYPE_COMPLEX){
    return FFI_BAD_TYPEDEF;
  }
  for(int i = 0; i < cif->nargs; i++){
    // If they put the COMPLEX type into a struct we won't notice, but whatever.
    if(cif->arg_types[i]->type == FFI_TYPE_COMPLEX){
      return FFI_BAD_TYPEDEF;
    }
  }
  return FFI_OK;
}

ffi_status FFI_HIDDEN
ffi_prep_cif_machdep_var(ffi_cif *cif, unsigned nfixedargs, unsigned ntotalargs)
{
  cif->flags |= VARARGS_FLAG;
  cif->nfixedargs = nfixedargs;
  // The varargs takes up one extra argument
  if(cif->nfixedargs + 1 > MAX_ARGS){
    return FFI_BAD_TYPEDEF;
  }
  return FFI_OK;
}

/**
 * A Javascript helper function. This takes an argument typ which is a wasm
 * pointer to an ffi_type object. It returns a pair a type and a type id.
 *
 *    - If it is not a struct, return its type and its typeid field.
 *    - If it is a struct of size >= 2, return the type and its typeid (which
 *      will be FFI_TYPE_STRUCT)
 *    - If it is a struct of size 0, return FFI_TYPE_VOID (????? this is broken)
 *    - If it is a struct of size 1, replace it with the single field and apply
 *      the same logic again to that.
 *
 * By always unboxing structs up front, we can avoid messy casework later.
 */
EM_JS_MACROS(
void,
unbox_small_structs, (ffi_type type_ptr), {
  var type_id = FFI_TYPE__TYPEID(type_ptr);
  while (type_id === FFI_TYPE_STRUCT) {
    var elements = FFI_TYPE__ELEMENTS(type_ptr);
    var first_element = DEREF_U32(elements, 0);
    if (first_element === 0) {
      type_id = FFI_TYPE_VOID;
      break;
    } else if (DEREF_U32(elements, 1) === 0) {
      type_ptr = first_element;
      type_id = FFI_TYPE__TYPEID(first_element);
    } else {
      break;
    }
  }
  return [type_ptr, type_id];
})

EM_JS_MACROS(
void,
ffi_call, (ffi_cif * cif, ffi_fp fn, void *rvalue, void **avalue),
{
  var abi = CIF__ABI(cif);
  var nargs = CIF__NARGS(cif);
  var nfixedargs = CIF__NFIXEDARGS(cif);
  var arg_types_ptr = CIF__ARGTYPES(cif);
  var rtype_unboxed = unbox_small_structs(CIF__RTYPE(cif));
  var rtype_ptr = rtype_unboxed[0];
  var rtype_id = rtype_unboxed[1];
  var orig_stack_ptr = stackSave();
  var cur_stack_ptr = orig_stack_ptr;

  var args = [];
  // Does our onwards call return by argument or normally? We return by argument
  // no matter what.
  var ret_by_arg = false;

  if (rtype_id === FFI_TYPE_COMPLEX) {
    throw new Error('complex ret marshalling nyi');
  }
  if (rtype_id < 0 || rtype_id > FFI_TYPE_LAST) {
    throw new Error('Unexpected rtype ' + rtype_id);
  }
  // If the return type is a struct with multiple entries or a long double, the
  // function takes an extra first argument which is a pointer to return value.
  // Conveniently, we've already received a pointer to return value, so we can
  // just use this. We also mark a flag that we don't need to convert the return
  // value of the dynamic call back to C.
  if (rtype_id === FFI_TYPE_LONGDOUBLE || rtype_id === FFI_TYPE_STRUCT) {
    args.push(rvalue);
    ret_by_arg = true;
  }

  // Accumulate a Javascript list of arguments for the Javascript wrapper for
  // the wasm function. The Javascript wrapper does a type conversion from
  // Javascript to C automatically, here we manually do the inverse conversion
  // from C to Javascript.
  for (var i = 0; i < nfixedargs; i++) {
    var arg_ptr = DEREF_U32(avalue, i);
    var arg_unboxed = unbox_small_structs(DEREF_U32(arg_types_ptr, i));
    var arg_type_ptr = arg_unboxed[0];
    var arg_type_id = arg_unboxed[1];

    // It's okay here to always use unsigned integers, when passed into a signed
    // slot of the same size they get interpreted correctly.
    switch (arg_type_id) {
    case FFI_TYPE_INT:
    case FFI_TYPE_SINT32:
    case FFI_TYPE_UINT32:
    case FFI_TYPE_POINTER:
      args.push(DEREF_U32(arg_ptr, 0));
      break;
    case FFI_TYPE_FLOAT:
      args.push(DEREF_F32(arg_ptr, 0));
      break;
    case FFI_TYPE_DOUBLE:
      args.push(DEREF_F64(arg_ptr, 0));
      break;
    case FFI_TYPE_UINT8:
      args.push(DEREF_U8(arg_ptr, 0));
      break;
    case FFI_TYPE_SINT8:
      args.push(DEREF_S8(arg_ptr, 0));
      break;
    case FFI_TYPE_UINT16:
      args.push(DEREF_U16(arg_ptr, 0));
      break;
    case FFI_TYPE_SINT16:
      args.push(DEREF_S16(arg_ptr, 0));
      break;
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
      args.push(LOAD_U64(arg_ptr, 0));
      break;
    case FFI_TYPE_LONGDOUBLE:
      // long double is passed as a pair of BigInts.
      args.push(LOAD_U64(arg_ptr, 0));
      args.push(LOAD_U64(arg_ptr, 1));
      break;
    case FFI_TYPE_STRUCT:
      // Nontrivial structs are passed by pointer.
      // Have to copy the struct onto the stack though because C ABI says it's
      // call by value.
      var size = FFI_TYPE__SIZE(arg_type_ptr);
      var align = FFI_TYPE__ALIGN(arg_type_ptr);
      STACK_ALLOC(cur_stack_ptr, size, align);
      HEAP8.subarray(cur_stack_ptr, cur_stack_ptr+size).set(HEAP8.subarray(arg_ptr, arg_ptr + size));
      args.push(cur_stack_ptr);
      break;
    case FFI_TYPE_COMPLEX:
      throw new Error('complex marshalling nyi');
    default:
      throw new Error('Unexpected type ' + arg_type_id);
    }
  }

  // Wasm functions can't directly manipulate the callstack, so varargs
  // arguments have to go on a separate stack. A varags function takes one extra
  // argument which is a pointer to where on the separate stack the args are
  // located. Because stacks are allocated backwards, we have to loop over the
  // varargs backwards.
  //
  // We don't have any way of knowing how many args were actually passed, so we
  // just always copy extra nonsense past the end. The ownwards call will know
  // not to look at it.
  if (nfixedargs != nargs) {
    var struct_arg_info = [];
    for (var i = nargs - 1;  i >= nfixedargs; i--) {
      var arg_ptr = DEREF_U32(avalue, i);
      var arg_unboxed = unbox_small_structs(DEREF_U32(arg_types_ptr, i));
      var arg_type_ptr = arg_unboxed[0];
      var arg_type_id = arg_unboxed[1];
      switch (arg_type_id) {
      case FFI_TYPE_UINT8:
      case FFI_TYPE_SINT8:
        STACK_ALLOC(cur_stack_ptr, 1, 1);
        DEREF_U8(cur_stack_ptr, 0) = DEREF_U8(arg_ptr, 0);
        break;
      case FFI_TYPE_UINT16:
      case FFI_TYPE_SINT16:
        STACK_ALLOC(cur_stack_ptr, 2, 2);
        DEREF_U16(cur_stack_ptr, 0) = DEREF_U16(arg_ptr, 0);
        break;
      case FFI_TYPE_INT:
      case FFI_TYPE_UINT32:
      case FFI_TYPE_SINT32:
      case FFI_TYPE_POINTER:
      case FFI_TYPE_FLOAT:
        STACK_ALLOC(cur_stack_ptr, 4, 4);
        DEREF_U32(cur_stack_ptr, 0) = DEREF_U32(arg_ptr, 0);
        break;
      case FFI_TYPE_DOUBLE:
      case FFI_TYPE_UINT64:
      case FFI_TYPE_SINT64:
        STACK_ALLOC(cur_stack_ptr, 8, 8);
        DEREF_U32(cur_stack_ptr, 0) = DEREF_U32(arg_ptr, 0);
        DEREF_U32(cur_stack_ptr, 1) = DEREF_U32(arg_ptr, 1);
        break;
      case FFI_TYPE_LONGDOUBLE:
        STACK_ALLOC(cur_stack_ptr, 16, 8);
        DEREF_U32(cur_stack_ptr, 0) = DEREF_U32(arg_ptr, 0);
        DEREF_U32(cur_stack_ptr, 1) = DEREF_U32(arg_ptr, 1);
        DEREF_U32(cur_stack_ptr, 2) = DEREF_U32(arg_ptr, 1);
        DEREF_U32(cur_stack_ptr, 3) = DEREF_U32(arg_ptr, 1);
        break;
      case FFI_TYPE_STRUCT:
        // Again, struct must be passed by pointer.
        // But ABI is by value, so have to copy struct onto stack.
        // Currently arguments are going onto stack so we can't put it there now. Come back for this.
        STACK_ALLOC(cur_stack_ptr, 4, 4);
        struct_arg_info.push([cur_stack_ptr, arg_ptr, FFI_TYPE__SIZE(arg_type_ptr), FFI_TYPE__ALIGN(arg_type_ptr)]);
        break;
      case FFI_TYPE_COMPLEX:
        throw new Error('complex arg marshalling nyi');
      default:
        throw new Error('Unexpected argtype ' + arg_type_id);
      }
    }
    // extra normal argument which is the pointer to the varargs.
    args.push(cur_stack_ptr);
    // TODO: What does this mean?
    // Now allocate variable struct args on stack too.
    for(var i = 0; i < struct_arg_info.length; i++) {
      var struct_info = struct_arg_info[i];
      var arg_target = struct_info[0];
      var arg_ptr = struct_info[1];
      var size = struct_info[2];
      var align = struct_info[3];
      STACK_ALLOC(cur_stack_ptr, size, align);
      HEAP8.subarray(cur_stack_ptr, cur_stack_ptr+size).set(HEAP8.subarray(arg_ptr, arg_ptr + size));
      DEREF_U32(arg_target, 0) = cur_stack_ptr;
    }
  }
  STACK_ALLOC(cur_stack_ptr, 0, MAX_ALIGN);
  stackRestore(cur_stack_ptr);
  LOG_DEBUG("CALL_FUNC_PTR", fn, args);
  var result = CALL_FUNC_PTR(fn, args);
  // Put the stack pointer back (we moved it if we made a varargs call)
  stackRestore(orig_stack_ptr);

  // We need to return by argument. If return value was a nontrivial struct or
  // long double, the onwards call already put the return value in rvalue
  if (ret_by_arg) {
    return;
  }

  // Otherwise the result was automatically converted from C into Javascript and
  // we need to manually convert it back to C.
  switch (rtype_id) {
  case FFI_TYPE_VOID:
    break;
  case FFI_TYPE_INT:
  case FFI_TYPE_UINT32:
  case FFI_TYPE_SINT32:
  case FFI_TYPE_POINTER:
    DEREF_U32(rvalue, 0) = result;
    break;
  case FFI_TYPE_FLOAT:
    DEREF_F32(rvalue, 0) = result;
    break;
  case FFI_TYPE_DOUBLE:
    DEREF_F64(rvalue, 0) = result;
    break;
  case FFI_TYPE_UINT8:
  case FFI_TYPE_SINT8:
    DEREF_U8(rvalue, 0) = result;
    break;
  case FFI_TYPE_UINT16:
  case FFI_TYPE_SINT16:
    DEREF_U16(rvalue, 0) = result;
    break;
  case FFI_TYPE_UINT64:
  case FFI_TYPE_SINT64:
    STORE_U64(rvalue, 0, result);
    break;
  case FFI_TYPE_COMPLEX:
    throw new Error('complex ret marshalling nyi');
  default:
    throw new Error('Unexpected rtype ' + rtype_id);
  }
});

#define CLOSURE__wrapper(addr) DEREF_U32(addr, 0)
#define CLOSURE__cif(addr) DEREF_U32(addr, 1)
#define CLOSURE__fun(addr) DEREF_U32(addr, 2)
#define CLOSURE__user_data(addr) DEREF_U32(addr, 3)

EM_JS_MACROS(void *, ffi_closure_alloc_helper, (size_t size, void **code), {
  var closure = _malloc(size);
  var index = getEmptyTableSlot();
  DEREF_U32(code, 0) = index;
  CLOSURE__wrapper(closure) = index;
  return closure;
})

void * __attribute__ ((visibility ("default")))
ffi_closure_alloc(size_t size, void **code) {
  return ffi_closure_alloc_helper(size, code);
}

EM_JS_MACROS(void, ffi_closure_free_helper, (void *closure), {
  var index = CLOSURE__wrapper(closure);
  freeTableIndexes.push(index);
  _free(closure);
})

void __attribute__ ((visibility ("default")))
ffi_closure_free(void *closure) {
  return ffi_closure_free_helper(closure);
}

EM_JS_MACROS(
ffi_status,
ffi_prep_closure_loc_helper,
(ffi_closure * closure, ffi_cif *cif, void *fun, void *user_data, void *codeloc),
{
  var abi = CIF__ABI(cif);
  var nargs = CIF__NARGS(cif);
  var nfixedargs = CIF__NFIXEDARGS(cif);
  var arg_types_ptr = CIF__ARGTYPES(cif);
  var rtype_unboxed = unbox_small_structs(CIF__RTYPE(cif));
  var rtype_ptr = rtype_unboxed[0];
  var rtype_id = rtype_unboxed[1];

  // First construct the signature of the javascript trampoline we are going to create.
  // Important: this is the signature for calling us, the onward call always has sig viiii.
  var sig;
  var ret_by_arg = false;
  switch (rtype_id) {
  case FFI_TYPE_VOID:
    sig = 'v';
    break;
  case FFI_TYPE_STRUCT:
  case FFI_TYPE_LONGDOUBLE:
    // Return via a first pointer argument.
    sig = 'vi';
    ret_by_arg = true;
    break;
  case FFI_TYPE_INT:
  case FFI_TYPE_UINT8:
  case FFI_TYPE_SINT8:
  case FFI_TYPE_UINT16:
  case FFI_TYPE_SINT16:
  case FFI_TYPE_UINT32:
  case FFI_TYPE_SINT32:
  case FFI_TYPE_POINTER:
    sig = 'i';
    break;
  case FFI_TYPE_FLOAT:
    sig = 'f';
    break;
  case FFI_TYPE_DOUBLE:
    sig = 'd';
    break;
  case FFI_TYPE_UINT64:
  case FFI_TYPE_SINT64:
    sig = 'j';
    break;
  case FFI_TYPE_COMPLEX:
    throw new Error('complex ret marshalling nyi');
  default:
    throw new Error('Unexpected rtype ' + rtype_id);
  }
  var unboxed_arg_type_id_list = [];
  var unboxed_arg_type_info_list = [];
  for (var i = 0; i < nargs; i++) {
    var arg_unboxed = unbox_small_structs(DEREF_U32(arg_types_ptr, i));
    var arg_type_ptr = arg_unboxed[0];
    var arg_type_id = arg_unboxed[1];
    unboxed_arg_type_id_list.push(arg_type_id);
    unboxed_arg_type_info_list.push([FFI_TYPE__SIZE(arg_type_ptr), FFI_TYPE__ALIGN(arg_type_ptr)]);
  }
  for (var i = 0; i < nfixedargs; i++) {
    switch (unboxed_arg_type_id_list[i]) {
    case FFI_TYPE_INT:
    case FFI_TYPE_UINT8:
    case FFI_TYPE_SINT8:
    case FFI_TYPE_UINT16:
    case FFI_TYPE_SINT16:
    case FFI_TYPE_UINT32:
    case FFI_TYPE_SINT32:
    case FFI_TYPE_POINTER:
    case FFI_TYPE_STRUCT:
      sig += 'i';
      break;
    case FFI_TYPE_FLOAT:
      sig += 'f';
      break;
    case FFI_TYPE_DOUBLE:
      sig += 'd';
      break;
    case FFI_TYPE_LONGDOUBLE:
      sig += 'jj';
      break;
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
      sig += 'j';
      break;
    case FFI_TYPE_COMPLEX:
      throw new Error('complex marshalling nyi');
    default:
      throw new Error('Unexpected argtype ' + arg_type_id);
    }
  }
  if (nfixedargs < nargs) {
    // extra pointer to varargs stack
    sig += "i";
  }
  LOG_DEBUG("CREATE_CLOSURE",  "sig:", sig);
  function trampoline() {
    var args = Array.prototype.slice.call(arguments);
    var size = 0;
    var orig_stack_ptr = stackSave();
    var cur_ptr = orig_stack_ptr;
    var ret_ptr;
    var jsarg_idx = 0;
    // Should we return by argument or not? The onwards call returns by argument
    // no matter what. (Warning: ret_by_arg means the opposite in ffi_call)
    if (ret_by_arg) {
      ret_ptr = args[jsarg_idx++];
    } else {
      // We might return 4 bytes or 8 bytes, allocate 8 just in case.
      STACK_ALLOC(cur_ptr, 8, 8);
      ret_ptr = cur_ptr;
    }
    cur_ptr -= 4 * nargs;
    var args_ptr = cur_ptr;
    var carg_idx = -1;
    // Here we either have the actual argument, or a pair of BigInts for long
    // double, or a pointer to struct. We have to store into args_ptr[i] a
    // pointer to the ith argument. If the argument is a struct, just store the
    // pointer. Otherwise allocate stack space and copy the js argument onto the
    // stack.
    for (var carg_idx = 0; carg_idx < nfixedargs; carg_idx++ ) {
      // jsarg_idx might start out as 0 or 1 depending on ret_by_arg
      // it advances an extra time for long double
      var cur_arg = args[jsarg_idx++];
      var arg_type_info = unboxed_arg_type_info_list[carg_idx];
      var arg_size = arg_type_info[0];
      var arg_align = arg_type_info[1];
      var arg_type_id = unboxed_arg_type_id_list[carg_idx];
      switch (arg_type_id) {
      case FFI_TYPE_UINT8:
      case FFI_TYPE_SINT8:
        // Bad things happen if we don't align to 4 here
        STACK_ALLOC(cur_ptr, 1, 4);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        DEREF_U8(cur_ptr, 0) = cur_arg;
        break;
      case FFI_TYPE_UINT16:
      case FFI_TYPE_SINT16:
        // Bad things happen if we don't align to 4 here
        STACK_ALLOC(cur_ptr, 2, 4);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        DEREF_U16(cur_ptr, 0) = cur_arg;
        break;
      case FFI_TYPE_INT:
      case FFI_TYPE_UINT32:
      case FFI_TYPE_SINT32:
      case FFI_TYPE_POINTER:
        STACK_ALLOC(cur_ptr, 4, 4);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        DEREF_U32(cur_ptr, 0) = cur_arg;
        break;
      case FFI_TYPE_STRUCT:
        // cur_arg is already a pointer to struct
        // copy it onto stack to pass by value
        STACK_ALLOC(cur_ptr, arg_size, arg_align);
        HEAP8.subarray(cur_ptr, cur_ptr + arg_size).set(HEAP8.subarray(cur_arg, cur_arg + arg_size));
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        break;
      case FFI_TYPE_FLOAT:
        STACK_ALLOC(cur_ptr, 4, 4);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        DEREF_F32(cur_ptr, 0) = cur_arg;
        break;
      case FFI_TYPE_DOUBLE:
        STACK_ALLOC(cur_ptr, 8, 8);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        DEREF_F64(cur_ptr, 0) = cur_arg;
        break;
      case FFI_TYPE_UINT64:
      case FFI_TYPE_SINT64:
        STACK_ALLOC(cur_ptr, 8, 8);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        STORE_U64(cur_ptr, 0, cur_arg);
        break;
      case FFI_TYPE_LONGDOUBLE:
        STACK_ALLOC(cur_ptr, 16, 8);
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
        STORE_U64(cur_ptr, 0, cur_arg);
        cur_arg = args[jsarg_idx++];
        STORE_U64(cur_ptr, 1, cur_arg);
        break;
      }
    }
    // If its a varargs call, last js argument is a pointer to the varargs.
    var varargs = args[args.length - 1];
    // We have no way of knowing how many varargs were actually provided, this
    // fills the rest of the stack space allocated with nonsense. The onward
    // call will know to ignore the nonsense.

    // We either have a pointer to the argument if the argument is not a struct
    // or a pointer to pointer to struct. We need to store a pointer to the
    // argument into args_ptr[i]
    for (var carg_idx = nfixedargs; carg_idx < nargs; carg_idx++) {
      var arg_type_id = unboxed_arg_type_id_list[carg_idx];
      var arg_type_info = unboxed_arg_type_info_list[carg_idx];
      var arg_size = arg_type_info[0];
      var arg_align = arg_type_info[1];
      if (arg_type_id === FFI_TYPE_STRUCT) {
        // In this case varargs is a pointer to pointer to struct so we need to
        // deref once
        var struct_ptr = DEREF_U32(varargs, 0);
        STACK_ALLOC(cur_ptr, arg_size, arg_align);
        HEAP8.subarray(cur_ptr, cur_ptr + arg_size).set(HEAP8.subarray(struct_ptr, struct_ptr + arg_size));
        DEREF_U32(args_ptr, carg_idx) = cur_ptr;
      } else {
        DEREF_U32(args_ptr, carg_idx) = varargs;
      }
      varargs += 4;
    }
    STACK_ALLOC(cur_ptr, 0, MAX_ALIGN);
    stackRestore(cur_ptr);
    LOG_DEBUG("CALL_CLOSURE",  "closure:", closure, "fptr", CLOSURE__fun(closure), "cif",  CLOSURE__cif(closure));
    CALL_FUNC_PTR(CLOSURE__fun(closure), [
        CLOSURE__cif(closure), ret_ptr, args_ptr,
        CLOSURE__user_data(closure)
    ]);
    stackRestore(orig_stack_ptr);

    // If we aren't supposed to return by argument, figure out what to return.
    if (!ret_by_arg) {
      switch (sig[0]) {
      case "i":
        return DEREF_U32(ret_ptr, 0);
      case "j":
        return LOAD_U64(ret_ptr, 0);
      case "d":
        return DEREF_F64(ret_ptr, 0);
      case "f":
        return DEREF_F32(ret_ptr, 0);
      }
    }
  }
  try {
    var wasm_trampoline = convertJsFunctionToWasm(trampoline, sig);
  } catch(e) {
    return FFI_BAD_TYPEDEF;
  }
  wasmTable.set(codeloc, wasm_trampoline);
  CLOSURE__cif(closure) = cif;
  CLOSURE__fun(closure) = fun;
  CLOSURE__user_data(closure) = user_data;
  return 0 /* FFI_OK */;
})

// EM_JS does not correctly handle function pointer arguments, so we need a
// helper
ffi_status ffi_prep_closure_loc(ffi_closure *closure, ffi_cif *cif,
                                void (*fun)(ffi_cif *, void *, void **, void *),
                                void *user_data, void *codeloc) {
  if (cif->abi != FFI_WASM32_EMSCRIPTEN)
    return FFI_BAD_ABI;
  return ffi_prep_closure_loc_helper(closure, cif, (void *)fun, user_data,
                                     codeloc);
}
