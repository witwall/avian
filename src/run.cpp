#include "common.h"
#include "system.h"
#include "heap.h"
#include "class-finder.h"
#include "constants.h"
#include "run.h"
#include "jnienv.h"
#include "builtin.h"
#include "machine.h"

using namespace vm;

namespace {

void
pushFrame(Thread* t, object method)
{
  if (t->frame >= 0) {
    pokeInt(t, t->frame + FrameIpOffset, t->ip);
  }
  t->ip = 0;

  unsigned parameterFootprint = methodParameterFootprint(t, method);
  unsigned base = t->sp - parameterFootprint;
  unsigned locals = parameterFootprint;

  if ((methodFlags(t, method) & ACC_NATIVE) == 0) {
    t->code = methodCode(t, method);

    locals = codeMaxLocals(t, t->code);

    memset(t->stack + ((base + parameterFootprint) * 2), 0,
           (locals - parameterFootprint) * BytesPerWord * 2);
  }

  unsigned frame = base + locals;
  pokeInt(t, frame + FrameNextOffset, t->frame);
  t->frame = frame;

  t->sp = frame + FrameFootprint;

  pokeInt(t, frame + FrameBaseOffset, base);
  pokeObject(t, frame + FrameMethodOffset, method);
  pokeInt(t, t->frame + FrameIpOffset, 0);

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      acquire(t, methodClass(t, method));
    } else {
      acquire(t, peekObject(t, base));
    }   
  }
}

void
popFrame(Thread* t)
{
  object method = frameMethod(t, t->frame);

  if (methodFlags(t, method) & ACC_SYNCHRONIZED) {
    if (methodFlags(t, method) & ACC_STATIC) {
      release(t, methodClass(t, method));
    } else {
      release(t, peekObject(t, frameBase(t, t->frame)));
    }   
  }

  t->sp = frameBase(t, t->frame);
  t->frame = frameNext(t, t->frame);
  if (t->frame >= 0) {
    t->code = methodCode(t, frameMethod(t, t->frame));
    t->ip = frameIp(t, t->frame);
  } else {
    t->code = 0;
    t->ip = 0;
  }
}

void
registerWeakReference(Thread* t, object r)
{
  PROTECT(t, r);

  ACQUIRE(t, t->vm->referenceLock);

  // jreferenceNext(t, r)
  cast<object>(r, 3 * BytesPerWord) = t->vm->weakReferences;
  t->vm->weakReferences = r;
}

inline object
make(Thread* t, object class_)
{
  PROTECT(t, class_);
  unsigned sizeInBytes = pad(classFixedSize(t, class_));
  object instance = allocate(t, sizeInBytes);
  *static_cast<object*>(instance) = class_;
  memset(static_cast<object*>(instance) + 1, 0,
         sizeInBytes - sizeof(object));

  if (UNLIKELY(classVmFlags(t, class_) & WeakReferenceFlag)) {
    registerWeakReference(t, instance);
  }

  return instance;
}

inline void
setStatic(Thread* t, object field, object value)
{
  set(t, arrayBody(t, classStaticTable(t, fieldClass(t, field)),
                   fieldOffset(t, field)), value);
}

bool
instanceOf(Thread* t, object class_, object o)
{
  if (o == 0) {
    return false;
  }

  if (classFlags(t, class_) & ACC_INTERFACE) {
    for (object oc = objectClass(t, o); oc; oc = classSuper(t, oc)) {
      object itable = classInterfaceTable(t, oc);
      for (unsigned i = 0; i < arrayLength(t, itable); i += 2) {
        if (arrayBody(t, itable, i) == class_) {
          return true;
        }
      }
    }
  } else {
    for (object oc = objectClass(t, o); oc; oc = classSuper(t, oc)) {
      if (oc == class_) {
        return true;
      }
    }
  }

  return false;
}

object
findInterfaceMethod(Thread* t, object method, object o)
{
  object interface = methodClass(t, method);
  object itable = classInterfaceTable(t, objectClass(t, o));
  for (unsigned i = 0; i < arrayLength(t, itable); i += 2) {
    if (arrayBody(t, itable, i) == interface) {
      return arrayBody(t, arrayBody(t, itable, i + 1),
                       methodOffset(t, method));
    }
  }
  abort(t);
}

inline object
findMethod(Thread* t, object method, object class_)
{
  return arrayBody(t, classVirtualTable(t, class_), 
                   methodOffset(t, method));
}

bool
isSuperclass(Thread* t, object class_, object base)
{
  for (object oc = classSuper(t, base); oc; oc = classSuper(t, oc)) {
    if (oc == class_) {
      return true;
    }
  }
  return false;
}

inline bool
isSpecialMethod(Thread* t, object method, object class_)
{
  return (classFlags(t, class_) & ACC_SUPER)
    and strcmp(reinterpret_cast<const int8_t*>("<init>"), 
               &byteArrayBody(t, methodName(t, method), 0)) != 0
    and isSuperclass(t, methodClass(t, method), class_);
}

object
find(Thread* t, object table, object reference,
     object& (*name)(Thread*, object),
     object& (*spec)(Thread*, object))
{
  object n = referenceName(t, reference);
  object s = referenceSpec(t, reference);
  for (unsigned i = 0; i < arrayLength(t, table); ++i) {
    object o = arrayBody(t, table, i);

    if (strcmp(&byteArrayBody(t, name(t, o), 0),
               &byteArrayBody(t, n, 0)) == 0 and
        strcmp(&byteArrayBody(t, spec(t, o), 0),
               &byteArrayBody(t, s, 0)) == 0)
    {
      return o;
    }               
  }

  return 0;
}

inline object
findFieldInClass(Thread* t, object class_, object reference)
{
  return find(t, classFieldTable(t, class_), reference, fieldName, fieldSpec);
}

inline object
findMethodInClass(Thread* t, object class_, object reference)
{
  return find(t, classMethodTable(t, class_), reference, methodName,
              methodSpec);
}

inline object
resolveClass(Thread* t, object pool, unsigned index)
{
  object o = arrayBody(t, pool, index);
  if (objectClass(t, o) == arrayBody(t, t->vm->types, Machine::ByteArrayType))
  {
    PROTECT(t, pool);

    o = resolveClass(t, o);
    if (UNLIKELY(t->exception)) return 0;
    
    set(t, arrayBody(t, pool, index), o);
  }
  return o; 
}

inline object
resolveClass(Thread* t, object container, object& (*class_)(Thread*, object))
{
  object o = class_(t, container);
  if (objectClass(t, o) == arrayBody(t, t->vm->types, Machine::ByteArrayType))
  {
    PROTECT(t, container);

    o = resolveClass(t, o);
    if (UNLIKELY(t->exception)) return 0;
    
    set(t, class_(t, container), o);
  }
  return o; 
}

inline object
resolve(Thread* t, object pool, unsigned index,
        object (*find)(Thread*, object, object),
        object (*makeError)(Thread*, object))
{
  object o = arrayBody(t, pool, index);
  if (objectClass(t, o) == arrayBody(t, t->vm->types, Machine::ReferenceType))
  {
    PROTECT(t, pool);

    object reference = o;
    PROTECT(t, reference);

    object class_ = resolveClass(t, o, referenceClass);
    if (UNLIKELY(t->exception)) return 0;
    
    for (o = 0; o == 0 and class_; class_ = classSuper(t, class_)) {
      o = find(t, class_, arrayBody(t, pool, index));
    }

    if (o == 0) {
      object message = makeString
        (t, "%s %s not found in %s",
         &byteArrayBody(t, referenceName(t, reference), 0),
         &byteArrayBody(t, referenceSpec(t, reference), 0),
         &byteArrayBody(t, referenceClass(t, reference), 0));
      t->exception = makeError(t, message);
    }
    
    set(t, arrayBody(t, pool, index), o);
  }

  return o;
}

inline object
resolveField(Thread* t, object pool, unsigned index)
{
  return resolve(t, pool, index, findFieldInClass, makeNoSuchFieldError);
}

inline object
resolveMethod(Thread* t, object pool, unsigned index)
{
  return resolve(t, pool, index, findMethodInClass, makeNoSuchMethodError);
}

object
makeNativeMethodData(Thread* t, object method, void* function, bool builtin)
{
  PROTECT(t, method);

  object data = makeNativeMethodData(t,
                                     function,
                                     0, // argument table size
                                     0, // return code,
                                     builtin,
                                     methodParameterCount(t, method) + 1,
                                     false);
        
  unsigned argumentTableSize = BytesPerWord;
  unsigned index = 0;

  nativeMethodDataParameterTypes(t, data, index++) = POINTER_TYPE;

  if ((methodFlags(t, method) & ACC_STATIC) == 0) {
    nativeMethodDataParameterTypes(t, data, index++) = POINTER_TYPE;
    argumentTableSize += BytesPerWord;
  }

  const char* s = reinterpret_cast<const char*>
    (&byteArrayBody(t, methodSpec(t, method), 0));
  ++ s; // skip '('
  while (*s and *s != ')') {
    unsigned code = fieldCode(t, *s);
    nativeMethodDataParameterTypes(t, data, index++) = fieldType(t, code);

    switch (*s) {
    case 'L':
      argumentTableSize += BytesPerWord;
      while (*s and *s != ';') ++ s;
      ++ s;
      break;

    case '[':
      argumentTableSize += BytesPerWord;
      while (*s == '[') ++ s;
      break;
      
    default:
      argumentTableSize += pad(primitiveSize(t, code));
      ++ s;
      break;
    }
  }

  nativeMethodDataArgumentTableSize(t, data) = argumentTableSize;
  nativeMethodDataReturnCode(t, data) = fieldCode(t, s[1]);

  return data;
}

inline object
resolveNativeMethodData(Thread* t, object method)
{
  if (objectClass(t, methodCode(t, method))
      == arrayBody(t, t->vm->types, Machine::ByteArrayType))
  {
    object data = 0;
    for (System::Library* lib = t->vm->libraries; lib; lib = lib->next()) {
      void* p = lib->resolve(reinterpret_cast<const char*>
                             (&byteArrayBody(t, methodCode(t, method), 0)));
      if (p) {
        PROTECT(t, method);
        data = makeNativeMethodData(t, method, p, false);
        break;
      }
    }

    if (data == 0) {
      object p = hashMapFind(t, t->vm->builtinMap, methodCode(t, method),
                             byteArrayHash, byteArrayEqual);
      if (p) {
        PROTECT(t, method);
        data = makeNativeMethodData(t, method, pointerValue(t, p), true);
      }
    }

    if (LIKELY(data)) {
      set(t, methodCode(t, method), data);
    } else {
      object message = makeString
        (t, "%s", &byteArrayBody(t, methodCode(t, method), 0));
      t->exception = makeUnsatisfiedLinkError(t, message);
    }

    return data;
  } else {
    return methodCode(t, method);
  }  
}

inline void
checkStack(Thread* t, object method)
{
  unsigned parameterFootprint = methodParameterFootprint(t, method);
  unsigned base = t->sp - parameterFootprint;
  if (UNLIKELY(base
               + codeMaxLocals(t, methodCode(t, method))
               + FrameFootprint
               + codeMaxStack(t, methodCode(t, method))
               > Thread::StackSizeInWords / 2))
  {
    t->exception = makeStackOverflowError(t);
  }
}

unsigned
invokeNative(Thread* t, object method)
{
  PROTECT(t, method);

  object data = resolveNativeMethodData(t, method);
  if (UNLIKELY(t->exception)) {
    return VoidField;
  }

  PROTECT(t, data);

  pushFrame(t, method);

  unsigned count = methodParameterCount(t, method);

  unsigned size = nativeMethodDataArgumentTableSize(t, data);
  uintptr_t args[size / BytesPerWord];
  unsigned offset = 0;

  args[offset++] = reinterpret_cast<uintptr_t>(t);

  unsigned sp = frameBase(t, t->frame);
  for (unsigned i = 0; i < count; ++i) {
    unsigned type = nativeMethodDataParameterTypes(t, data, i + 1);

    switch (type) {
    case INT8_TYPE:
    case INT16_TYPE:
    case INT32_TYPE:
    case FLOAT_TYPE:
      args[offset++] = peekInt(t, sp++);
      break;

    case INT64_TYPE:
    case DOUBLE_TYPE: {
      uint64_t v = peekLong(t, sp);
      memcpy(args + offset, &v, 8);
      offset += (8 / BytesPerWord);
      sp += 2;
    } break;

    case POINTER_TYPE:
      args[offset++] = reinterpret_cast<uintptr_t>
        (t->stack + ((sp++) * 2) + 1);
      break;

    default: abort(t);
    }
  }

  unsigned returnCode = nativeMethodDataReturnCode(t, data);
  unsigned returnType = fieldType(t, returnCode);
  void* function = nativeMethodDataFunction(t, data);

  bool builtin = nativeMethodDataBuiltin(t, data);
  Thread::State oldState = t->state;
  if (not builtin) {    
    enter(t, Thread::IdleState);
  }

  if (DebugRun) {
    fprintf(stderr, "invoke native method %s.%s\n",
            &byteArrayBody(t, className(t, methodClass(t, method)), 0),
            &byteArrayBody(t, methodName(t, method), 0));
  }

  uint64_t result = t->vm->system->call
    (function,
     args,
     &nativeMethodDataParameterTypes(t, data, 0),
     count + 1,
     size,
     returnType);

  if (DebugRun) {
    fprintf(stderr, "return from native method %s.%s\n",
            &byteArrayBody
            (t, className(t, methodClass(t, frameMethod(t, t->frame))), 0),
            &byteArrayBody
            (t, methodName(t, frameMethod(t, t->frame)), 0));
  }

  if (not builtin) {
    enter(t, oldState);
  }

  popFrame(t);

  if (UNLIKELY(t->exception)) {
    return VoidField;
  }

  switch (returnCode) {
  case ByteField:
  case BooleanField:
  case CharField:
  case ShortField:
  case FloatField:
  case IntField:
    if (DebugRun) {
      fprintf(stderr, "result: " LLD "\n", result);
    }
    pushInt(t, result);
    break;

  case LongField:
  case DoubleField:
    if (DebugRun) {
      fprintf(stderr, "result: " LLD "\n", result);
    }
    pushLong(t, result);
    break;

  case ObjectField:
    if (DebugRun) {
      fprintf(stderr, "result: %p at %p\n", result == 0 ? 0 :
              *reinterpret_cast<object*>(static_cast<uintptr_t>(result)),
              reinterpret_cast<object*>(static_cast<uintptr_t>(result)));
    }
    pushObject(t, result == 0 ? 0 :
               *reinterpret_cast<object*>(static_cast<uintptr_t>(result)));
    break;

  case VoidField:
    break;

  default:
    abort(t);
  };

  return returnCode;
}

object
run(Thread* t)
{
  unsigned instruction = nop;
  unsigned& ip = t->ip;
  unsigned& sp = t->sp;
  int& frame = t->frame;
  object& code = t->code;
  object& exception = t->exception;
  uintptr_t* stack = t->stack;

  if (UNLIKELY(exception)) goto throw_;

 loop:
  instruction = codeBody(t, code, ip++);

  if (DebugRun) {
    fprintf(stderr, "ip: %d; instruction: 0x%x in %s.%s ",
            ip - 1,
            instruction,
            &byteArrayBody
            (t, className(t, methodClass(t, frameMethod(t, frame))), 0),
            &byteArrayBody
            (t, methodName(t, frameMethod(t, frame)), 0));

    int line = lineNumber(t, frameMethod(t, frame), ip);
    switch (line) {
    case NativeLine:
      fprintf(stderr, "(native)\n");
      break;
    case UnknownLine:
      fprintf(stderr, "(unknown line)\n");
      break;
    default:
      fprintf(stderr, "(line %d)\n", line);
    }
  }

  switch (instruction) {
  case aaload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < objectArrayLength(t, array)))
      {
        pushObject(t, objectArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aastore: {
    object value = popObject(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < objectArrayLength(t, array)))
      {
        set(t, objectArrayBody(t, array, index), value);
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    objectArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case aconst_null: {
    pushObject(t, 0);
  } goto loop;

  case aload: {
    pushObject(t, localObject(t, codeBody(t, code, ip++)));
  } goto loop;

  case aload_0: {
    pushObject(t, localObject(t, 0));
  } goto loop;

  case aload_1: {
    pushObject(t, localObject(t, 1));
  } goto loop;

  case aload_2: {
    pushObject(t, localObject(t, 2));
  } goto loop;

  case aload_3: {
    pushObject(t, localObject(t, 3));
  } goto loop;

  case anewarray: {
    int32_t count = popInt(t);

    if (LIKELY(count >= 0)) {
      uint8_t index1 = codeBody(t, code, ip++);
      uint8_t index2 = codeBody(t, code, ip++);
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;
            
      pushObject(t, makeObjectArray(t, class_, count, true));
    } else {
      object message = makeString(t, "%d", count);
      exception = makeNegativeArraySizeException(t, message);
      goto throw_;
    }
  } goto loop;

  case areturn: {
    object result = popObject(t);
    popFrame(t);
    if (frame >= 0) {
      pushObject(t, result);
      goto loop;
    } else {
      return result;
    }
  } goto loop;

  case arraylength: {
    object array = popObject(t);
    if (LIKELY(array)) {
      pushInt(t, cast<uintptr_t>(array, BytesPerWord));
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case astore: {
    setLocalObject(t, codeBody(t, code, ip++), popObject(t));
  } goto loop;

  case astore_0: {
    setLocalObject(t, 0, popObject(t));
  } goto loop;

  case astore_1: {
    setLocalObject(t, 1, popObject(t));
  } goto loop;

  case astore_2: {
    setLocalObject(t, 2, popObject(t));
  } goto loop;

  case astore_3: {
    setLocalObject(t, 3, popObject(t));
  } goto loop;

  case athrow: {
    exception = popObject(t);
    if (UNLIKELY(exception == 0)) {
      exception = makeNullPointerException(t);      
    }
  } goto throw_;

  case baload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < byteArrayLength(t, array)))
      {
        pushInt(t, byteArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    byteArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bastore: {
    int8_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < byteArrayLength(t, array)))
      {
        byteArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    byteArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case bipush: {
    pushInt(t, codeBody(t, code, ip++));
  } goto loop;

  case caload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < charArrayLength(t, array)))
      {
        pushInt(t, charArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case castore: {
    uint16_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < charArrayLength(t, array)))
      {
        charArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    charArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case checkcast: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    if (peekObject(t, sp - 1)) {
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;

      if (not instanceOf(t, class_, peekObject(t, sp - 1))) {
        object message = makeString
          (t, "%s as %s",
           &byteArrayBody
           (t, className(t, objectClass(t, peekObject(t, sp - 1))), 0),
           &byteArrayBody(t, className(t, class_), 0));
        exception = makeClassCastException(t, message);
        goto throw_;
      }
    }
  } goto loop;

  case dup: {
    if (DebugStack) {
      fprintf(stderr, "dup\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup_x1: {
    if (DebugStack) {
      fprintf(stderr, "dup_x1\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp    ) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup_x2: {
    if (DebugStack) {
      fprintf(stderr, "dup_x2\n");
    }

    memcpy(stack + ((sp    ) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 3) * 2), stack + ((sp    ) * 2), BytesPerWord * 2);
    ++ sp;
  } goto loop;

  case dup2: {
    if (DebugStack) {
      fprintf(stderr, "dup2\n");
    }

    memcpy(stack + ((sp + 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case dup2_x1: {
    if (DebugStack) {
      fprintf(stderr, "dup2_x1\n");
    }

    memcpy(stack + ((sp + 1) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp    ) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 3) * 2), stack + ((sp    ) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case dup2_x2: {
    if (DebugStack) {
      fprintf(stderr, "dup2_x2\n");
    }

    memcpy(stack + ((sp + 1) * 2), stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp    ) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 3) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), stack + ((sp - 4) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 4) * 2), stack + ((sp    ) * 2), BytesPerWord * 4);
    sp += 2;
  } goto loop;

  case getfield: {
    if (LIKELY(peekObject(t, sp - 1))) {
      uint8_t index1 = codeBody(t, code, ip++);
      uint8_t index2 = codeBody(t, code, ip++);
      uint16_t index = (index1 << 8) | index2;
    
      object field = resolveField(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;
      
      object instance = popObject(t);

      switch (fieldCode(t, field)) {
      case ByteField:
      case BooleanField:
        pushInt(t, cast<int8_t>(instance, fieldOffset(t, field)));
        break;

      case CharField:
      case ShortField:
        pushInt(t, cast<int16_t>(instance, fieldOffset(t, field)));
        break;

      case FloatField:
      case IntField:
        pushInt(t, cast<int32_t>(instance, fieldOffset(t, field)));
        break;

      case DoubleField:
      case LongField:
        pushLong(t, cast<int64_t>(instance, fieldOffset(t, field)));
        break;

      case ObjectField:
        pushObject(t, cast<object>(instance, fieldOffset(t, field)));
        break;

      default:
        abort(t);
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case getstatic: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;

    object clinit = classInitializer(t, fieldClass(t, field));
    if (clinit) {
      set(t, classInitializer(t, fieldClass(t, field)), 0);
      code = clinit;
      ip -= 3;
      goto invoke;
    }

    object v = arrayBody(t, classStaticTable(t, fieldClass(t, field)),
                         fieldOffset(t, field));

    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField:
      pushInt(t, intValue(t, v));
      break;

    case DoubleField:
    case LongField:
      pushLong(t, longValue(t, v));
      break;

    case ObjectField:
      pushObject(t, v);
      break;

    default: abort(t);
    }
  } goto loop;

  case goto_: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
  } goto loop;
    
  case goto_w: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);
    uint8_t offset3 = codeBody(t, code, ip++);
    uint8_t offset4 = codeBody(t, code, ip++);

    ip = (ip - 5) + static_cast<int32_t>
      (((offset1 << 24) | (offset2 << 16) | (offset3 << 8) | offset4));
  } goto loop;

  case i2b: {
    pushInt(t, static_cast<int8_t>(popInt(t)));
  } goto loop;

  case i2c: {
    pushInt(t, static_cast<uint16_t>(popInt(t)));
  } goto loop;

  case i2l: {
    pushLong(t, popInt(t));
  } goto loop;

  case i2s: {
    pushInt(t, static_cast<int16_t>(popInt(t)));
  } goto loop;

  case iadd: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a + b);
  } goto loop;

  case iaload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < intArrayLength(t, array)))
      {
        pushInt(t, intArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iand: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a & b);
  } goto loop;

  case iastore: {
    int32_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < intArrayLength(t, array)))
      {
        intArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    intArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case iconst_0: {
    pushInt(t, 0);
  } goto loop;

  case iconst_1: {
    pushInt(t, 1);
  } goto loop;

  case iconst_2: {
    pushInt(t, 2);
  } goto loop;

  case iconst_3: {
    pushInt(t, 3);
  } goto loop;

  case iconst_4: {
    pushInt(t, 4);
  } goto loop;

  case iconst_5: {
    pushInt(t, 5);
  } goto loop;

  case idiv: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a / b);
  } goto loop;

  case if_acmpeq: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    object b = popObject(t);
    object a = popObject(t);
    
    if (a == b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_acmpne: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    object b = popObject(t);
    object a = popObject(t);
    
    if (a != b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmpeq: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a == b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmpne: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a != b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmpgt: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a > b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmpge: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a >= b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmplt: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a < b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case if_icmple: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    if (a <= b) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifeq: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (popInt(t) == 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifne: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (popInt(t)) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifgt: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (static_cast<int32_t>(popInt(t)) > 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifge: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (static_cast<int32_t>(popInt(t)) >= 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case iflt: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (static_cast<int32_t>(popInt(t)) < 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifle: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (static_cast<int32_t>(popInt(t)) <= 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifnonnull: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (popObject(t)) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case ifnull: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    if (popObject(t) == 0) {
      ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
    }
  } goto loop;

  case iinc: {
    uint8_t index = codeBody(t, code, ip++);
    int8_t c = codeBody(t, code, ip++);
    
    setLocalInt(t, index, localInt(t, index) + c);
  } goto loop;

  case iload: {
    pushInt(t, localInt(t, codeBody(t, code, ip++)));
  } goto loop;

  case iload_0: {
    pushInt(t, localInt(t, 0));
  } goto loop;

  case iload_1: {
    pushInt(t, localInt(t, 1));
  } goto loop;

  case iload_2: {
    pushInt(t, localInt(t, 2));
  } goto loop;

  case iload_3: {
    pushInt(t, localInt(t, 3));
  } goto loop;

  case imul: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a * b);
  } goto loop;

  case ineg: {
    pushInt(t, - popInt(t));
  } goto loop;

  case instanceof: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    if (peekObject(t, sp - 1)) {
      uint16_t index = (index1 << 8) | index2;
      
      object class_ = resolveClass(t, codePool(t, code), index - 1);
      if (UNLIKELY(exception)) goto throw_;

      if (instanceOf(t, class_, peekObject(t, sp - 1))) {
        pushInt(t, 1);
      } else {
        pushInt(t, 0);
      }
    } else {
      pushInt(t, 0);
    }
  } goto loop;

  case invokeinterface: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;
    
    ip += 2;

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      code = findInterfaceMethod
        (t, method, peekObject(t, sp - parameterFootprint));
      if (UNLIKELY(exception)) goto throw_;

      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokespecial: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      object class_ = methodClass(t, frameMethod(t, frame));
      if (isSpecialMethod(t, method, class_)) {
        class_ = classSuper(t, class_);

        if (classVirtualTable(t, class_) == 0) {
          PROTECT(t, class_);

          resolveClass(t, className(t, class_));
          if (UNLIKELY(exception)) goto throw_;

          object clinit = classInitializer(t, class_);
          if (clinit) {
            set(t, classInitializer(t, methodClass(t, method)), 0);
            code = clinit;
            ip -= 3;
            goto invoke;
          }          
        }

        code = findMethod(t, method, class_);
      } else {
        code = method;
      }
      
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case invokestatic: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    object clinit = classInitializer(t, methodClass(t, method));
    if (clinit) {
      set(t, classInitializer(t, methodClass(t, method)), 0);
      code = clinit;
      ip -= 3;
      goto invoke;
    }

    code = method;
  } goto invoke;

  case invokevirtual: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    object method = resolveMethod(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
    
    unsigned parameterFootprint = methodParameterFootprint(t, method);
    if (LIKELY(peekObject(t, sp - parameterFootprint))) {
      object class_ = objectClass(t, peekObject(t, sp - parameterFootprint));

      if (classVirtualTable(t, class_) == 0) {
        PROTECT(t, class_);

        resolveClass(t, className(t, class_));
        if (UNLIKELY(exception)) goto throw_;

        object clinit = classInitializer(t, class_);
        if (clinit) {
          set(t, classInitializer(t, methodClass(t, method)), 0);
          code = clinit;
          ip -= 3;
          goto invoke;
        }          
      }

      code = findMethod(t, method, class_);      
      goto invoke;
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case ior: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a | b);
  } goto loop;

  case irem: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a % b);
  } goto loop;

  case ireturn: {
    int32_t result = popInt(t);
    popFrame(t);
    if (frame >= 0) {
      pushInt(t, result);
      goto loop;
    } else {
      return makeInt(t, result);
    }
  } goto loop;

  case ishl: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a << b);
  } goto loop;

  case ishr: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a >> b);
  } goto loop;

  case istore: {
    setLocalInt(t, codeBody(t, code, ip++), popInt(t));
  } goto loop;

  case istore_0: {
    setLocalInt(t, 0, popInt(t));
  } goto loop;

  case istore_1: {
    setLocalInt(t, 1, popInt(t));
  } goto loop;

  case istore_2: {
    setLocalInt(t, 2, popInt(t));
  } goto loop;

  case istore_3: {
    setLocalInt(t, 3, popInt(t));
  } goto loop;

  case isub: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a - b);
  } goto loop;

  case iushr: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, static_cast<uint32_t>(a >> b));
  } goto loop;

  case ixor: {
    int32_t b = popInt(t);
    int32_t a = popInt(t);
    
    pushInt(t, a ^ b);
  } goto loop;

  case jsr: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);

    pushInt(t, ip);
    ip = (ip - 3) + static_cast<int16_t>(((offset1 << 8) | offset2));
  } goto loop;

  case jsr_w: {
    uint8_t offset1 = codeBody(t, code, ip++);
    uint8_t offset2 = codeBody(t, code, ip++);
    uint8_t offset3 = codeBody(t, code, ip++);
    uint8_t offset4 = codeBody(t, code, ip++);

    pushInt(t, ip);
    ip = (ip - 3) + static_cast<int32_t>
      ((offset1 << 24) | (offset2 << 16) | (offset3 << 8) | offset4);
  } goto loop;

  case l2i: {
    pushInt(t, static_cast<int32_t>(popLong(t)));
  } goto loop;

  case ladd: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a + b);
  } goto loop;

  case laload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < longArrayLength(t, array)))
      {
        pushLong(t, longArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case land: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a & b);
  } goto loop;

  case lastore: {
    int64_t value = popLong(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < longArrayLength(t, array)))
      {
        longArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    longArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case lcmp: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushInt(t, a > b ? 1 : a == b ? 0 : -1);
  } goto loop;

  case lconst_0: {
    pushLong(t, 0);
  } goto loop;

  case lconst_1: {
    pushLong(t, 1);
  } goto loop;

  case ldc:
  case ldc_w: {
    uint16_t index;

    if (instruction == ldc) {
      index = codeBody(t, code, ip++);
    } else {
      uint8_t index1 = codeBody(t, code, ip++);
      uint8_t index2 = codeBody(t, code, ip++);
      index = (index1 << 8) | index2;
    }

    object v = arrayBody(t, codePool(t, code), index - 1);

    if (objectClass(t, v) == arrayBody(t, t->vm->types, Machine::IntType)) {
      pushInt(t, intValue(t, v));
    } else if (objectClass(t, v)
               == arrayBody(t, t->vm->types, Machine::StringType))
    {
      pushObject(t, v);
    } else if (objectClass(t, v)
               == arrayBody(t, t->vm->types, Machine::FloatType))
    {
      pushInt(t, floatValue(t, v));
    } else {
      abort(t);
    }
  } goto loop;

  case ldc2_w: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    object v = arrayBody(t, codePool(t, code), ((index1 << 8) | index2) - 1);

    if (objectClass(t, v) == arrayBody(t, t->vm->types, Machine::LongType)) {
      pushLong(t, longValue(t, v));
    } else if (objectClass(t, v)
               == arrayBody(t, t->vm->types, Machine::DoubleType))
    {
      pushLong(t, doubleValue(t, v));
    } else {
      abort(t);
    }
  } goto loop;

  case vm::ldiv: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a / b);
  } goto loop;

  case lload: {
    pushLong(t, localLong(t, codeBody(t, code, ip++)));
  } goto loop;

  case lload_0: {
    pushLong(t, localLong(t, 0));
  } goto loop;

  case lload_1: {
    pushLong(t, localLong(t, 1));
  } goto loop;

  case lload_2: {
    pushLong(t, localLong(t, 2));
  } goto loop;

  case lload_3: {
    pushLong(t, localLong(t, 3));
  } goto loop;

  case lmul: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a * b);
  } goto loop;

  case lneg: {
    pushLong(t, - popInt(t));
  } goto loop;

  case lor: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a | b);
  } goto loop;

  case lrem: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a % b);
  } goto loop;

  case lreturn: {
    int64_t result = popLong(t);
    popFrame(t);
    if (frame >= 0) {
      pushLong(t, result);
      goto loop;
    } else {
      return makeLong(t, result);
    }
  } goto loop;

  case lshl: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a << b);
  } goto loop;

  case lshr: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a >> b);
  } goto loop;

  case lstore: {
    setLocalLong(t, codeBody(t, code, ip++), popLong(t));
  } goto loop;

  case lstore_0: {
    setLocalLong(t, 0, popLong(t));
  } goto loop;

  case lstore_1: {
    setLocalLong(t, 1, popLong(t));
  } goto loop;

  case lstore_2: {
    setLocalLong(t, 2, popLong(t));
  } goto loop;

  case lstore_3: {
    setLocalLong(t, 3, popLong(t));
  } goto loop;

  case lsub: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a - b);
  } goto loop;

  case lushr: {
    uint64_t b = popLong(t);
    uint64_t a = popLong(t);
    
    pushLong(t, a >> b);
  } goto loop;

  case lxor: {
    int64_t b = popLong(t);
    int64_t a = popLong(t);
    
    pushLong(t, a ^ b);
  } goto loop;

  case monitorenter: {
    object o = popObject(t);
    if (LIKELY(o)) {
      acquire(t, o);
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case monitorexit: {
    object o = popObject(t);
    if (LIKELY(o)) {
      release(t, o);
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case new_: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;
    
    object class_ = resolveClass(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;

    object clinit = classInitializer(t, class_);
    if (clinit) {
      set(t, classInitializer(t, class_), 0);
      code = clinit;
      ip -= 3;
      goto invoke;
    }

    pushObject(t, make(t, class_));
  } goto loop;

  case newarray: {
    int32_t count = popInt(t);

    if (LIKELY(count >= 0)) {
      uint8_t type = codeBody(t, code, ip++);

      object array;

      switch (type) {
      case T_BOOLEAN:
        array = makeBooleanArray(t, count, true);
        break;

      case T_CHAR:
        array = makeCharArray(t, count, true);
        break;

      case T_FLOAT:
        array = makeFloatArray(t, count, true);
        break;

      case T_DOUBLE:
        array = makeDoubleArray(t, count, true);
        break;

      case T_BYTE:
        array = makeByteArray(t, count, true);
        break;

      case T_SHORT:
        array = makeShortArray(t, count, true);
        break;

      case T_INT:
        array = makeIntArray(t, count, true);
        break;

      case T_LONG:
        array = makeLongArray(t, count, true);
        break;

      default: abort(t);
      }
            
      pushObject(t, array);
    } else {
      object message = makeString(t, "%d", count);
      exception = makeNegativeArraySizeException(t, message);
      goto throw_;
    }
  } goto loop;

  case nop: goto loop;

  case pop_: {
    -- sp;
  } goto loop;

  case pop2: {
    sp -= 2;
  } goto loop;

  case putfield: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;
    
    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;
      
    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField: {
      int32_t value = popInt(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        switch (fieldCode(t, field)) {
        case ByteField:
        case BooleanField:
          cast<int8_t>(o, fieldOffset(t, field)) = value;
          break;
            
        case CharField:
        case ShortField:
          cast<int16_t>(o, fieldOffset(t, field)) = value;
          break;
            
        case FloatField:
        case IntField:
          cast<int32_t>(o, fieldOffset(t, field)) = value;
          break;
        }
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    case DoubleField:
    case LongField: {
      int64_t value = popLong(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        cast<int64_t>(o, fieldOffset(t, field)) = value;
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    case ObjectField: {
      object value = popObject(t);
      object o = popObject(t);
      if (LIKELY(o)) {
        set(t, cast<object>(o, fieldOffset(t, field)), value);
      } else {
        exception = makeNullPointerException(t);
        goto throw_;
      }
    } break;

    default: abort(t);
    }
  } goto loop;

  case putstatic: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    object field = resolveField(t, codePool(t, code), index - 1);
    if (UNLIKELY(exception)) goto throw_;

    object clinit = classInitializer(t, fieldClass(t, field));
    if (clinit) {
      set(t, classInitializer(t, fieldClass(t, field)), 0);
      code = clinit;
      ip -= 3;
      goto invoke;
    }

    PROTECT(t, field);
      
    object v;

    switch (fieldCode(t, field)) {
    case ByteField:
    case BooleanField:
    case CharField:
    case ShortField:
    case FloatField:
    case IntField: {
      v = makeInt(t, popInt(t));
    } break;

    case DoubleField:
    case LongField: {
      v = makeLong(t, popLong(t));
    } break;

    case ObjectField:
      v = popObject(t);
      break;

    default: abort(t);
    }

    set(t, arrayBody(t, classStaticTable(t, fieldClass(t, field)),
                     fieldOffset(t, field)), v);
  } goto loop;

  case ret: {
    ip = localInt(t, codeBody(t, code, ip));
  } goto loop;

  case return_: {
    popFrame(t);
    if (frame >= 0) {
      goto loop;
    } else {
      return 0;
    }
  } goto loop;

  case saload: {
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < shortArrayLength(t, array)))
      {
        pushInt(t, shortArrayBody(t, array, index));
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sastore: {
    int16_t value = popInt(t);
    int32_t index = popInt(t);
    object array = popObject(t);

    if (LIKELY(array)) {
      if (LIKELY(index >= 0 and
                 static_cast<uintptr_t>(index) < shortArrayLength(t, array)))
      {
        shortArrayBody(t, array, index) = value;
      } else {
        object message = makeString(t, "%d not in [0,%d]", index,
                                    shortArrayLength(t, array));
        exception = makeArrayIndexOutOfBoundsException(t, message);
        goto throw_;
      }
    } else {
      exception = makeNullPointerException(t);
      goto throw_;
    }
  } goto loop;

  case sipush: {
    uint8_t byte1 = codeBody(t, code, ip++);
    uint8_t byte2 = codeBody(t, code, ip++);

    pushInt(t, (byte1 << 8) | byte2);
  } goto loop;

  case swap: {
    uintptr_t tmp[2];
    memcpy(tmp                   , stack + ((sp - 1) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 1) * 2), stack + ((sp - 2) * 2), BytesPerWord * 2);
    memcpy(stack + ((sp - 2) * 2), tmp                   , BytesPerWord * 2);
  } goto loop;

  case wide: goto wide;

  default: abort(t);
  }

 wide:
  switch (codeBody(t, code, ip++)) {
  case aload: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    pushObject(t, localObject(t, (index1 << 8) | index2));
  } goto loop;

  case astore: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    setLocalObject(t, (index1 << 8) | index2, popObject(t));
  } goto loop;

  case iinc: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);
    uint16_t index = (index1 << 8) | index2;

    uint8_t count1 = codeBody(t, code, ip++);
    uint8_t count2 = codeBody(t, code, ip++);
    uint16_t count = (count1 << 8) | count2;
    
    setLocalInt(t, index, localInt(t, index) + count);
  } goto loop;

  case iload: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    pushInt(t, localInt(t, (index1 << 8) | index2));
  } goto loop;

  case istore: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    setLocalInt(t, (index1 << 8) | index2, popInt(t));
  } goto loop;

  case lload: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    pushLong(t, localLong(t, (index1 << 8) | index2));
  } goto loop;

  case lstore: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    setLocalLong(t, (index1 << 8) | index2,  popLong(t));
  } goto loop;

  case ret: {
    uint8_t index1 = codeBody(t, code, ip++);
    uint8_t index2 = codeBody(t, code, ip++);

    ip = localInt(t, (index1 << 8) | index2);
  } goto loop;

  default: abort(t);
  }

 invoke: {
    if (methodFlags(t, code) & ACC_NATIVE) {
      invokeNative(t, code);
      if (UNLIKELY(exception)) goto throw_;
    } else {
      checkStack(t, code);
      if (UNLIKELY(exception)) goto throw_;

      pushFrame(t, code);
    }
  } goto loop;

 throw_:
  if (DebugRun) {
    fprintf(stderr, "throw\n");
  }

  pokeInt(t, t->frame + FrameIpOffset, t->ip);
  for (; frame >= 0; frame = frameNext(t, frame)) {
    if (methodFlags(t, frameMethod(t, frame)) & ACC_NATIVE) {
      return 0;
    }

    code = methodCode(t, frameMethod(t, frame));
    object eht = codeExceptionHandlerTable(t, code);
    if (eht) {
      for (unsigned i = 0; i < exceptionHandlerTableLength(t, eht); ++i) {
        ExceptionHandler* eh = exceptionHandlerTableBody(t, eht, i);

        if (frameIp(t, frame) - 1 >= exceptionHandlerStart(eh)
            and frameIp(t, frame) - 1 < exceptionHandlerEnd(eh))
        {
          object catchType = 0;
          if (exceptionHandlerCatchType(eh)) {
            catchType = arrayBody
              (t, codePool(t, code), exceptionHandlerCatchType(eh) - 1);
          }

          if (catchType) {
            PROTECT(t, eht);
            catchType = resolveClass(t, catchType);
            eh = exceptionHandlerTableBody(t, eht, i);
          }

          if (catchType == 0 or instanceOf(t, catchType, exception)) {
            sp = frame + FrameFootprint;
            ip = exceptionHandlerIp(eh);
            pushObject(t, exception);
            exception = 0;
            goto loop;
          }
        }
      }
    }
  }

  for (object e = exception; e; e = throwableCause(t, e)) {
    if (e == exception) {
      fprintf(stderr, "uncaught exception: ");
    } else {
      fprintf(stderr, "caused by: ");
    }

    fprintf(stderr, "%s", &byteArrayBody
            (t, className(t, objectClass(t, exception)), 0));
  
    if (throwableMessage(t, exception)) {
      object m = throwableMessage(t, exception);
      char message[stringLength(t, m) + 1];
      stringChars(t, m, message);
      fprintf(stderr, ": %s\n", message);
    } else {
      fprintf(stderr, "\n");
    }

    object trace = throwableTrace(t, e);
    for (unsigned i = 0; i < arrayLength(t, trace); ++i) {
      object e = arrayBody(t, trace, i);
      const int8_t* class_ = &byteArrayBody
        (t, className(t, methodClass(t, traceElementMethod(t, e))), 0);
      const int8_t* method = &byteArrayBody
        (t, methodName(t, traceElementMethod(t, e)), 0);
      int line = lineNumber(t, traceElementMethod(t, e), traceElementIp(t, e));

      fprintf(stderr, "  at %s.%s ", class_, method);

      switch (line) {
      case NativeLine:
        fprintf(stderr, "(native)\n");
        break;
      case UnknownLine:
        fprintf(stderr, "(unknown line)\n");
        break;
      default:
        fprintf(stderr, "(line %d)\n", line);
      }
    }
  }

  return 0;
}

void
run(Thread* t, const char* className, int argc, const char** argv)
{
  enter(t, Thread::ActiveState);

  object args = makeObjectArray
    (t, arrayBody(t, t->vm->types, Machine::StringType), argc, true);

  PROTECT(t, args);

  for (int i = 0; i < argc; ++i) {
    object arg = makeString(t, "%s", argv);
    set(t, objectArrayBody(t, args, i), arg);
  }

  run(t, className, "main", "([Ljava/lang/String;)V", 0, args);
}

} // namespace

namespace vm {

object
run(Thread* t, const char* className, const char* methodName,
    const char* methodSpec, object this_, ...)
{
  assert(t, t->state == Thread::ActiveState
         or t->state == Thread::ExclusiveState);

  if (UNLIKELY(t->sp + parameterFootprint(methodSpec) + 1
               > Thread::StackSizeInWords / 2))
  {
    t->exception = makeStackOverflowError(t);
    return 0;
  }

  if (this_) {
    pushObject(t, this_);
  }

  va_list a;
  va_start(a, this_);

  const char* s = methodSpec;
  ++ s; // skip '('
  while (*s and *s != ')') {
    switch (*s) {
    case 'L':
      while (*s and *s != ';') ++ s;
      ++ s;
      pushObject(t, va_arg(a, object));
      break;

    case '[':
      while (*s == '[') ++ s;
      switch (*s) {
      case 'L':
        while (*s and *s != ';') ++ s;
        ++ s;
        break;

      default:
        ++ s;
        break;
      }
      pushObject(t, va_arg(a, object));
      break;
      
    case 'J':
    case 'D':
      ++ s;
      pushLong(t, va_arg(a, uint64_t));
      break;
          
    default:
      ++ s;
      pushInt(t, va_arg(a, uint32_t));
      break;
    }
  }

  va_end(a);

  object class_ = resolveClass(t, makeByteArray(t, "%s", className));
  if (LIKELY(t->exception == 0)) {
    PROTECT(t, class_);

    object name = makeByteArray(t, methodName);
    PROTECT(t, name);

    object spec = makeByteArray(t, methodSpec);
    object reference = makeReference(t, class_, name, spec);
    
    object method = findMethodInClass(t, class_, reference);
    if (LIKELY(t->exception == 0)) {
      assert(t, ((methodFlags(t, method) & ACC_STATIC) == 0) xor (this_ == 0));

      if (methodFlags(t, method) & ACC_NATIVE) {
        unsigned returnCode = invokeNative(t, method);

        if (LIKELY(t->exception == 0)) {
          switch (returnCode) {
          case ByteField:
          case BooleanField:
          case CharField:
          case ShortField:
          case FloatField:
          case IntField:
            return makeInt(t, popInt(t));

          case LongField:
          case DoubleField:
            return makeLong(t, popLong(t));
        
          case ObjectField:
            return popObject(t);

          case VoidField:
            return 0;

          default:
            abort(t);
          };
        }
      } else {
        checkStack(t, method);
        if (LIKELY(t->exception == 0)) {
          pushFrame(t, method);
        }
      }
    }    
  }

  return ::run(t);
}

int
run(System* system, Heap* heap, ClassFinder* classFinder,
    const char* className, int argc, const char** argv)
{
  Machine m(system, heap, classFinder);
  Thread t(&m, 0, 0, 0);

  enter(&t, Thread::ActiveState);

  ::run(&t, className, argc, argv);

  int exitCode = 0;
  if (t.exception) exitCode = -1;

  exit(&t);

  return exitCode;
}

}