#pragma once

#include <array>
#include <initializer_list>
#include <ranges>
#include <type_traits>
#include <vector>
#include <memory_resource>

#include "utils/Assert.h" // IWYU pragma: keep

namespace utils {

template <typename T>
void move_vector(std::pmr::vector<T>& from, std::pmr::vector<T>& to) {
   to.reserve(from.size());
   to.insert(to.end(),
             std::make_move_iterator(from.begin()),
             std::make_move_iterator(from.end()));
}

// Remove a pointer or reference from a type.
template <typename T>
using remove_ptr_or_ref_t = std::conditional_t<
      /* Condition */ std::is_pointer_v<T>,
      /* T* -> T */ std::remove_pointer_t<T>,
      /* T& -> T, or T -> T */ std::remove_reference_t<T>>;

// Transfer const from one type to another.
template <typename To, typename From>
using transfer_const_t = std::conditional_t<
      /* Condition */ std::is_const_v<From>,
      /* From const -> To const */ std::add_const_t<To>, To>;

// Transfer volatile from one type to another.
template <typename To, typename From>
using transfer_volatile_t = std::conditional_t<
      /* Condition */ std::is_volatile_v<From>,
      /* From volatile -> To volatile */ std::add_volatile_t<To>, To>;

// Transfer const and volatile from one type to another.
template <typename To, typename From>
using transfer_cv_t = transfer_volatile_t<transfer_const_t<To, From>, From>;

// Merge const and volatile from two types into U.
template <typename U, typename V>
using union_cv_t = transfer_cv_t<transfer_cv_t<U, V>, U>;

// Transfer pointer or reference from one type to another.
template <typename To, typename From>
using transfer_ptr_or_ref_t = std::conditional_t<
      /* Condition */ std::is_pointer_v<From>,
      /* From* -> To* */ std::add_pointer_t<To>,
      std::conditional_t<
            /* Condition */ std::is_reference_v<From>,
            /* From& -> To& */ std::add_lvalue_reference_t<To>,
            /* From -> To */ To>>;

// Checks if type From can be cast to type To.
template <typename To, typename From>
consteval bool is_valid_type_cast_f() {
   constexpr bool c1 = std::is_class_v<remove_ptr_or_ref_t<To>>;
   static_assert(c1, "To must be a class or a pointer to a class");
   constexpr bool c2 = std::is_class_v<remove_ptr_or_ref_t<From>>;
   static_assert(c2, "From must be a class or a pointer to a class");
   constexpr bool c3 = std::is_pointer_v<From> || std::is_reference_v<From>;
   static_assert(c3, "From must be a pointer or a reference");
   constexpr bool c4 = !(std::is_pointer_v<From> && std::is_reference_v<To>);
   static_assert(c4, "From* -> To& is illegal");
   constexpr bool c5 = !(std::is_reference_v<From> && std::is_pointer_v<To>);
   static_assert(c5, "From& -> To* is illegal");
   constexpr bool c6 = !std::is_reference_v<To>;
   static_assert(c6, "Casting to a reference is illegal");
   return c1 && c2 && c3 && c4 && c5 && c6;
}

} // namespace utils

/**
 * @brief Given a type To and a type From, returns the canonicalized type.
 * Strips To and From of their pointer and reference types, and unions
 * const and volatile between From and To.
 */
template <typename To, typename From>
using canonicalize_t = utils::union_cv_t<utils::remove_ptr_or_ref_t<To>,
                                         utils::remove_ptr_or_ref_t<From>>;

/**
 * @brief Casts a pointer of type From to a pointer type of: To or To*.
 * Will assert, if the cast is invalid.
 *
 * @tparam To A class or pointer to a class type.
 * @tparam From A pointer to a class type.
 */
template <typename To, typename From>
/* To* */ std::enable_if_t<std::is_pointer_v<From>, canonicalize_t<To, From>*>
cast(From from) {
   if constexpr(utils::is_valid_type_cast_f<To, From>()) {
      assert(from && "Tried to cast a nullptr");
      auto ptr = dynamic_cast<canonicalize_t<To, From>*>(from);
      assert(ptr && "Invalid cast");
      return ptr;
   }
   return nullptr;
}

/**
 * @brief Casts a reference of type From to a pointer of type of: To or To*.
 * Same as cast, but for references.
 *
 * @tparam To A class or pointer to a class type.
 * @tparam From A reference to a class type.
 */
template <typename To, typename From>
/* To* */ std::enable_if_t<!std::is_pointer_v<From>, canonicalize_t<To, From>*>
cast(From& from) {
   return cast<To>(&from);
}

/**
 * @brief Casts a pointer of type From to a pointer type of: To or To*.
 * Will assert, if the from pointer is nullptr. Will return
 * nullptr if the cast is invalid.
 *
 * @tparam To A class or pointer to a class type.
 * @tparam From A pointer to a class type.
 */
template <typename To, typename From>
/* To* */ std::enable_if_t<std::is_pointer_v<From>, canonicalize_t<To, From>*>
dyn_cast(From from) {
   if constexpr(utils::is_valid_type_cast_f<To, From>()) {
      assert(from && "Tried to dyn_cast a nullptr");
      return dynamic_cast<canonicalize_t<To, From>*>(from);
   }
   return nullptr;
}

/**
 * @brief Casts a reference of type From to a pointer type of: To or To*.
 * Same as dyn_cast, but for references.
 *
 * @tparam To A class or pointer to a class type.
 * @tparam From A reference to a class type.
 */
template <typename To, typename From>
/* To* */ std::enable_if_t<!std::is_pointer_v<From>, canonicalize_t<To, From>*>
dyn_cast(From& from) {
   return dyn_cast<To>(&from);
}

/**
 * @brief Same as dyn_cast, but returns nullptr if the from pointer is nullptr.
 *
 * @tparam To A class or pointer to a class type.
 * @tparam From A pointer to a class type.
 */
template <typename To, typename From>
/* To* */ std::enable_if_t<std::is_pointer_v<From>, canonicalize_t<To, From>*>
dyn_cast_or_null(From from) {
   if constexpr(utils::is_valid_type_cast_f<To, From>()) {
      return dynamic_cast<canonicalize_t<To, From>*>(from);
   }
   return nullptr;
}

/* ===--------------------------------------------------------------------=== */
// function_ref taken from here:
// https://vittorioromeo.info/index/blog/passing_functions_to_functions.html
/* ===--------------------------------------------------------------------=== */

namespace utils::details {

template <typename...>
using void_t = void;

template <class T, class R = void, class = void>
struct is_callable : std::false_type {};

template <class T>
struct is_callable<T, void, void_t<std::result_of_t<T>>> : std::true_type {};

template <class T, class R>
struct is_callable<T, R, void_t<std::result_of_t<T>>>
      : std::is_convertible<std::result_of_t<T>, R> {};

template <typename TSignature>
struct signature_helper;

template <typename TReturn, typename... TArgs>
struct signature_helper<TReturn(TArgs...)> {
   using fn_ptr_type = TReturn (*)(TArgs...);
};

template <typename TSignature>
using fn_ptr = typename signature_helper<TSignature>::fn_ptr_type;

template <typename T>
struct dependent_false : std::false_type {};

template <typename TSignature>
class function_ref;

template <typename TReturn, typename... TArgs>
class function_ref<TReturn(TArgs...)> final {
private:
   using signature_type = TReturn(void*, TArgs...);
   void* _ptr;
   TReturn (*_erased_fn)(void*, TArgs...);

public:
   template <typename T, typename = std::enable_if_t<
                               is_callable<T&(TArgs...)>{} &&
                               !std::is_same<std::decay_t<T>, function_ref>{}>>
   function_ref(T&& x) noexcept : _ptr{(void*)std::addressof(x)} {
      _erased_fn = [](void* ptr, TArgs... xs) -> TReturn {
         return (*reinterpret_cast<std::add_pointer_t<T>>(ptr))(
               std::forward<TArgs>(xs)...);
      };
   }
   decltype(auto) operator()(TArgs... xs) const
         noexcept(noexcept(_erased_fn(_ptr, std::forward<TArgs>(xs)...))) {
      return _erased_fn(_ptr, std::forward<TArgs>(xs)...);
   }
};

} // namespace utils::details

namespace utils {

/**
 * @brief A non-owning, lightweight view of a range whose element types are
 * convertible to T
 *
 * @tparam T The type of the elements in the range
 */
template <typename T>
class range_ref {
public:
   range_ref() {
      range_ = nullptr;
      foreach_ = nullptr;
      sz_ = 0;
   }

   template <typename U, class Tp>
      requires std::convertible_to<U, T>
   range_ref(std::vector<U, Tp>& vec) {
      range_ = const_cast<void*>(static_cast<void const*>(&vec));
      foreach_ = [](void* r, details::function_ref<void(T)> callback) {
         for(auto&& v : *reinterpret_cast<decltype(&vec)>(r)) callback(v);
      };
      sz_ = vec.size();
   }

   template <std::ranges::view R>
      requires std::convertible_to<std::ranges::range_value_t<R>, T>
   range_ref(R&& range) {
      range_ = const_cast<void*>(static_cast<void const*>(&range));
      foreach_ = [](void* r, details::function_ref<void(T)> callback) {
         for(auto&& v : *reinterpret_cast<decltype(&range)>(r)) callback(v);
      };
      sz_ = std::ranges::size(range);
   }

   // Unlike the vector/view constructors above (which store a non-owning
   // pointer to a container that outlives the call), a braced-init-list's
   // backing array dies at the end of the full-expression. Iterating it lazily
   // in for_each would read freed memory, so we eagerly copy the elements into
   // an inline buffer and mark the view as owning.
   template <typename U>
      requires std::convertible_to<U, T>
   range_ref(std::initializer_list<U>&& list) {
      assert(list.size() <= kInlineCapacity &&
             "range_ref initializer_list exceeds inline buffer capacity");
      std::size_t i = 0;
      for(auto&& v : list) inl_[i++] = v;
      sz_ = list.size();
      owned_ = true;
   }

   inline void for_each(details::function_ref<void(T)> callback) {
      if(owned_) {
         for(std::size_t i = 0; i < sz_; ++i) callback(inl_[i]);
      } else if(foreach_) {
         foreach_(range_, callback);
      }
   }

   inline std::size_t size() const { return sz_; }

private:
   using range_fun_t = void (*)(void*, details::function_ref<void(T)>);
   static constexpr std::size_t kInlineCapacity = 8;
   void* range_ = nullptr;
   range_fun_t foreach_ = nullptr;
   std::size_t sz_ = 0;
   // Storage for the owning (initializer_list) case; empty otherwise.
   std::array<T, kInlineCapacity> inl_{};
   bool owned_ = false;
};

/**
 * @brief Converts a given std::tuple into an std::array
 *
 * @tparam T The type of the tuple
 * @param tuple The tuple to convert
 * @return constexpr auto The resulting std::array<...>
 */
template <typename T>
constexpr auto array_from_tuple(T&& tuple) {
   constexpr auto get_array = [](auto&&... x) {
      return std::array{std::forward<decltype(x)>(x)...};
   };
   return std::apply(get_array, std::forward<T>(tuple));
}

/**
 * @brief 
 * 
 * @tparam T 
 * @tparam V 
 * @param f 
 * @param tuple 
 * @return consteval 
 */
template <typename T, typename V>
consteval auto map_tuple(T f, V tuple) {
   return std::apply(
         [f](auto... x) {
            return std::tuple_cat([f](auto x0) { return f(x0); }(x)...);
         },
         tuple);
}

/**
 * @brief 
 * 
 * @tparam T 
 * @tparam N 
 */
template <class T, std::size_t N>
concept has_tuple_element = requires(T t) {
   typename std::tuple_element_t<N, std::remove_const_t<T>>;
   { get<N>(t) } -> std::convertible_to<const std::tuple_element_t<N, T>&>;
};

/**
 * @brief 
 * 
 * @tparam T 
 */
template <class T>
concept tuple_like = !std::is_reference_v<T> && requires(T t) {
   typename std::tuple_size<T>::type;
   requires std::derived_from<
         std::tuple_size<T>,
         std::integral_constant<std::size_t, std::tuple_size_v<T>>>;
} && []<std::size_t... N>(std::index_sequence<N...>) {
   return (has_tuple_element<T, N> && ...);
}(std::make_index_sequence<std::tuple_size_v<T>>());

} // namespace utils
