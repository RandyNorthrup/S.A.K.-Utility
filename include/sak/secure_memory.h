/// @file secure_memory.h
/// @brief Secure memory handling utilities for sensitive data
/// @details Provides RAII-based secure memory management following security best practices
/// @note Part of security infrastructure (Phase 5)

#ifndef SAK_SECURE_MEMORY_H
#define SAK_SECURE_MEMORY_H

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <span>
#include <algorithm>

namespace sak {

/// @brief Secure memory wiper - ensures memory is zeroed before deallocation
/// @details Uses volatile pointer to prevent compiler optimization of zeroing
class secure_wiper {
public:
    /// @brief Securely wipe memory region
    /// @param ptr Pointer to memory to wipe
    /// @param size Size of memory region in bytes
    static void wipe(void* ptr, std::size_t size) noexcept {
        if (ptr == nullptr || size == 0) {
            return;
        }
        
        // Use volatile to prevent compiler from optimizing away the zeroing
        volatile unsigned char* vptr = static_cast<volatile unsigned char*>(ptr);
        
        // Zero the memory
        for (std::size_t i = 0; i < size; ++i) {
            vptr[i] = 0;
        }
        
        // Memory barrier to ensure writes complete
#ifdef _MSC_VER
        _ReadWriteBarrier();
#else
        __asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
    }
    
    /// @brief Securely wipe std::span
    /// @tparam T Element type
    /// @param data Span to wipe
    template<typename T>
    static void wipe(std::span<T> data) noexcept {
        wipe(data.data(), data.size_bytes());
    }
};

/// @brief Custom allocator that zeros memory on deallocation
/// @tparam T Type to allocate
template<typename T>
class secure_allocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    
    constexpr secure_allocator() noexcept = default;
    constexpr secure_allocator(const secure_allocator&) noexcept = default;
    
    template<typename U>
    constexpr secure_allocator(const secure_allocator<U>&) noexcept {}
    
    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        
        void* ptr = ::operator new(n * sizeof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        
        return static_cast<T*>(ptr);
    }
    
    void deallocate(T* ptr, std::size_t n) noexcept {
        if (ptr != nullptr && n > 0) {
            // Securely wipe memory before deallocation
            secure_wiper::wipe(ptr, n * sizeof(T));
        }
        ::operator delete(ptr);
    }
    
    template<typename U>
    friend constexpr bool operator==(
        const secure_allocator<T>&,
        const secure_allocator<U>&) noexcept {
        return true;
    }
    
    template<typename U>
    friend constexpr bool operator!=(
        const secure_allocator<T>&,
        const secure_allocator<U>&) noexcept {
        return false;
    }
};

/// @brief Secure string type that zeros memory on destruction
using secure_string = std::basic_string<char, std::char_traits<char>, secure_allocator<char>>;

/// @brief RAII wrapper for secure memory regions
/// @tparam T Element type
template<typename T>
class secure_buffer {
public:
    /// @brief Construct secure buffer with specified size
    /// @param size Number of elements
    explicit secure_buffer(std::size_t size)
        : m_data(std::make_unique<T[]>(size))
        , m_size(size) {
        
        // Zero initialize
        std::memset(m_data.get(), 0, size * sizeof(T));
    }
    
    // No copy
    secure_buffer(const secure_buffer&) = delete;
    secure_buffer& operator=(const secure_buffer&) = delete;
    
    // Move allowed
    secure_buffer(secure_buffer&& other) noexcept
        : m_data(std::move(other.m_data))
        , m_size(other.m_size) {
        other.m_size = 0;
    }
    
    secure_buffer& operator=(secure_buffer&& other) noexcept {
        if (this != &other) {
            clear();
            m_data = std::move(other.m_data);
            m_size = other.m_size;
            other.m_size = 0;
        }
        return *this;
    }
    
    /// @brief Destructor - securely wipes memory
    ~secure_buffer() {
        clear();
    }
    
    /// @brief Get pointer to buffer data
    [[nodiscard]] T* data() noexcept {
        return m_data.get();
    }
    
    /// @brief Get const pointer to buffer data
    [[nodiscard]] const T* data() const noexcept {
        return m_data.get();
    }
    
    /// @brief Get buffer size
    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    
    /// @brief Get span view of buffer
    [[nodiscard]] std::span<T> span() noexcept {
        return std::span<T>(m_data.get(), m_size);
    }
    
    /// @brief Get const span view of buffer
    [[nodiscard]] std::span<const T> span() const noexcept {
        return std::span<const T>(m_data.get(), m_size);
    }
    
    /// @brief Array subscript operator
    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        return m_data[index];
    }
    
    /// @brief Const array subscript operator
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        return m_data[index];
    }
    
    /// @brief Securely clear buffer contents
    void clear() noexcept {
        if (m_data && m_size > 0) {
            secure_wiper::wipe(m_data.get(), m_size * sizeof(T));
        }
    }
    
    /// @brief Check if buffer is empty
    [[nodiscard]] bool empty() const noexcept {
        return m_size == 0;
    }

private:
    std::unique_ptr<T[]> m_data;
    std::size_t m_size{0};
};

/// @brief RAII guard for secure memory - wipes on scope exit
/// @tparam T Element type
template<typename T>
class secure_memory_guard {
public:
    /// @brief Construct guard for memory region
    /// @param ptr Pointer to memory to guard
    /// @param size Size of memory region
    secure_memory_guard(T* ptr, std::size_t size) noexcept
        : m_ptr(ptr)
        , m_size(size) {}
    
    /// @brief Construct guard for span
    /// @param data Span to guard
    explicit secure_memory_guard(std::span<T> data) noexcept
        : m_ptr(data.data())
        , m_size(data.size()) {}
    
    // No copy or move
    secure_memory_guard(const secure_memory_guard&) = delete;
    secure_memory_guard& operator=(const secure_memory_guard&) = delete;
    secure_memory_guard(secure_memory_guard&&) = delete;
    secure_memory_guard& operator=(secure_memory_guard&&) = delete;
    
    /// @brief Destructor - securely wipes memory
    ~secure_memory_guard() {
        if (m_ptr && m_size > 0) {
            secure_wiper::wipe(m_ptr, m_size * sizeof(T));
        }
    }

private:
    T* m_ptr;
    std::size_t m_size;
};

/// @brief Helper to create secure_memory_guard for automatic memory wiping
/// @tparam T Element type
/// @param ptr Pointer to memory
/// @param size Size of memory region
/// @return RAII guard that will wipe memory on destruction
template<typename T>
[[nodiscard]] auto make_secure_guard(T* ptr, std::size_t size) {
    return secure_memory_guard<T>(ptr, size);
}

/// @brief Helper to create secure_memory_guard for span
/// @tparam T Element type
/// @param data Span to guard
/// @return RAII guard that will wipe memory on destruction
template<typename T>
[[nodiscard]] auto make_secure_guard(std::span<T> data) {
    return secure_memory_guard<T>(data);
}

/// @brief Secure comparison - constant time to prevent timing attacks
/// @param a First buffer
/// @param b Second buffer
/// @return True if buffers are equal
template<typename T>
[[nodiscard]] bool secure_compare(std::span<const T> a, std::span<const T> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    
    // Constant-time comparison
    volatile unsigned char result = 0;
    const auto* ptr_a = reinterpret_cast<const unsigned char*>(a.data());
    const auto* ptr_b = reinterpret_cast<const unsigned char*>(b.data());
    const std::size_t byte_count = a.size() * sizeof(T);
    
    for (std::size_t i = 0; i < byte_count; ++i) {
        result |= ptr_a[i] ^ ptr_b[i];
    }
    
    return result == 0;
}

/// @brief Secure string comparison - constant time
/// @param a First string
/// @param b Second string
/// @return True if strings are equal
[[nodiscard]] inline bool secure_compare(
    std::string_view a,
    std::string_view b) noexcept {
    
    if (a.size() != b.size()) {
        return false;
    }
    
    volatile unsigned char result = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    
    return result == 0;
}

/// @brief Generate cryptographically secure random bytes
/// @param buffer Buffer to fill with random bytes
/// @param size Number of bytes to generate
/// @return True if successful
[[nodiscard]] bool generate_secure_random(void* buffer, std::size_t size) noexcept;

/// @brief Generate cryptographically secure random bytes into span
/// @tparam T Element type
/// @param data Span to fill with random bytes
/// @return True if successful
template<typename T>
[[nodiscard]] bool generate_secure_random(std::span<T> data) noexcept {
    return generate_secure_random(data.data(), data.size_bytes());
}

/// @brief Lock memory to prevent swapping (platform-specific)
/// @param ptr Pointer to memory
/// @param size Size of memory region
/// @return True if successful (may fail on some platforms)
/// @note Requires elevated privileges on some platforms
[[nodiscard]] bool lock_memory(void* ptr, std::size_t size) noexcept;

/// @brief Unlock previously locked memory
/// @param ptr Pointer to memory
/// @param size Size of memory region
/// @return True if successful
[[nodiscard]] bool unlock_memory(void* ptr, std::size_t size) noexcept;

/// @brief RAII wrapper for locked memory
class locked_memory {
public:
    /// @brief Lock memory region
    /// @param ptr Pointer to memory
    /// @param size Size of memory region
    locked_memory(void* ptr, std::size_t size) noexcept
        : m_ptr(ptr)
        , m_size(size)
        , m_locked(lock_memory(ptr, size)) {}
    
    // No copy or move
    locked_memory(const locked_memory&) = delete;
    locked_memory& operator=(const locked_memory&) = delete;
    locked_memory(locked_memory&&) = delete;
    locked_memory& operator=(locked_memory&&) = delete;
    
    /// @brief Destructor - unlocks memory
    ~locked_memory() {
        if (m_locked && m_ptr) {
            // Explicitly ignore return value as we're in destructor
            (void)unlock_memory(m_ptr, m_size);
        }
    }
    
    /// @brief Check if memory was successfully locked
    [[nodiscard]] bool is_locked() const noexcept {
        return m_locked;
    }

private:
    void* m_ptr;
    std::size_t m_size;
    bool m_locked;
};

} // namespace sak

#endif // SAK_SECURE_MEMORY_H
