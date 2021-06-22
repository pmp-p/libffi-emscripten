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
#include <stdint.h>
#include <stdlib.h>

#include <emscripten/emscripten.h>

ffi_status FFI_HIDDEN
ffi_prep_cif_machdep(ffi_cif *cif)
{
  return FFI_OK;
}

#define EM_JS_MACROS(ret, name, args, body) EM_JS(ret, name, args, body)

#define DEREF_U16(addr, offset) HEAPU16[(addr >> 1) + offset]

#define DEREF_U32(addr, offset) HEAPU32[(addr >> 2) + offset]
#define DEREF_I32(addr, offset) HEAP32[(addr >> 2) + offset]
#define DEREF_F32(addr, offset) HEAPF32[(addr >> 2) + offset]

#define DEREF_F64(addr, offset) HEAPF64[(addr >> 3) + offset]

#define FFI_TYPE__TYPE(addr)  DEREF_U16(addr + 6, 0)

#if WASM_BIGINT
#define SIG(sig)
#else
#define SIG(sig) sig
#endif


EM_JS_MACROS(void, ffi_call, (ffi_cif *cif, ffi_fp fn, void *rvalue, void **avalue), {
  const cif_abi = DEREF_U32(cif, 0);
  const cif_nargs = DEREF_U32(cif, 1);
  const cif_arg_types = DEREF_U32(cif, 2);
  const cif_rtype = DEREF_U32(cif, 3);

  const args = [];
  const rtype = FFI_TYPE__TYPE(cif_rtype);

#if WASM_BIGINT
  switch(rtype){
    case FFI_TYPE_STRUCT:
      throw new Error('struct ret marshalling nyi');
    case FFI_TYPE_COMPLEX:
      throw new Error('complex ret marshalling nyi');
  }
  if (rtype < 0 || rtype > 14) {
    throw new Error('Unexpected rtype ' + rtype);
  }
#else
  let sig;
  switch(rtype) {
    case FFI_TYPE_VOID:
      sig = 'v';
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
    case FFI_TYPE_LONGDOUBLE:
      sig = 'd';
      break;
    case FFI_TYPE_UINT64:
    case FFI_TYPE_SINT64:
      sig = 'j';
      break;
    case FFI_TYPE_STRUCT:
      throw new Error('struct ret marshalling nyi');
    case FFI_TYPE_COMPLEX:
      throw new Error('complex ret marshalling nyi');
    default:
      throw new Error('Unexpected rtype ' + rtype);
  }
#endif

  for (var i = 0; i < cif_nargs; i++) {
    var ptr = DEREF_U32(avalue, i);

    var arg_type = DEREF_U32(cif_arg_types, i);
    var typ = FFI_TYPE__TYPE(arg_type);

    switch(typ){
      case FFI_TYPE_INT:
      case FFI_TYPE_SINT32:
        args.push(DEREF_I32(ptr, 0));
        SIG(sig += 'i');
        break;
      case FFI_TYPE_FLOAT:
        args.push(DEREF_F32(ptr, 0));
        SIG(sig += 'f');
        break;
      case FFI_TYPE_DOUBLE:
        args.push(DEREF_F64(ptr, 0));
        SIG(sig += 'd'); 
        break;
      case FFI_TYPE_LONGDOUBLE:
        let HEAPU64 = new BigInt64Array(HEAP8.buffer);
        args.push(HEAPU64[ptr >> 3]);
        args.push(HEAPU64[(ptr >> 3) + 1]);
        break;
      case FFI_TYPE_UINT8:
        args.push(HEAPU8[ptr]);
        SIG(sig += 'i');
        break;
      case FFI_TYPE_SINT8:
        args.push(HEAP8[ptr]);
        SIG(sig += 'i');
        break;
      case FFI_TYPE_UINT16:
        args.push(DEREF_U16(ptr, 0));
        SIG(sig += 'i');
        break;
      case FFI_TYPE_SINT16:
        args.push(DEREF_U16(ptr, 0));
        SIG(sig += 'i');
        break;
      case FFI_TYPE_UINT32:
      case FFI_TYPE_POINTER:
        args.push(DEREF_U32(ptr, 0));
        SIG(sig += 'i');
        break;
      case FFI_TYPE_UINT64:
      case FFI_TYPE_SINT64:
#if WASM_BIGINT
        args.push(BigInt(DEREF_U32(ptr, 0)) | (BigInt(DEREF_U32(ptr, 1)) << 32n));
#else
        // LEGALIZE_JS_FFI mode splits i64 (j) into two i32 args
        // for compatibility with JavaScript's f64-based numbers.
        args.push(DEREF_U32(ptr, 0));
        args.push(DEREF_U32(ptr, 1));
        sig += 'j';
#endif
        break;
      case FFI_TYPE_STRUCT:
        throw new Error('struct marshalling nyi');
      case FFI_TYPE_COMPLEX:
        throw new Error('complex marshalling nyi');
      default:
        throw new Error('Unexpected type ' + typ);
    }
  }

#if WASM_BIGINT
  var result = wasmTable.get(fn).apply(null, args);
#else
  var result = dynCall(sig, fn, args);
#endif

  switch(rtype){
    case FFI_TYPE_VOID:
      break;
    case FFI_TYPE_INT:
    case FFI_TYPE_UINT32: 
    case FFI_TYPE_SINT32: 
    case FFI_TYPE_POINTER:
      DEREF_I32(rvalue, 0) = result;
      break;
    case FFI_TYPE_FLOAT:
      DEREF_F32(rvalue, 0) = result;
      break;
    case FFI_TYPE_DOUBLE: 
    case FFI_TYPE_LONGDOUBLE:
      DEREF_F64(rvalue, 0) = result;
      break;
    case FFI_TYPE_UINT8:
    case FFI_TYPE_SINT8:
      HEAP8[rvalue] = result;
      break;
    case FFI_TYPE_UINT16: 
    case FFI_TYPE_SINT16:
      DEREF_I16(rvalue, 0) = result;
      break;
    case FFI_TYPE_UINT64: 
    case FFI_TYPE_SINT64:
#if WASM_BIGINT
      DEREF_I32(rvalue, 0) = Number(result & 0xffffffffn) | 0;
      DEREF_I32(rvalue, 1) = Number(result >> 32n) | 0;
#else
      // Warning: returns a truncated 32-bit integer directly.
      // High bits are in $tempRet0
      DEREF_I32(rvalue, 0) = result;
      DEREF_I32(rvalue, 1) = Module.getTempRet0();
#endif 
      break;
    case  FFI_TYPE_STRUCT:
      throw new Error('struct ret marshalling nyi');
    case FFI_TYPE_COMPLEX:
      throw new Error('complex ret marshalling nyi');
    default:
      throw new Error('Unexpected rtype ' + rtype);
  }
});


// void *ffi_closure_alloc (size_t size, void **code){

// }

// void ffi_closure_free(void *writable){

// }

// EM_JS_MACROS(void *, ffi_prep_closure_create_trampoline, (void (*fun)(ffi_cif*,void*,void**,void*)), {

// })


// ffi_status
// ffi_prep_closure_loc (ffi_closure* closure,
//         ffi_cif* cif,
//         void (*fun)(ffi_cif*,void*,void**,void*),
//         void *user_data,
//         void *codeloc)
// {
//   void (*closure_func)(ffi_closure*) = NULL;


//   closure->cif  = cif;
//   closure->user_data = user_data;
//   closure->fun  = fun;

//   return FFI_OK;
// }
