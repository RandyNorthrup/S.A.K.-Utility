// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for InputValidator
 * Tests input sanitization and security validation
 */

#include "sak/input_validator.h"
#include <QTest>
#include <QTemporaryDir>

class TestInputValidator : public QObject {
    Q_OBJECT

private slots:
    void testPathTraversalPrevention() {
        sak::InputValidator validator;
        
        // Malicious paths
        QVERIFY(!validator.isValidPath("../../../etc/passwd"));
        QVERIFY(!validator.isValidPath("..\\..\\..\\Windows\\System32"));
        QVERIFY(!validator.isValidPath("C:\\Users\\..\\..\\Windows"));
        QVERIFY(!validator.isValidPath("/etc/../../../root"));
        
        // Valid paths
        QVERIFY(validator.isValidPath("C:\\Users\\Public\\Documents"));
        QVERIFY(validator.isValidPath("D:\\Data\\Files"));
        QVERIFY(validator.isValidPath("/home/user/documents"));
    }

    void testSanitizePath() {
        sak::InputValidator validator;
        
        // Remove path traversal
        QString path1 = validator.sanitizePath("C:\\Users\\..\\Public");
        QVERIFY(!path1.contains(".."));
        
        // Remove multiple slashes
        QString path2 = validator.sanitizePath("C:\\\\Users\\\\\\Public");
        QVERIFY(!path2.contains("\\\\"));
        
        // Remove null bytes
        QString path3 = validator.sanitizePath(QString("C:\\Users\0Public"));
        QVERIFY(!path3.contains('\0'));
    }

    void testCommandInjectionPrevention() {
        sak::InputValidator validator;
        
        // Malicious commands
        QVERIFY(!validator.isValidCommandArg("test; rm -rf /"));
        QVERIFY(!validator.isValidCommandArg("test && del C:\\*.*"));
        QVERIFY(!validator.isValidCommandArg("test | cat /etc/passwd"));
        QVERIFY(!validator.isValidCommandArg("test `whoami`"));
        QVERIFY(!validator.isValidCommandArg("test $(id)"));
        QVERIFY(!validator.isValidCommandArg("test & shutdown -s"));
        
        // Valid arguments
        QVERIFY(validator.isValidCommandArg("myfile.txt"));
        QVERIFY(validator.isValidCommandArg("--option=value"));
        QVERIFY(validator.isValidCommandArg("C:\\Program Files\\App"));
    }

    void testSanitizeCommandArg() {
        sak::InputValidator validator;
        
        QString arg1 = validator.sanitizeCommandArg("test; rm -rf");
        QVERIFY(!arg1.contains(";"));
        
        QString arg2 = validator.sanitizeCommandArg("test && malicious");
        QVERIFY(!arg2.contains("&&"));
        
        QString arg3 = validator.sanitizeCommandArg("test | pipe");
        QVERIFY(!arg3.contains("|"));
    }

    void testFilenameValidation() {
        sak::InputValidator validator;
        
        // Invalid filenames (Windows reserved)
        QVERIFY(!validator.isValidFilename("CON"));
        QVERIFY(!validator.isValidFilename("PRN"));
        QVERIFY(!validator.isValidFilename("AUX"));
        QVERIFY(!validator.isValidFilename("NUL"));
        QVERIFY(!validator.isValidFilename("COM1"));
        QVERIFY(!validator.isValidFilename("LPT1"));
        
        // Invalid characters
        QVERIFY(!validator.isValidFilename("file<name>.txt"));
        QVERIFY(!validator.isValidFilename("file>name.txt"));
        QVERIFY(!validator.isValidFilename("file:name.txt"));
        QVERIFY(!validator.isValidFilename("file\"name.txt"));
        QVERIFY(!validator.isValidFilename("file|name.txt"));
        QVERIFY(!validator.isValidFilename("file?name.txt"));
        QVERIFY(!validator.isValidFilename("file*name.txt"));
        
        // Valid filenames
        QVERIFY(validator.isValidFilename("document.txt"));
        QVERIFY(validator.isValidFilename("file_name-2024.pdf"));
        QVERIFY(validator.isValidFilename("archive (1).zip"));
    }

    void testSanitizeFilename() {
        sak::InputValidator validator;
        
        QString name1 = validator.sanitizeFilename("file<>name.txt");
        QVERIFY(!name1.contains('<'));
        QVERIFY(!name1.contains('>'));
        
        QString name2 = validator.sanitizeFilename("CON.txt");
        QVERIFY(name2 != "CON.txt");  // Should be modified
        
        QString name3 = validator.sanitizeFilename("file|name?.txt");
        QVERIFY(!name3.contains('|'));
        QVERIFY(!name3.contains('?'));
    }

    void testUrlValidation() {
        sak::InputValidator validator;
        
        // Valid URLs
        QVERIFY(validator.isValidUrl("https://example.com"));
        QVERIFY(validator.isValidUrl("http://example.com/path"));
        QVERIFY(validator.isValidUrl("https://sub.example.com:8080/path?query=1"));
        
        // Invalid URLs
        QVERIFY(!validator.isValidUrl("javascript:alert(1)"));
        QVERIFY(!validator.isValidUrl("file:///etc/passwd"));
        QVERIFY(!validator.isValidUrl("data:text/html,<script>alert(1)</script>"));
        QVERIFY(!validator.isValidUrl("not a url"));
        QVERIFY(!validator.isValidUrl("ftp://example.com"));  // If FTP not allowed
    }

    void testEmailValidation() {
        sak::InputValidator validator;
        
        // Valid emails
        QVERIFY(validator.isValidEmail("user@example.com"));
        QVERIFY(validator.isValidEmail("first.last@example.co.uk"));
        QVERIFY(validator.isValidEmail("user+tag@example.com"));
        
        // Invalid emails
        QVERIFY(!validator.isValidEmail("notanemail"));
        QVERIFY(!validator.isValidEmail("@example.com"));
        QVERIFY(!validator.isValidEmail("user@"));
        QVERIFY(!validator.isValidEmail("user @example.com"));
        QVERIFY(!validator.isValidEmail("user@example"));
    }

    void testSqlInjectionPrevention() {
        sak::InputValidator validator;
        
        // SQL injection attempts
        QVERIFY(!validator.isValidInput("' OR '1'='1"));
        QVERIFY(!validator.isValidInput("admin'--"));
        QVERIFY(!validator.isValidInput("1; DROP TABLE users--"));
        QVERIFY(!validator.isValidInput("' UNION SELECT * FROM passwords--"));
        QVERIFY(!validator.isValidInput("1' AND '1'='1"));
        
        // Valid inputs
        QVERIFY(validator.isValidInput("John Smith"));
        QVERIFY(validator.isValidInput("user@example.com"));
        QVERIFY(validator.isValidInput("My file name.txt"));
    }

    void testXssPatternDetection() {
        sak::InputValidator validator;
        
        // XSS attempts
        QVERIFY(!validator.isValidInput("<script>alert('xss')</script>"));
        QVERIFY(!validator.isValidInput("<img src=x onerror=alert(1)>"));
        QVERIFY(!validator.isValidInput("javascript:alert(1)"));
        QVERIFY(!validator.isValidInput("<iframe src='malicious.com'>"));
        QVERIFY(!validator.isValidInput("<body onload=alert(1)>"));
        
        // Valid HTML-like content (if allowed)
        QVERIFY(validator.isValidInput("Normal <text> with brackets"));
    }

    void testLdapInjectionPrevention() {
        sak::InputValidator validator;
        
        // LDAP injection attempts
        QVERIFY(!validator.isValidLdapInput("*)(uid=*))(|(uid=*"));
        QVERIFY(!validator.isValidLdapInput("admin)(|(password=*))"));
        QVERIFY(!validator.isValidLdapInput("*"));
        
        // Valid LDAP input
        QVERIFY(validator.isValidLdapInput("username"));
        QVERIFY(validator.isValidLdapInput("john.smith"));
    }

    void testBufferOverflowPrevention() {
        sak::InputValidator validator;
        
        // Set max length
        validator.setMaxInputLength(100);
        
        // Valid length
        QString short_input(50, 'a');
        QVERIFY(validator.isValidLength(short_input));
        
        // Excessive length
        QString long_input(500, 'a');
        QVERIFY(!validator.isValidLength(long_input));
        
        // Truncate
        QString truncated = validator.truncateToMaxLength(long_input);
        QCOMPARE(truncated.length(), 100);
    }

    void testUnicodeHandling() {
        sak::InputValidator validator;
        
        // Valid Unicode
        QVERIFY(validator.isValidUnicode("Hello 世界"));
        QVERIFY(validator.isValidUnicode("Привет мир"));
        QVERIFY(validator.isValidUnicode("🎉 Emoji"));
        
        // Invalid Unicode sequences
        QString invalid = QString::fromUtf8("\xC0\x80");  // Overlong encoding
        QVERIFY(!validator.isValidUnicode(invalid));
    }

    void testNullByteInjection() {
        sak::InputValidator validator;
        
        // Null byte injection attempts
        QString null_injection = QString("file.txt\0.exe");
        QVERIFY(!validator.containsNullBytes(null_injection));
        
        QString clean = QString("file.txt");
        QVERIFY(validator.containsNullBytes(clean) == false);
    }

    void testIntegerValidation() {
        sak::InputValidator validator;
        
        // Valid integers
        QVERIFY(validator.isValidInteger("123"));
        QVERIFY(validator.isValidInteger("-456"));
        QVERIFY(validator.isValidInteger("0"));
        
        // Invalid integers
        QVERIFY(!validator.isValidInteger("abc"));
        QVERIFY(!validator.isValidInteger("12.34"));
        QVERIFY(!validator.isValidInteger("1e10"));
        QVERIFY(!validator.isValidInteger("0x123"));
    }

    void testIntegerRange() {
        sak::InputValidator validator;
        
        QVERIFY(validator.isInRange(50, 0, 100));
        QVERIFY(validator.isInRange(0, 0, 100));
        QVERIFY(validator.isInRange(100, 0, 100));
        
        QVERIFY(!validator.isInRange(-1, 0, 100));
        QVERIFY(!validator.isInRange(101, 0, 100));
    }

    void testPortNumberValidation() {
        sak::InputValidator validator;
        
        // Valid ports
        QVERIFY(validator.isValidPort(80));
        QVERIFY(validator.isValidPort(443));
        QVERIFY(validator.isValidPort(8080));
        QVERIFY(validator.isValidPort(65535));
        
        // Invalid ports
        QVERIFY(!validator.isValidPort(0));
        QVERIFY(!validator.isValidPort(-1));
        QVERIFY(!validator.isValidPort(65536));
        QVERIFY(!validator.isValidPort(99999));
    }

    void testIpAddressValidation() {
        sak::InputValidator validator;
        
        // Valid IPv4
        QVERIFY(validator.isValidIpAddress("192.168.1.1"));
        QVERIFY(validator.isValidIpAddress("10.0.0.1"));
        QVERIFY(validator.isValidIpAddress("255.255.255.255"));
        QVERIFY(validator.isValidIpAddress("0.0.0.0"));
        
        // Invalid IPv4
        QVERIFY(!validator.isValidIpAddress("256.1.1.1"));
        QVERIFY(!validator.isValidIpAddress("192.168.1"));
        QVERIFY(!validator.isValidIpAddress("192.168.1.1.1"));
        QVERIFY(!validator.isValidIpAddress("abc.def.ghi.jkl"));
        
        // Valid IPv6
        QVERIFY(validator.isValidIpAddress("2001:0db8:85a3:0000:0000:8a2e:0370:7334"));
        QVERIFY(validator.isValidIpAddress("::1"));
        QVERIFY(validator.isValidIpAddress("fe80::1"));
    }

    void testWhitelistValidation() {
        sak::InputValidator validator;
        
        QStringList whitelist = {"allowed1", "allowed2", "allowed3"};
        validator.setWhitelist(whitelist);
        
        QVERIFY(validator.isWhitelisted("allowed1"));
        QVERIFY(validator.isWhitelisted("allowed2"));
        QVERIFY(!validator.isWhitelisted("not_allowed"));
        QVERIFY(!validator.isWhitelisted("ALLOWED1"));  // Case sensitive
    }

    void testBlacklistValidation() {
        sak::InputValidator validator;
        
        QStringList blacklist = {"forbidden1", "forbidden2", "forbidden3"};
        validator.setBlacklist(blacklist);
        
        QVERIFY(!validator.isNotBlacklisted("forbidden1"));
        QVERIFY(!validator.isNotBlacklisted("forbidden2"));
        QVERIFY(validator.isNotBlacklisted("allowed"));
    }

    void testRegexPatternMatching() {
        sak::InputValidator validator;
        
        // Test custom pattern
        QString pattern = "^[A-Za-z0-9_-]+$";
        validator.setCustomPattern(pattern);
        
        QVERIFY(validator.matchesPattern("valid_name-123"));
        QVERIFY(!validator.matchesPattern("invalid name!"));
        QVERIFY(!validator.matchesPattern("test@example"));
    }

    void testMultipleValidationRules() {
        sak::InputValidator validator;
        
        // Configure validator
        validator.setMaxInputLength(50);
        validator.setAllowedCharacters("A-Za-z0-9_-");
        
        // Test combined rules
        QString input1 = "valid_name-123";
        QVERIFY(validator.validate(input1));
        
        QString input2 = "invalid name!";
        QVERIFY(!validator.validate(input2));
        
        QString input3 = QString(100, 'a');
        QVERIFY(!validator.validate(input3));  // Too long
    }

    void testSanitizeHtml() {
        sak::InputValidator validator;
        
        QString html1 = validator.sanitizeHtml("<script>alert('xss')</script>");
        QVERIFY(!html1.contains("<script>"));
        
        QString html2 = validator.sanitizeHtml("<p>Safe text</p>");
        QVERIFY(html2.contains("Safe text"));
        
        QString html3 = validator.sanitizeHtml("<img src=x onerror=alert(1)>");
        QVERIFY(!html3.contains("onerror"));
    }

    void testEncodingValidation() {
        sak::InputValidator validator;
        
        // Test UTF-8 validation
        QVERIFY(validator.isValidUtf8("Hello World"));
        QVERIFY(validator.isValidUtf8("Hello 世界"));
        
        // Invalid UTF-8 sequences
        QByteArray invalid;
        invalid.append('\xFF');
        invalid.append('\xFE');
        QVERIFY(!validator.isValidUtf8(QString::fromUtf8(invalid)));
    }

    void testDirectoryTraversal() {
        sak::InputValidator validator;
        
        // Absolute path with traversal
        QVERIFY(!validator.containsTraversal("C:\\Windows\\..\\..\\sensitive"));
        QVERIFY(!validator.containsTraversal("/etc/../../../root"));
        
        // Relative traversal
        QVERIFY(!validator.containsTraversal("../../../etc/passwd"));
        QVERIFY(!validator.containsTraversal("..\\..\\..\\Windows\\System32"));
        
        // Clean paths
        QVERIFY(!validator.containsTraversal("C:\\Users\\Public"));
        QVERIFY(!validator.containsTraversal("/home/user/documents"));
    }

    void testPerformance() {
        sak::InputValidator validator;
        
        QElapsedTimer timer;
        timer.start();
        
        // Validate 10,000 inputs
        for (int i = 0; i < 10000; i++) {
            QString input = QString("test_input_%1").arg(i);
            validator.isValidInput(input);
        }
        
        qint64 elapsed = timer.elapsed();
        qDebug() << "Validated 10,000 inputs in" << elapsed << "ms";
        
        // Should be reasonably fast
        QVERIFY(elapsed < 1000);  // Less than 1 second
    }
};

QTEST_MAIN(TestInputValidator)
#include "test_input_validator.moc"
