/*
 * MVKSmallVectorAllocator.h
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

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>


namespace mvk_smallvector_memory_allocator
{
  inline char *alloc( const size_t num_bytes )
  {
    return new char[num_bytes];
  }

  inline void free( void *ptr )
  {
    delete[] (char*)ptr;
  }
};


//////////////////////////////////////////////////////////////////////////////////////////
//
// mvk_smallvector_allocator -> malloc based MVKSmallVector allocator with preallocated storage
//
//////////////////////////////////////////////////////////////////////////////////////////
template <typename T, int M>
class mvk_smallvector_allocator final
{
public:
	typedef T value_type;
	T      *ptr;
	size_t  num_elements_used;

private:

	// Once dynamic allocation is in use, the preallocated content memory space will
	// be re-purposed to hold the capacity count. If the content type is small, ensure
	// preallocated memory is large enough to hold the capacity count, and increase
	// the number of preallocated elements accordintly, to make use of this memory.
	// In addition, increase the number of pre-allocated elements to fill any space created
	// by the alignment of this structure, to maximize the use of the preallocated memory.
	static constexpr size_t CAP_CNT_SIZE = sizeof( size_t );
	static constexpr size_t ALIGN_CNT = CAP_CNT_SIZE / sizeof( T );
	static constexpr size_t ALIGN_MASK = (ALIGN_CNT > 0) ? (ALIGN_CNT - 1) : 0;

	static constexpr size_t MIN_CNT = M > ALIGN_CNT ? M : ALIGN_CNT;
	static constexpr size_t N = (MIN_CNT + ALIGN_MASK) & ~ALIGN_MASK;

	static constexpr size_t MIN_STACK_SIZE = ( N * sizeof( T ) );
	static constexpr size_t STACK_SIZE = MIN_STACK_SIZE > CAP_CNT_SIZE ? MIN_STACK_SIZE : CAP_CNT_SIZE;
	alignas( alignof( T ) ) unsigned char elements_stack[ STACK_SIZE ];

  void set_num_elements_reserved( const size_t num_elements_reserved )
  {
    *reinterpret_cast<size_t*>( &elements_stack[0] ) = num_elements_reserved;
  }

public:
  const T &operator[]( const size_t i ) const { return ptr[i]; }
  T       &operator[]( const size_t i )       { return ptr[i]; }

  size_t size() const { return num_elements_used; }

  //
  // faster element construction and destruction using type traits
  //
  template<class S, class... Args>
  void construct( S *_ptr, Args&&... _args )
  {
    if constexpr( std::is_trivially_constructible_v<S, Args...> )
    {
      *_ptr = S( std::forward<Args>( _args )... );
    }
    else
    {
      new ( _ptr ) S( std::forward<Args>( _args )... );
    }
  }

  template<class S>
  void destruct( S *_ptr )
  {
    if constexpr( !std::is_trivially_destructible_v<S> )
    {
      _ptr->~S();
    }
  }

  template<class S>
  void destruct_all()
  {
    if constexpr( !std::is_trivially_destructible_v<S> )
    {
      for( size_t i = 0; i < num_elements_used; ++i )
      {
        ptr[i].~S();
      }
    }

    num_elements_used = 0;
  }

  //
  // Moves count elements from src to uninitialized storage at dst, leaving src uninitialized.
  // Trivially copyable elements are copied as a single block of bytes.
  //
  void relocate( T *dst, T *src, const size_t count )
  {
    if constexpr( std::is_trivially_copyable_v<T> )
    {
      if( count ) { memcpy( static_cast<void*>( dst ), static_cast<const void*>( src ), count * sizeof( T ) ); }
    }
    else
    {
      for( size_t i = 0; i < count; ++i )
      {
        construct( &dst[i], std::move( src[i] ) );
        destruct( &src[i] );
      }
    }
  }

  template<class S>
  void swap_stack( mvk_smallvector_allocator &a )
  {
    if constexpr( std::is_trivially_copyable_v<S> )
    {
      // Only the bytes that hold constructed elements in either allocator need to move.
      const size_t used_bytes = ( num_elements_used > a.num_elements_used ? num_elements_used : a.num_elements_used ) * sizeof( T );
      alignas( alignof( T ) ) unsigned char tmp_storage[ STACK_SIZE ];
      memcpy( tmp_storage, elements_stack, used_bytes );
      memcpy( elements_stack, a.elements_stack, used_bytes );
      memcpy( a.elements_stack, tmp_storage, used_bytes );
    }
    else
    {
      // Both allocators hold their elements in their inline stack storage. Move this
      // allocator's elements to raw temporary storage, move a's elements into this
      // allocator, then move the temporaries into a. Only constructed elements are
      // touched; the stack storage beyond num_elements_used is uninitialized memory.
      alignas( alignof( T ) ) unsigned char tmp_storage[ STACK_SIZE ];
      T *tmp = reinterpret_cast< T* >( &tmp_storage[0] );
      relocate( tmp, ptr, num_elements_used );
      relocate( ptr, a.ptr, a.num_elements_used );
      relocate( a.ptr, tmp, num_elements_used );
    }
  }

public:
  mvk_smallvector_allocator() : ptr(reinterpret_cast<T*>( &elements_stack[0] )), num_elements_used(0)
  {
  }

  mvk_smallvector_allocator( mvk_smallvector_allocator &&a ) noexcept
  {
    // is a heap based -> steal ptr from a
    if( !a.get_data_on_stack() )
    {
      ptr = a.ptr;
      set_num_elements_reserved( a.get_capacity() );

      a.ptr = a.get_default_ptr();
    }
    else
    {
      ptr = get_default_ptr();
      relocate( ptr, a.ptr, a.num_elements_used );
    }

	num_elements_used = a.num_elements_used;
    a.num_elements_used = 0;
  }

  ~mvk_smallvector_allocator()
  {
    deallocate();
  }

  size_t get_capacity() const
  {
    return get_data_on_stack() ? N : *reinterpret_cast<const size_t*>( &elements_stack[0] );
  }

  constexpr T *get_default_ptr() const
  {
    return reinterpret_cast< T* >( const_cast< unsigned char * >( &elements_stack[0] ) );
  }

  bool get_data_on_stack() const
  {
    return ptr == get_default_ptr();
  }

  void swap( mvk_smallvector_allocator &a )
  {
    // both allocators on heap -> easy case
    if( !get_data_on_stack() && !a.get_data_on_stack() )
    {
      auto copy_ptr = ptr;
      auto copy_num_elements_reserved = get_capacity();
      ptr = a.ptr;
      set_num_elements_reserved( a.get_capacity() );
      a.ptr = copy_ptr;
      a.set_num_elements_reserved( copy_num_elements_reserved );
    }
    // both allocators on stack -> just switch the stack contents
    else if( get_data_on_stack() && a.get_data_on_stack() )
    {
      swap_stack<T>( a );
    }
    else if( get_data_on_stack() && !a.get_data_on_stack() )
    {
      auto copy_ptr = a.ptr;
      auto copy_num_elements_reserved = a.get_capacity();

      a.ptr = a.get_default_ptr();
      relocate( a.ptr, ptr, num_elements_used );

      ptr = copy_ptr;
      set_num_elements_reserved( copy_num_elements_reserved );
    }
    else if( !get_data_on_stack() && a.get_data_on_stack() )
    {
      auto copy_ptr = ptr;
      auto copy_num_elements_reserved = get_capacity();

      ptr = get_default_ptr();
      relocate( ptr, a.ptr, a.num_elements_used );

      a.ptr = copy_ptr;
      a.set_num_elements_reserved( copy_num_elements_reserved );
    }

    auto copy_num_elements_used = num_elements_used;
    num_elements_used = a.num_elements_used;
    a.num_elements_used = copy_num_elements_used;
  }

  //
  // allocates rounded up to the defined alignment the number of bytes / if the system cannot allocate the specified amount of memory then a null block is returned
  //
  void allocate( const size_t num_elements_to_reserve )
  {
    deallocate();

    // check if enough memory on stack space is left
    if( num_elements_to_reserve <= N )
    {
      return;
    }

    ptr = reinterpret_cast< T* >( mvk_smallvector_memory_allocator::alloc( num_elements_to_reserve * sizeof( T ) ) );
    num_elements_used = 0;
    set_num_elements_reserved( num_elements_to_reserve );
  }

  void _re_allocate( const size_t num_elements_to_reserve )
  {
    auto *new_ptr = reinterpret_cast< T* >( mvk_smallvector_memory_allocator::alloc( num_elements_to_reserve * sizeof( T ) ) );

    relocate( new_ptr, ptr, num_elements_used );

    if( ptr != get_default_ptr() )
    {
      mvk_smallvector_memory_allocator::free( ptr );
    }

    ptr = new_ptr;
    set_num_elements_reserved( num_elements_to_reserve );
  }

  void re_allocate( const size_t num_elements_to_reserve )
  {
    //TM_ASSERT( num_elements_to_reserve > get_capacity() );

    if( num_elements_to_reserve > N )
    {
      _re_allocate( num_elements_to_reserve );
    }
  }

  void shrink_to_fit()
  {
    // nothing to do if data is on stack already
    if( get_data_on_stack() )
      return;

    // move elements to stack space
    if( num_elements_used <= N )
    {
      //const auto num_elements_reserved = get_capacity();

      auto *stack_ptr = get_default_ptr();
      relocate( stack_ptr, ptr, num_elements_used );

      mvk_smallvector_memory_allocator::free( ptr );

      ptr = stack_ptr;
    }
    else
    {
      auto *new_ptr = reinterpret_cast< T* >( mvk_smallvector_memory_allocator::alloc( num_elements_used * sizeof( T ) ) );

      relocate( new_ptr, ptr, num_elements_used );

      mvk_smallvector_memory_allocator::free( ptr );

      ptr = new_ptr;
      set_num_elements_reserved( num_elements_used );
    }
  }

  void deallocate()
  {
    destruct_all<T>();

    if( !get_data_on_stack() )
    {
      mvk_smallvector_memory_allocator::free( ptr );
    }

    ptr = get_default_ptr();
    num_elements_used = 0;
  }
};

