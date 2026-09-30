/*
 * MVKSmallVector.h
 *
 * Copyright (c) 2012-2026 Dr. Torsten Hans (hans@ipacs.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// In case MVKSmallVector should use just std::vector instead
#if 0

template<typename T, size_t N = 0>
using MVKSmallVector = std::vector<T>;

#else

// MVKSmallVector.h is a sequence container that (optionally) implements a small buffer optimization.
// It behaves similarly to std::vector, except until a certain number of elements are reserved,
// it does not use the heap. Like std::vector, MVKSmallVector is guaranteed to use contiguous memory,
// so if the preallocated number of elements are exceeded, all elements are then in heap.
// MVKSmallVector supports just the necessary members to be compatible with MoltenVK.
// If C++17 will be the default in the future, code can be simplified quite a bit.
//
// Example:
//
//  MVKSmallVector<int, 2> sv;
//  sv.push_back( 1 );  // no allocation, uses pre-allocated memory
//  sv.push_back( 2 );	// no allocation, uses pre-allocated memory
//  sv.push_back( 3 );	// adding another element now reserves memory from heap and copies from pre-allocated memory
//
// If you don't need any inline storage use:
//  MVKSmallVector<int> v;   // this is essentially the same as using std::vector
//
// The per-instance memory overhead of MVKSmallVector (16 bytes) is smaller than std::vector
// (24 bytes), but MVKSmallVector lacks the polymorphism of std::vector, that allows it to
// be passed to functions without reference to the pre-allocation size. MVKSmallVector
// supports the contents() function to derive an MVKArrayRef from its contents, which can
// be passed to functions without reference to the MVKSmallVector pre-allocaton size.

#include "MVKSmallVectorAllocator.h"
#include "MVKFoundation.h"
#include <type_traits>
#include <algorithm>
#include <initializer_list>
#include <utility>


template<typename Type, typename Allocator = mvk_smallvector_allocator<Type, 0>>
class MVKSmallVectorImpl
{
  Allocator  alc;
  
public:
  using value_type = Type;

  // Iterators are plain pointers into the contiguous storage. Like the pointers returned
  // by data(), they are invalidated by any operation that grows the storage.
  using iterator = Type*;
  using const_iterator = const Type*;
  using reverse_iterator = std::reverse_iterator<iterator>;

private:
  // this is the growth strategy -> adjust to your needs
  size_t vector_GetNextCapacity() const
  {
    constexpr auto ELEMENTS_FOR_64_BYTES = 64 / sizeof( Type );
    constexpr auto MINIMUM_CAPACITY = ELEMENTS_FOR_64_BYTES > 4 ? ELEMENTS_FOR_64_BYTES : 4;
    const auto current_capacity = capacity();
    return MINIMUM_CAPACITY + ( 3 * current_capacity ) / 2;
  }

  void vector_Allocate( const size_t s )
  {
    const auto new_reserved_size = s > size() ? s : size();

    alc.allocate( new_reserved_size );
  }

  void vector_ReAllocate( const size_t s )
  {
    alc.re_allocate( s );
  }

public:
  MVKSmallVectorImpl()
  {
  }

  MVKSmallVectorImpl( const size_t n, const Type t = { } )
  {
    if( n > 0 )
    {
      alc.allocate( n );

      for( size_t i = 0; i < n; ++i )
      {
        alc.construct( &alc.ptr[i], t );
      }

      alc.num_elements_used = n;
    }
  }

  MVKSmallVectorImpl( const MVKSmallVectorImpl &a )
  {
    const size_t n = a.size();

    if( n > 0 )
    {
      alc.allocate( n );

      if constexpr( std::is_trivially_copyable_v<Type> )
      {
        memcpy( static_cast<void*>( alc.ptr ), static_cast<const void*>( a.alc.ptr ), n * sizeof( Type ) );
      }
      else
      {
        for( size_t i = 0; i < n; ++i )
        {
          alc.construct( &alc.ptr[i], a.alc.ptr[i] );
        }
      }

      alc.num_elements_used = n;
    }
  }

  template<typename U>
  MVKSmallVectorImpl( const U &a )
  {
    const size_t n = a.size();

    if( n > 0 )
    {
      alc.allocate( n );

      for( size_t i = 0; i < n; ++i )
      {
        alc.construct( &alc.ptr[i], a[i] );
      }

      alc.num_elements_used = n;
    }
  }

  MVKSmallVectorImpl( MVKSmallVectorImpl &&a ) noexcept : alc{ std::move( a.alc ) }
  {
  }

  MVKSmallVectorImpl( std::initializer_list<Type> vector )
  {
    if( vector.size() > capacity() )
    {
      vector_Allocate( vector.size() );
    }

    // std::initializer_list does not yet support std::move, we use it anyway but it has no effect
    for( auto &&element : vector )
    {
      alc.construct( &alc.ptr[alc.num_elements_used], std::move( element ) );
      ++alc.num_elements_used;
    }
  }

  ~MVKSmallVectorImpl()
  {
  }

  MVKSmallVectorImpl& operator=( const MVKSmallVectorImpl &a )
  {
    if( this != &a )
    {
      const auto n = a.size();

      if constexpr( std::is_trivially_copyable_v<Type> )
      {
        if( n > capacity() )
        {
          alc.num_elements_used = 0;
          vector_ReAllocate( n );
        }
        if( n ) { memcpy( static_cast<void*>( alc.ptr ), static_cast<const void*>( a.alc.ptr ), n * sizeof( Type ) ); }
        alc.num_elements_used = n;
      }
      else if( alc.num_elements_used == n )
      {
        for( size_t i = 0; i < n; ++i )
        {
          alc.ptr[i] = a.alc.ptr[i];
        }
      }
      else
      {
        alc.template destruct_all<Type>();

        if( n > capacity() )
        {
          vector_ReAllocate( n );
        }

        for( size_t i = 0; i < n; ++i )
        {
          alc.construct( &alc.ptr[i], a.alc.ptr[i] );
        }

        alc.num_elements_used = n;
      }
    }

    return *this;
  }

  MVKSmallVectorImpl& operator=( MVKSmallVectorImpl &&a ) noexcept
  {
    alc.swap( a.alc );
    return *this;
  }

  bool operator==( const MVKSmallVectorImpl &a ) const
  {
    if( alc.num_elements_used != a.alc.num_elements_used )
      return false;
    for( size_t i = 0; i < alc.num_elements_used; ++i )
    {
      if( alc[i] != a.alc[i] )
        return false;
    }
    return true;
  }

  bool operator!=( const MVKSmallVectorImpl &a ) const
  {
    if( alc.num_elements_used != a.alc.num_elements_used )
      return true;
    for( size_t i = 0; i < alc.num_elements_used; ++i )
    {
      if( alc.ptr[i] != a.alc[i] )
        return true;
    }
    return false;
  }

  void swap( MVKSmallVectorImpl &a ) noexcept
  {
    alc.swap( a.alc );
  }

  // Removes the elements that satisfy the predicate, and returns how many were removed.
  // Found by argument-dependent lookup, like std::erase_if() for the standard containers.
  template<class Pred>
  friend size_t erase_if( MVKSmallVectorImpl &v, Pred pred )
  {
    auto new_end = std::remove_if( v.begin(), v.end(), pred );
    size_t removed = v.end() - new_end;
    v.erase( new_end, v.end() );
    return removed;
  }

  // As before, a const vector hands out mutable iterators; the elements are not treated as part of its constness.
  iterator begin() const { return alc.ptr; }
  iterator end()   const { return alc.ptr + alc.num_elements_used; }

  reverse_iterator rbegin() const { return reverse_iterator( end() ); }
  reverse_iterator rend()   const { return reverse_iterator( begin() ); }

  MVKArrayRef<const Type> contents() const { return MVKArrayRef<const Type>(data(), size()); }
  MVKArrayRef<      Type> contents()       { return MVKArrayRef<      Type>(data(), size()); }

  const Type &operator[]( const size_t i ) const  { return alc[i]; }
        Type &operator[]( const size_t i )        { return alc[i]; }
  const Type &at( const size_t i )         const  { return alc[i]; }
        Type &at( const size_t i )                { return alc[i]; }
  const Type &front()                      const  { return alc[0]; }
        Type &front()                             { return alc[0]; }
  const Type &back()                       const  { return alc[alc.num_elements_used - 1]; }
        Type &back()                              { return alc[alc.num_elements_used - 1]; }
  const Type *data()                       const  { return alc.ptr; }
        Type *data()                              { return alc.ptr; }

  size_t      size()                       const { return alc.num_elements_used; }
  size_t      byteSize()                   const { return size() * sizeof(Type); }
  bool        empty()                      const { return alc.num_elements_used == 0; }
  size_t      capacity()                   const { return alc.get_capacity(); }

  void pop_back()
  {
    if( alc.num_elements_used > 0 )
    {
      --alc.num_elements_used;
      alc.destruct( &alc.ptr[alc.num_elements_used] );
    }
  }

  void clear()
  {
    alc.template destruct_all<Type>();
  }

  void reset()
  {
    alc.deallocate();
  }

  void reserve( const size_t new_size )
  {
    if( new_size > capacity() )
    {
      vector_ReAllocate( new_size );
    }
  }

  void assign( const size_t new_size, const Type &t )
  {
    if( new_size <= capacity() )
    {
      clear();
    }
    else
    {
      vector_Allocate( new_size );
    }

    for( size_t i = 0; i < new_size; ++i )
    {
      alc.construct( &alc.ptr[i], t );
    }

    alc.num_elements_used = new_size;
  }

  template <class InputIterator>
  void assign( InputIterator first, InputIterator last )
  {
    clear();

    while( first != last )
    {
      emplace_back( *first );
      ++first;
    }
  }

  void resize( const size_t new_size, const Type t = { } )
  {
    if( new_size == alc.num_elements_used )
    {
      return;
    }

    if( new_size == 0 )
    {
      clear();
      return;
    }

    if( new_size > alc.num_elements_used )
    {
      if( new_size > capacity() )
      {
        vector_ReAllocate( new_size );
      }

      while( alc.num_elements_used < new_size )
      {
        alc.construct( &alc.ptr[alc.num_elements_used], t );
        ++alc.num_elements_used;
      }
    }
    else
    {
      if constexpr( std::is_trivially_destructible_v<Type> )
      {
        alc.num_elements_used = new_size;
      }
      else
      {
        while( alc.num_elements_used > new_size )
        {
          --alc.num_elements_used;
          alc.destruct( &alc.ptr[alc.num_elements_used] );
        }
      }
    }
  }

  // trims the capacity of the slist to the number of alc.ptr
  void shrink_to_fit()
  {
    alc.shrink_to_fit();
  }

  void erase( const iterator it )
  {
    if( it < end() )
    {
      --alc.num_elements_used;

      for( size_t i = it - alc.ptr; i < alc.num_elements_used; ++i )
      {
        alc.ptr[i] = std::move( alc.ptr[i + 1] );
      }

      // this is required for types with a destructor
      alc.destruct( &alc.ptr[alc.num_elements_used] );
    }
  }

  void erase( const iterator first, const iterator last )
  {
    if( first < end() )
    {
      size_t first_pos = first - alc.ptr;
      size_t last_pos = last < end() ? last - alc.ptr : size();
      size_t n = last_pos - first_pos;
      if( n == 0 ) { return; }    // An empty range removes nothing, and must not move elements onto themselves.
      alc.num_elements_used -= n;

      for( size_t i = first_pos, e = last_pos; i < alc.num_elements_used && e < alc.num_elements_used + n; ++i, ++e )
      {
        alc.ptr[i] = std::move( alc.ptr[e] );
      }

      // this is required for types with a destructor
      for( size_t i = alc.num_elements_used; i < alc.num_elements_used + n; ++i )
      {
        alc.destruct( &alc.ptr[i] );
      }
    }
  }

  // adds t before it and automatically resizes vector if necessary
  void insert( const iterator it, Type t )
  {
    if( it >= end() || alc.num_elements_used == 0 )
    {
      push_back( std::move( t ) );
    }
    else
    {
      // Take the position before growing the storage, which invalidates the iterator.
      const size_t it_position = it - alc.ptr;

      if( alc.num_elements_used == capacity() )
        vector_ReAllocate( vector_GetNextCapacity() );

      // move construct last element
      alc.construct( &alc.ptr[alc.num_elements_used], std::move( alc.ptr[alc.num_elements_used - 1] ) );

      // move the remaining elements
      for( size_t i = alc.num_elements_used - 1; i > it_position; --i )
      {
        alc.ptr[i] = std::move( alc.ptr[i - 1] );
      }

      alc.ptr[it_position] = std::move( t );
      ++alc.num_elements_used;
    }
  }

  void push_back( const Type &t )
  {
    if( alc.num_elements_used == capacity() )
      vector_ReAllocate( vector_GetNextCapacity() );

    alc.construct( &alc.ptr[alc.num_elements_used], t );
    ++alc.num_elements_used;
  }

  void push_back( Type &&t )
  {
    if( alc.num_elements_used == capacity() )
      vector_ReAllocate( vector_GetNextCapacity() );

    alc.construct( &alc.ptr[alc.num_elements_used], std::move( t ) );
    ++alc.num_elements_used;
  }

  template<class... Args>
  Type &emplace_back( Args&&... args )
  {
    if( alc.num_elements_used == capacity() )
      vector_ReAllocate( vector_GetNextCapacity() );

    alc.construct( &alc.ptr[alc.num_elements_used], std::forward<Args>( args )... );
    ++alc.num_elements_used;

    return alc.ptr[alc.num_elements_used - 1];
  }
};

// specialization for pointer types
template<typename Type, typename Allocator>
class MVKSmallVectorImpl<Type*, Allocator>
{

  Allocator  alc;

public:
  using value_type = Type*;

  // Iterators are plain pointers into the contiguous storage. Like the pointers returned
  // by data(), they are invalidated by any operation that grows the storage.
  using iterator = Type**;
  using const_iterator = Type* const*;
  using reverse_iterator = std::reverse_iterator<iterator>;

private:
  // this is the growth strategy -> adjust to your needs
  size_t vector_GetNextCapacity() const
  {
    constexpr auto ELEMENTS_FOR_64_BYTES = 64 / sizeof( Type* );
    constexpr auto MINIMUM_CAPACITY = ELEMENTS_FOR_64_BYTES > 4 ? ELEMENTS_FOR_64_BYTES : 4;
    const auto current_capacity = capacity();
    return MINIMUM_CAPACITY + ( 3 * current_capacity ) / 2;
  }

  void vector_Allocate( const size_t s )
  {
    const auto new_reserved_size = s > size() ? s : size();

    alc.allocate( new_reserved_size );
  }

  void vector_ReAllocate( const size_t s )
  {
    alc.re_allocate( s );
  }

public:
  MVKSmallVectorImpl()
  {
  }

  MVKSmallVectorImpl( const size_t n, Type *t = nullptr )
  {
    if ( n > 0 )
    {
      alc.allocate( n );

      for ( size_t i = 0; i < n; ++i )
      {
        alc.ptr[i] = t;
      }

      alc.num_elements_used = n;
    }
  }

  MVKSmallVectorImpl( const MVKSmallVectorImpl &a )
  {
    const size_t n = a.size();

    if ( n > 0 )
    {
      alc.allocate( n );

      for ( size_t i = 0; i < n; ++i )
      {
        alc.ptr[i] = a.alc.ptr[i];
      }

      alc.num_elements_used = n;
    }
  }

  MVKSmallVectorImpl( MVKSmallVectorImpl &&a ) noexcept : alc{ std::move( a.alc ) }
  {
  }

  MVKSmallVectorImpl( std::initializer_list<Type*> vector )
  {
    if ( vector.size() > capacity() )
    {
      vector_Allocate( vector.size() );
    }

    // std::initializer_list does not yet support std::move, we use it anyway but it has no effect
    for ( auto element : vector )
    {
      alc.ptr[alc.num_elements_used] = element;
      ++alc.num_elements_used;
    }
  }

  ~MVKSmallVectorImpl()
  {
  }

  MVKSmallVectorImpl& operator=( const MVKSmallVectorImpl &a )
  {
    if ( this != &a )
    {
      const auto n = a.size();

      if ( n > capacity() )
      {
        vector_ReAllocate( n );
      }

      for ( size_t i = 0; i < n; ++i )
      {
        alc.ptr[i] = a.alc.ptr[i];
      }

      alc.num_elements_used = n;
    }

    return *this;
  }

  MVKSmallVectorImpl& operator=( MVKSmallVectorImpl &&a ) noexcept
  {
    alc.swap( a.alc );
    return *this;
  }

  bool operator==( const MVKSmallVectorImpl &a ) const
  {
    if ( alc.num_elements_used != a.alc.num_elements_used )
      return false;
    for ( size_t i = 0; i < alc.num_elements_used; ++i )
    {
      if ( alc[i] != a.alc[i] )
        return false;
    }
    return true;
  }

  bool operator!=( const MVKSmallVectorImpl &a ) const
  {
    if ( alc.num_elements_used != a.alc.num_elements_used )
      return true;
    for ( size_t i = 0; i < alc.num_elements_used; ++i )
    {
      if ( alc.ptr[i] != a.alc[i] )
        return true;
    }
    return false;
  }

  void swap( MVKSmallVectorImpl &a ) noexcept
  {
    alc.swap( a.alc );
  }

  // Removes the elements that satisfy the predicate, and returns how many were removed.
  // Found by argument-dependent lookup, like std::erase_if() for the standard containers.
  template<class Pred>
  friend size_t erase_if( MVKSmallVectorImpl &v, Pred pred )
  {
    auto new_end = std::remove_if( v.begin(), v.end(), pred );
    size_t removed = v.end() - new_end;
    v.erase( new_end, v.end() );
    return removed;
  }

  // As before, a const vector hands out mutable iterators; the elements are not treated as part of its constness.
  iterator begin() const { return alc.ptr; }
  iterator end()   const { return alc.ptr + alc.num_elements_used; }

  reverse_iterator rbegin() const { return reverse_iterator( end() ); }
  reverse_iterator rend()   const { return reverse_iterator( begin() ); }

  const MVKArrayRef<Type*> contents() const { return MVKArrayRef<Type*>(data(), size()); }
        MVKArrayRef<Type*> contents()       { return MVKArrayRef<Type*>(data(), size()); }

  const Type * const  at( const size_t i )         const { return alc[i]; }
        Type *       &at( const size_t i )               { return alc[i]; }
  const Type * const  operator[]( const size_t i ) const { return alc[i]; }
        Type *       &operator[]( const size_t i )       { return alc[i]; }
  const Type * const  front()                      const { return alc[0]; }
        Type *       &front()                            { return alc[0]; }
  const Type * const  back()                       const { return alc[alc.num_elements_used - 1]; }
        Type *       &back()                             { return alc[alc.num_elements_used - 1]; }
  const Type * const *data()                       const { return alc.ptr; }
        Type *       *data()                             { return alc.ptr; }

  size_t   size()                                  const { return alc.num_elements_used; }
  bool     empty()                                 const { return alc.num_elements_used == 0; }
  size_t   capacity()                              const { return alc.get_capacity(); }

  void pop_back()
  {
    if ( alc.num_elements_used > 0 )
    {
      --alc.num_elements_used;
    }
  }

  void clear()
  {
    alc.num_elements_used = 0;
  }

  void reset()
  {
    alc.deallocate();
  }

  void reserve( const size_t new_size )
  {
    if ( new_size > capacity() )
    {
      vector_ReAllocate( new_size );
    }
  }

  void assign( const size_t new_size, const Type *t )
  {
    if ( new_size <= capacity() )
    {
      clear();
    }
    else
    {
      vector_Allocate( new_size );
    }

    for ( size_t i = 0; i < new_size; ++i )
    {
      alc.ptr[i] = const_cast< Type* >( t );
    }

    alc.num_elements_used = new_size;
  }

  template <class InputIterator>
  void assign( InputIterator first, InputIterator last )
  {
    clear();

    while( first != last )
    {
      push_back( *first );
      ++first;
    }
  }

  void resize( const size_t new_size, const Type *t = nullptr )
  {
    if ( new_size == alc.num_elements_used )
    {
      return;
    }

    if ( new_size == 0 )
    {
      clear();
      return;
    }

    if ( new_size > alc.num_elements_used )
    {
      if ( new_size > capacity() )
      {
        vector_ReAllocate( new_size );
      }

      while ( alc.num_elements_used < new_size )
      {
        alc.ptr[alc.num_elements_used] = const_cast< Type* >( t );
        ++alc.num_elements_used;
      }
    }
    else
    {
      alc.num_elements_used = new_size;
    }
  }

  // trims the capacity of the MVKSmallVectorImpl to the number of used elements
  void shrink_to_fit()
  {
    alc.shrink_to_fit();
  }

  void erase( const iterator it )
  {
    if ( it < end() )
    {
      --alc.num_elements_used;

      for ( size_t i = it - alc.ptr; i < alc.num_elements_used; ++i )
      {
        alc.ptr[i] = alc.ptr[i + 1];
      }
    }
  }

  void erase( const iterator first, const iterator last )
  {
    if( first < end() )
    {
      size_t first_pos = first - alc.ptr;
      size_t last_pos = last < end() ? last - alc.ptr : size();
      size_t n = last_pos - first_pos;
      if( n == 0 ) { return; }
      alc.num_elements_used -= n;

      for( size_t i = first_pos, e = last_pos; i < alc.num_elements_used && e < alc.num_elements_used + n; ++i, ++e )
      {
        alc.ptr[i] = alc.ptr[e];
      }
    }
  }

  // adds t before position it and automatically resizes vector if necessary
  void insert( const iterator it, const Type *t )
  {
    if ( it >= end() || alc.num_elements_used == 0 )
    {
      push_back( t );
    }
    else
    {
      // Take the position before growing the storage, which invalidates the iterator.
      const size_t it_position = it - alc.ptr;

      if ( alc.num_elements_used == capacity() )
        vector_ReAllocate( vector_GetNextCapacity() );

      // move the remaining elements
      for ( size_t i = alc.num_elements_used; i > it_position; --i )
      {
        alc.ptr[i] = alc.ptr[i - 1];
      }

      alc.ptr[it_position] = const_cast< Type* >( t );
      ++alc.num_elements_used;
    }
  }

  void push_back( const Type *t )
  {
    if ( alc.num_elements_used == capacity() )
      vector_ReAllocate( vector_GetNextCapacity() );

    alc.ptr[alc.num_elements_used] = const_cast< Type* >( t );
    ++alc.num_elements_used;
  }
};

template<typename Type, size_t N = 0>
using MVKSmallVector = MVKSmallVectorImpl<Type, mvk_smallvector_allocator<Type, N>>;

#endif


