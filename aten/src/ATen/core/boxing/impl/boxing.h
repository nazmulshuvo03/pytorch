#pragma once

// This file contains boxing (not unboxing) logic,
// i.e. how to make a vector<IValue> from a set of concrete arguments.

#include <ATen/core/ivalue.h>
#include <c10/core/TensorOptions.h>
#include <ATen/core/Dimname.h>

#include <ATen/core/boxing/KernelFunction.h>

#include <c10/util/Metaprogramming.h>

namespace c10 {
namespace impl {

//
// utils
//

// is_tensor_ref
template<class T> struct is_tensor_ref : std::false_type {};
template <> struct is_tensor_ref<at::Tensor&> : std::true_type {};

// is_tuple_of_tensor_refs
//
template<class T, class Enable = void>
struct is_tuple_of_tensor_refs : std::false_type {};

template<class T>
struct is_tuple_of_tensor_refs<T, std::enable_if_t<guts::is_instantiation_of<std::tuple, T>::value, void>>
: guts::typelist::all<is_tensor_ref, guts::typelist::from_tuple_t<T>>
{};

// has_ivalue_to
//
template<class T, class Enable = void>
struct has_ivalue_to : std::false_type {};

template<class T>
struct has_ivalue_to<T, guts::void_t<decltype(std::declval<IValue>().to<T>())>>
: std::true_type
{};

//
// boxing predicates
//

//
// ilia
//

// A boxable arg type is one that IValue has a constructor for.
// Assume T is decayed
template <typename T>
using ok_to_box = guts::disjunction<
    std::is_constructible<IValue, T>,
    // TensorOptions are not directly constructible into IValue,
    // but torch::jit::push knows how to handle them
    std::is_same<TensorOptions, T>,
    // void returns are ok
    std::is_same<void, T>>;

// TODO boxing should be ok for all kernels. Then remove ok_to_box and supports_boxing.

template <typename Result>
using supports_boxing_result =
  guts::negation<guts::disjunction<
    std::is_lvalue_reference<Result>,
    guts::negation<ok_to_box<Result>>,
    std::is_same<IntArrayRef, Result>
  >>;

//
// /ilia
//

template <typename T>
using can_box =
  guts::disjunction<
    std::is_constructible<IValue, T>,
    // TensorOptions are not directly constructible into IValue,
    // but torch::jit::push knows how to handle them
    std::is_same<TensorOptions, T>
  >;

template <typename... Ts>
using can_box_all = guts::conjunction<can_box<std::decay_t<Ts>>...>;

// an unboxable result is one that can be extracted from an IValue
template <typename T>
using can_unbox =
  guts::conjunction<
    guts::disjunction<
      has_ivalue_to<T>,
      // void returns are ok
      std::is_same<void, T>
    >,
    guts::negation<std::is_lvalue_reference<T>>
  >;

//
// BoxedKernelWrapper
//
// For a given function type FT, BoxedKernelWrapper<FT> implements
// a `call` method that
// - takes a boxed kernel and unboxed arguments as specified by FT
// - boxes the arguments
// - calls the boxed kernel
// - unboxes and returns the result
//
// The partial specializations below handle various cases: in
// particular, not all types appearing in op signatures are supported,
// and ops returning references have nonstandard wrapper implementations.
//

// base definition should never be instantiated.
// A "no call method defined on BoxedKernelWrapper" compile error means that
// an op signature has failed to trigger any of the partial specializations
// that follow.
//
template<class FuncType, class Enable = void>
struct BoxedKernelWrapper {};

// 1. Unsupported type traps.
//
// These specializations capture the remaining gaps in boxing support.
// Rather than triggering compile errors, we generate boxed kernels that
// raise runtime errors. As support for these types is added, the
// specializations can be removed.
//

// at::Dimname
template <class... Args>
using has_dimname_arg =
  guts::disjunction<
    std::is_same<at::Dimname, std::decay_t<Args>>...,
    std::is_same<c10::ArrayRef<at::Dimname>, std::decay_t<Args>>...,
    std::is_same<c10::optional<c10::ArrayRef<at::Dimname>>, std::decay_t<Args>>...
  >;

template <class Result, class... Args>
using supports_boxing =
  guts::conjunction<
    supports_boxing_result<Result>,
    ok_to_box<std::decay_t<Args>>...
  >;

template <typename T, std::enable_if_t<!c10::impl::ok_to_box<T>::value>* = nullptr>
inline bool pushIValueOrCannotBox(std::vector<c10::IValue>& stack, const T& v) {
  torch::jit::push(stack, "cannot box");
  return false;
}
template <typename T, std::enable_if_t<c10::impl::ok_to_box<T>::value>* = nullptr>
inline bool pushIValueOrCannotBox(std::vector<c10::IValue>& stack, const T& v) {
  torch::jit::push(stack, v);
  return true;
}

// boxArgumentsOrCannotBoxIntoStack takes the arguments and pushes them as IValues onto the stack.
// In case the argument cannot be converted to IValue, the function pushes "cannot box"
// IValue string. Return value - whether all of the arguments could be converted to IValues
inline bool boxArgumentsOrCannotBoxIntoStack(std::vector<c10::IValue>& stack) {
  return true;
}
template<typename Item>
inline bool boxArgumentsOrCannotBoxIntoStack(std::vector<c10::IValue>& stack, const Item& item) {
  return pushIValueOrCannotBox(stack, item);
}
template<typename Item, typename... Rest>
inline bool boxArgumentsOrCannotBoxIntoStack(std::vector<c10::IValue>& stack, const Item& item, Rest... other_items) {
  auto res = pushIValueOrCannotBox(stack, item);
  return boxArgumentsOrCannotBoxIntoStack(stack, other_items...) && res;
}

template<class Result>
std::enable_if_t<!supports_boxing_result<Result>::value, Result>
callBoxedFunc(KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, const OperatorHandle& opHandle, torch::jit::Stack& stack) {
  TORCH_INTERNAL_ASSERT(false, "Tried to call KernelFunction::callBoxedFunc() but return result cannot be boxed");
}

template<class Result>
std::enable_if_t<supports_boxing_result<Result>::value && !std::is_same<void, Result>::value, Result>
callBoxedFunc(KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, const OperatorHandle& opHandle, torch::jit::Stack& stack) {
  (*boxed_kernel_func)(functor, opHandle, &stack);
  TORCH_INTERNAL_ASSERT(stack.size() == 1, "A boxed kernel should only push one return to the stack");
  return std::move(stack[0]).to<Result>();
}

template<class Result>
std::enable_if_t<supports_boxing_result<Result>::value && std::is_same<void, Result>::value, Result>
callBoxedFunc(KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, const OperatorHandle& opHandle, torch::jit::Stack& stack) {
  (*boxed_kernel_func)(functor, opHandle, &stack);
  TORCH_INTERNAL_ASSERT(stack.size() == 0, "A boxed kernel returned a value but when we called it with KernelFunction::callBoxedFunc(), we expected it to return void.");
}

template<class Result, class... Args>
struct BoxedKernelWrapper<Result(Args...), std::enable_if_t<has_dimname_arg<Args...>::value, void>> {
  static Result call(KernelFunction::InternalBoxedKernelFunction*, OperatorKernel*, const OperatorHandle&, Args... args) {
    TORCH_INTERNAL_ASSERT(false, "Call to a boxed kernel with unboxable parameter type at::Dimname.");
  }
};

// at::Quantizer
template <class... Args>
using has_quantizer_arg =
  guts::disjunction<
    std::is_same<at::Quantizer, std::decay_t<Args>>...,
    std::is_same<c10::intrusive_ptr<at::Quantizer>, std::decay_t<Args>>...
  >;

//
// ilia
//

template<class Result, class... Args>
std::enable_if_t<supports_boxing<Result, Args...>::value && !std::is_same<void, Result>::value, Result>
boxAndCallBoxedFunc(KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, const OperatorHandle& opHandle, Args... args) {
  // TODO Reuse stack vector instead of allocating?
  torch::jit::Stack stack;
  torch::jit::push(stack, std::forward<Args>(args)...);

  return callBoxedFunc<Result>(boxed_kernel_func, functor, opHandle, stack);
}

//
// /ilia
//

template<class Result, class... Args>
struct BoxedKernelWrapper<Result(Args...), std::enable_if_t<has_quantizer_arg<Args...>::value, void>> {
  static Result call(KernelFunction::InternalBoxedKernelFunction*, OperatorKernel*, const OperatorHandle&, Args... args) {
    TORCH_INTERNAL_ASSERT(false, "Unboxed call to a boxed kernel with unboxable parameter type at::Quantizer.");
  }
};

// 2. Supported signatures, other than ref-passing.
//

//
// ilia
//

template<class Result, class... Args>
std::enable_if_t<supports_boxing<Result, Args...>::value && std::is_same<void, Result>::value, Result>
boxAndCallBoxedFunc(KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func, OperatorKernel* functor, const OperatorHandle& opHandle, Args... args) {
  // TODO Reuse stack vector instead of allocating?
  torch::jit::Stack stack;
  torch::jit::push(stack, std::forward<Args>(args)...);

  callBoxedFunc<Result>(boxed_kernel_func, functor, opHandle, stack);
}

//
// /ilia
//

template<class Result, class... Args>
struct BoxedKernelWrapper<
  Result(Args...),
  std::enable_if_t<
    can_box_all<Args...>::value && can_unbox<Result>::value && !is_tuple_of_tensor_refs<Result>::value,
    void
  >
> {
  static Result call(
    KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func,
    OperatorKernel* functor,
    const OperatorHandle& opHandle,
    Args... args
  ) {
    // TODO Reuse stack vector instead of allocating?
    torch::jit::Stack stack;
    stack.reserve(sizeof...(Args));
    torch::jit::push(stack, std::forward<Args>(args)...);

    (*boxed_kernel_func)(functor, opHandle, &stack);

    return guts::if_constexpr<!std::is_same<void, Result>::value>(
      [&] (auto delay_check) {
        TORCH_INTERNAL_ASSERT(
          stack.size() == 1,
          "Boxed kernel was expected to push exactly one return value to the stack."
        );
        return delay_check(std::move(stack[0]).to<Result>());
      },
      [&] {
        TORCH_INTERNAL_ASSERT(
          stack.size() == 0,
          "Boxed kernel for op with void return type pushed one or more return values to the stack."
        );
      }
    );
  }
};

// 3. signatures returning a single reference of the same type as
// their initial argument.
//
// Note that the passed kernels are assumed to be for inplace/outplace ops,
// and the generated BoxedKernelWrapper specializations will simply return
// the initial argument.
//
template<class Result, class... OtherArgs>
struct BoxedKernelWrapper<
  Result(Result, OtherArgs...),
  std::enable_if_t<
    can_box_all<Result, OtherArgs...>::value && is_tensor_ref<Result>::value,
    void
  >
> {
  static Result call(
    KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func,
    OperatorKernel* functor,
    const OperatorHandle& opHandle,
    Result outArg,
    OtherArgs... otherArgs
  ) {
    // TODO Reuse stack vector instead of allocating?
    torch::jit::Stack stack;
    stack.reserve(1 + sizeof...(OtherArgs));
    torch::jit::push_one(stack, outArg);
    torch::jit::push(stack, std::forward<OtherArgs>(otherArgs)...);

    (*boxed_kernel_func)(functor, opHandle, &stack);

    return outArg;
  }
};

// 4. signatures returning a tuple of Tensor references.
// Note that the passed kernels are assumed to be for inplace/outplace ops,
// and the generated BoxedKernelWrapper specializations will return a tuple
// of those initial arguments.
//
template<class Result, class... Args>
struct BoxedKernelWrapper<
  Result(Args...),
  std::enable_if_t<
    can_box_all<Args...>::value && is_tuple_of_tensor_refs<Result>::value,
    void
  >
> {
  static Result call(
    KernelFunction::InternalBoxedKernelFunction* boxed_kernel_func,
    OperatorKernel* functor,
    const OperatorHandle& opHandle,
    Args... args
  ) {
    // TODO Reuse stack vector instead of allocating?
    torch::jit::Stack stack;
    stack.reserve(sizeof...(Args));
    torch::jit::push(stack, std::forward<Args>(args)...);

    (*boxed_kernel_func)(functor, opHandle, &stack);

    using ArgTuple = std::tuple<Args...>;
    constexpr int n = std::tuple_size<Result>();
    auto result = guts::tuple_take<ArgTuple, n>(ArgTuple{args...});
    static_assert(
        std::is_same<Result, decltype(result)>::value,
        "The parameter list of an op returning a tuple of Tensor references "
            "must begin with an equal number of Tensor reference parameters."
    );
    return result;
  }
};

} // impl
} // c10
