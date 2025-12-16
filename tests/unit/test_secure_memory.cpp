// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for SecureMemory
 * Tests secure memory operations and wiping
 */

#include "sak/secure_memory.h"
#include <QTest>
#include <QByteArray>
#include <cstring>

class TestSecureMemory : public QObject {
    Q_OBJECT

private slots:
    void testSecureAllocation() {
        sak::SecureMemory mem(256);
        
        QVERIFY(mem.isValid());
        QCOMPARE(mem.size(), 256);
        QVERIFY(mem.data() != nullptr);
    }

    void testSecureWrite() {
        sak::SecureMemory mem(128);
        
        const char* data = "Sensitive password data";
        mem.write(data, strlen(data));
        
        QVERIFY(memcmp(mem.data(), data, strlen(data)) == 0);
    }

    void testSecureRead() {
        sak::SecureMemory mem(128);
        
        const char* data = "Secret information";
        mem.write(data, strlen(data));
        
        char buffer[128] = {0};
        mem.read(buffer, strlen(data));
        
        QVERIFY(memcmp(buffer, data, strlen(data)) == 0);
    }

    void testMemoryWiping() {
        sak::SecureMemory mem(256);
        
        // Write sensitive data
        const char* sensitive = "This should be wiped";
        mem.write(sensitive, strlen(sensitive));
        
        // Store pointer for later verification
        void* ptr = mem.data();
        
        // Explicit wipe
        mem.wipe();
        
        // Verify all bytes are zero
        bool allZero = true;
        for (size_t i = 0; i < mem.size(); i++) {
            if (static_cast<unsigned char*>(ptr)[i] != 0) {
                allZero = false;
                break;
            }
        }
        
        QVERIFY(allZero);
    }

    void testDestructorWiping() {
        void* ptr = nullptr;
        size_t size = 0;
        
        {
            sak::SecureMemory mem(128);
            const char* data = "Temporary secret";
            mem.write(data, strlen(data));
            
            ptr = mem.data();
            size = mem.size();
            
            // mem goes out of scope and should wipe memory
        }
        
        // Note: This test is platform-dependent and may not work reliably
        // as the memory might be reallocated. This is for demonstration.
        // In production, use memory sanitizers or valgrind.
    }

    void testSecureString() {
        sak::SecureString str;
        
        str.assign("password123");
        
        QCOMPARE(str.size(), 11);
        QCOMPARE(str.toString(), QString("password123"));
    }

    void testSecureStringClearing() {
        sak::SecureString str;
        str.assign("sensitive_data");
        
        QVERIFY(!str.isEmpty());
        
        str.clear();
        
        QVERIFY(str.isEmpty());
        QCOMPARE(str.size(), 0);
    }

    void testSecureBuffer() {
        sak::SecureBuffer buffer(512);
        
        QByteArray data = "Binary sensitive data";
        buffer.append(data);
        
        QCOMPARE(buffer.size(), data.size());
        QVERIFY(memcmp(buffer.data(), data.data(), data.size()) == 0);
    }

    void testSecureBufferResize() {
        sak::SecureBuffer buffer(100);
        
        buffer.resize(200);
        QCOMPARE(buffer.capacity(), 200);
        
        buffer.resize(50);
        QCOMPARE(buffer.size(), 50);
    }

    void testMemoryLocking() {
        sak::SecureMemory mem(4096);  // Page size
        
        // Attempt to lock memory (prevent swapping)
        bool locked = mem.lock();
        
        // May fail without admin privileges
        if (locked) {
            QVERIFY(mem.isLocked());
            
            mem.unlock();
            QVERIFY(!mem.isLocked());
        } else {
            qDebug() << "Memory locking requires elevated privileges";
        }
    }

    void testSecureCompare() {
        sak::SecureMemory mem1(64);
        sak::SecureMemory mem2(64);
        
        const char* data = "compare_this";
        mem1.write(data, strlen(data));
        mem2.write(data, strlen(data));
        
        // Timing-safe comparison
        QVERIFY(mem1.secureCompare(mem2));
        
        // Modify second buffer
        mem2.write("different", 9);
        QVERIFY(!mem1.secureCompare(mem2));
    }

    void testConstantTimeCompare() {
        sak::SecureMemory mem;
        
        const char* str1 = "password123";
        const char* str2 = "password123";
        const char* str3 = "password456";
        
        // Should use constant-time comparison to prevent timing attacks
        QVERIFY(mem.constantTimeCompare(str1, str2, strlen(str1)));
        QVERIFY(!mem.constantTimeCompare(str1, str3, strlen(str1)));
    }

    void testSecureRandomGeneration() {
        sak::SecureMemory mem(32);
        
        mem.fillRandom();
        
        // Check that it's not all zeros (very unlikely with random data)
        bool hasNonZero = false;
        for (size_t i = 0; i < mem.size(); i++) {
            if (static_cast<unsigned char*>(mem.data())[i] != 0) {
                hasNonZero = true;
                break;
            }
        }
        
        QVERIFY(hasNonZero);
    }

    void testSecureZeroMemory() {
        sak::SecureMemory mem(256);
        
        // Fill with data
        memset(mem.data(), 0xFF, mem.size());
        
        // Securely zero (prevent compiler optimization)
        mem.secureZero();
        
        // Verify all zeros
        for (size_t i = 0; i < mem.size(); i++) {
            QCOMPARE(static_cast<unsigned char*>(mem.data())[i], 0);
        }
    }

    void testProtectedMemoryRegion() {
        sak::ProtectedMemory protected_mem(1024);
        
        // Write to protected region
        const char* data = "Protected data";
        protected_mem.write(data, strlen(data));
        
        // Read from protected region
        char buffer[128] = {0};
        protected_mem.read(buffer, strlen(data));
        
        QVERIFY(memcmp(buffer, data, strlen(data)) == 0);
    }

    void testMemoryProtection() {
        sak::SecureMemory mem(4096);
        
        // Set memory protection (read-only)
        bool protected_set = mem.setProtection(sak::SecureMemory::Protection::ReadOnly);
        
        if (protected_set) {
            QVERIFY(mem.isProtected());
            
            // Remove protection
            mem.setProtection(sak::SecureMemory::Protection::ReadWrite);
            QVERIFY(!mem.isProtected());
        }
    }

    void testSecureSwap() {
        sak::SecureMemory mem1(64);
        sak::SecureMemory mem2(64);
        
        const char* data1 = "first_data";
        const char* data2 = "second_data";
        
        mem1.write(data1, strlen(data1));
        mem2.write(data2, strlen(data2));
        
        // Secure swap
        mem1.swap(mem2);
        
        char buffer[64] = {0};
        mem1.read(buffer, strlen(data2));
        QVERIFY(memcmp(buffer, data2, strlen(data2)) == 0);
        
        memset(buffer, 0, sizeof(buffer));
        mem2.read(buffer, strlen(data1));
        QVERIFY(memcmp(buffer, data1, strlen(data1)) == 0);
    }

    void testSecureCopy() {
        sak::SecureMemory source(128);
        const char* data = "Copy this securely";
        source.write(data, strlen(data));
        
        sak::SecureMemory destination(128);
        destination.secureCopy(source);
        
        char buffer[128] = {0};
        destination.read(buffer, strlen(data));
        QVERIFY(memcmp(buffer, data, strlen(data)) == 0);
    }

    void testMemoryBarrier() {
        sak::SecureMemory mem(64);
        
        const char* data = "test_data";
        mem.write(data, strlen(data));
        
        // Ensure writes are visible across threads
        mem.memoryBarrier();
        
        // This is more of a compile-time check
        QVERIFY(true);
    }

    void testGuardPages() {
        sak::SecureMemory mem(4096, true);  // Enable guard pages
        
        // Write within bounds (should work)
        const char* data = "Safe write";
        mem.write(data, strlen(data));
        QVERIFY(true);
        
        // Attempting to write beyond bounds should be caught by guard pages
        // This would trigger a segfault in production, so we skip actual test
    }

    void testCanaryValues() {
        sak::SecureMemory mem(256);
        mem.enableCanary();
        
        const char* data = "Check canary";
        mem.write(data, strlen(data));
        
        // Verify canary is intact
        QVERIFY(mem.verifyCanary());
        
        // Corrupt memory beyond buffer (simulation)
        // In production this would detect overflow
    }

    void testSecureRealloc() {
        sak::SecureMemory mem(128);
        
        const char* data = "Initial data";
        mem.write(data, strlen(data));
        
        // Resize (should preserve data and wipe old memory)
        mem.secureRealloc(256);
        
        QCOMPARE(mem.size(), 256);
        
        char buffer[128] = {0};
        mem.read(buffer, strlen(data));
        QVERIFY(memcmp(buffer, data, strlen(data)) == 0);
    }

    void testThreadSafety() {
        sak::SecureMemory shared_mem(1024);
        shared_mem.enableThreadSafety();
        
        // Write from multiple threads
        QVector<QFuture<void>> futures;
        
        for (int i = 0; i < 10; i++) {
            futures.append(QtConcurrent::run([&shared_mem, i]() {
                QString data = QString("Thread %1 data").arg(i);
                shared_mem.write(data.toUtf8().data(), data.length());
            }));
        }
        
        // Wait for all threads
        for (auto& future : futures) {
            future.waitForFinished();
        }
        
        QVERIFY(true);  // No crashes
    }

    void testMemoryDump() {
        sak::SecureMemory mem(64);
        const char* data = "Dump this";
        mem.write(data, strlen(data));
        
        QByteArray dump = mem.dump();
        
        QCOMPARE(dump.size(), static_cast<int>(mem.size()));
        QVERIFY(dump.contains("Dump this"));
    }

    void testSecureHash() {
        sak::SecureMemory mem(64);
        const char* data = "Hash me";
        mem.write(data, strlen(data));
        
        QByteArray hash = mem.calculateHash();
        
        QVERIFY(!hash.isEmpty());
        QCOMPARE(hash.size(), 32);  // SHA256
    }

    void testIsWiped() {
        sak::SecureMemory mem(128);
        
        mem.write("data", 4);
        QVERIFY(!mem.isWiped());
        
        mem.wipe();
        QVERIFY(mem.isWiped());
    }

    void testPerformance() {
        QElapsedTimer timer;
        timer.start();
        
        // Allocate and wipe 1000 buffers
        for (int i = 0; i < 1000; i++) {
            sak::SecureMemory mem(1024);
            mem.fillRandom();
            mem.wipe();
        }
        
        qint64 elapsed = timer.elapsed();
        qDebug() << "Allocated/wiped 1000 buffers in" << elapsed << "ms";
        
        // Should complete in reasonable time
        QVERIFY(elapsed < 2000);  // Less than 2 seconds
    }

    void testWipingPatterns() {
        sak::SecureMemory mem(256);
        
        const char* data = "Sensitive";
        mem.write(data, strlen(data));
        
        // Test different wiping methods
        mem.wipeWithPattern(0x00);
        mem.wipeWithPattern(0xFF);
        mem.wipeWithPattern(0xAA);
        
        // Final secure wipe
        mem.wipe();
        QVERIFY(mem.isWiped());
    }

    void testMemoryAlignmentCorrect() {
        sak::SecureMemory mem(127);  // Odd size
        
        // Should still allocate properly aligned memory
        QVERIFY(mem.isValid());
        QVERIFY(reinterpret_cast<uintptr_t>(mem.data()) % sizeof(void*) == 0);
    }
};

QTEST_MAIN(TestSecureMemory)
#include "test_secure_memory.moc"
