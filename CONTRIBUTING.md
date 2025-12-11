# Contributing to S.A.K. Utility

Thank you for your interest in contributing to S.A.K. Utility! This document provides guidelines and instructions for contributing to the project.

## 📋 Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [How to Contribute](#how-to-contribute)
- [Coding Standards](#coding-standards)
- [Testing](#testing)
- [Pull Request Process](#pull-request-process)

## 🤝 Code of Conduct

- Be respectful and inclusive
- Focus on constructive feedback
- Help others learn and grow
- Maintain professional communication

## 🚀 Getting Started

### Prerequisites

- **Windows 10/11** (x64)
- **Visual Studio 2022** with C++ Desktop Development
- **CMake 3.28+**
- **Qt 6.5.3** (msvc2019_64)
- **Git**

### Setting Up Your Development Environment

1. **Fork the repository** on GitHub

2. **Clone your fork**:
   ```powershell
   git clone https://github.com/YOUR_USERNAME/S.A.K.-Utility.git
   cd S.A.K.-Utility
   ```

3. **Add upstream remote**:
   ```powershell
   git remote add upstream https://github.com/RandyNorthrup/S.A.K.-Utility.git
   ```

4. **Configure CMake**:
   ```powershell
   cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.5.3/msvc2019_64"
   ```

5. **Build the project**:
   ```powershell
   cmake --build build --config Release
   ```

## 💻 How to Contribute

### Reporting Bugs

1. Check if the bug is already reported in [Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
2. If not, create a new issue with:
   - Clear, descriptive title
   - Steps to reproduce
   - Expected behavior
   - Actual behavior
   - Screenshots (if applicable)
   - System information (Windows version, etc.)

### Suggesting Enhancements

1. Check existing [Issues](https://github.com/RandyNorthrup/S.A.K.-Utility/issues) and [Discussions](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
2. Create a new issue with:
   - Clear description of the enhancement
   - Use cases and benefits
   - Potential implementation approach

### Contributing Code

1. **Create a branch** for your feature/fix:
   ```powershell
   git checkout -b feature/your-feature-name
   ```

2. **Make your changes** following our [Coding Standards](#coding-standards)

3. **Test your changes** thoroughly

4. **Commit your changes**:
   ```powershell
   git add .
   git commit -m "feat: Add awesome feature"
   ```
   
   Use conventional commit messages:
   - `feat:` - New feature
   - `fix:` - Bug fix
   - `docs:` - Documentation changes
   - `style:` - Code style changes (formatting, etc.)
   - `refactor:` - Code refactoring
   - `test:` - Adding or updating tests
   - `chore:` - Maintenance tasks

5. **Push to your fork**:
   ```powershell
   git push origin feature/your-feature-name
   ```

6. **Create a Pull Request** on GitHub

## 📝 Coding Standards

### C++ Style Guide

#### General Principles
- **C++23 Standard**: Use modern C++ features
- **Qt Conventions**: Follow Qt naming and patterns
- **RAII**: Always manage resources properly
- **Const Correctness**: Use `const` wherever applicable
- **Type Safety**: Avoid C-style casts

#### Naming Conventions

```cpp
// Classes: PascalCase
class AppMigrationPanel : public QWidget { };

// Functions/Methods: camelCase
void updateTableView();

// Private Members: m_ prefix + camelCase
QTableView* m_tableView;
QString m_currentPath;

// Constants: k prefix + PascalCase
const int kMaxThreads = 8;
const QString kDefaultPath = "C:/";

// Enums: PascalCase
enum class InstallStatus {
    Pending,
    InProgress,
    Completed,
    Failed
};
```

#### File Organization

```cpp
// Header file (example.h)
#pragma once

#include <QWidget>          // Qt headers
#include <vector>           // STL headers
#include "sak/types.h"      // Project headers

namespace sak {

class Example : public QWidget {
    Q_OBJECT
    
public:
    explicit Example(QWidget* parent = nullptr);
    ~Example() override;
    
Q_SIGNALS:
    void dataChanged();
    
private Q_SLOTS:
    void onButtonClicked();
    
private:
    void setupUi();
    void setupConnections();
    
    QWidget* m_widget;
};

} // namespace sak
```

#### Code Formatting
- **Indentation**: 4 spaces (no tabs)
- **Line Length**: 100 characters maximum
- **Braces**: Opening brace on same line
- **Spacing**: Space after keywords, around operators

```cpp
// Good
if (condition) {
    doSomething();
} else {
    doSomethingElse();
}

// Bad
if(condition)
{
    doSomething();
}
else{doSomethingElse();}
```

#### Qt-Specific Guidelines

```cpp
// Use Q_EMIT instead of emit (QT_NO_KEYWORDS)
Q_EMIT dataChanged();

// Use Q_SIGNALS and Q_SLOTS
Q_SIGNALS:
    void finished();

private Q_SLOTS:
    void onTimeout();

// Prefer Qt containers for Qt code
QList<QString> items;
QVector<int> values;

// Use Qt's parent-child memory management
auto* widget = new QWidget(this);  // 'this' manages lifetime
```

### CMake Standards

```cmake
# Use modern CMake (3.28+)
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

# Group related files
set(CORE_SOURCES
    src/core/file1.cpp
    src/core/file2.cpp
)

# Use target-based approach
target_include_directories(target_name
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
```

## 🧪 Testing

### Running Tests

```powershell
# Build all tests
cmake --build build --config Release

# Run specific test
.\build\Release\test_app_scanner.exe
.\build\Release\test_chocolatey_manager.exe
```

### Writing Tests

- Create test files in `tests/` directory
- Use descriptive test names
- Test both success and failure cases
- Include edge cases
- Verify cleanup (no leaks, proper RAII)

```cpp
// Example test structure
void testAppScanner() {
    AppScanner scanner;
    auto apps = scanner.scanInstalledApplications();
    
    assert(!apps.empty() && "Should find installed apps");
    assert(apps[0].name.length() > 0 && "App name should not be empty");
}
```

## 🔄 Pull Request Process

### Before Submitting

- [ ] Code follows style guidelines
- [ ] All tests pass
- [ ] New tests added for new features
- [ ] Documentation updated (if needed)
- [ ] Commit messages follow conventions
- [ ] No merge conflicts with main branch

### PR Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change
- [ ] Documentation update

## Testing
- [ ] Tested on Windows 10/11
- [ ] All unit tests pass
- [ ] Manual testing completed

## Screenshots (if applicable)
Add screenshots showing UI changes

## Checklist
- [ ] Code follows style guidelines
- [ ] Self-review completed
- [ ] Comments added for complex code
- [ ] Documentation updated
```

### Review Process

1. Maintainers will review your PR
2. Address any requested changes
3. Once approved, PR will be merged
4. Your contribution will be included in the next release!

## 📚 Additional Resources

- [Qt Documentation](https://doc.qt.io/qt-6/)
- [CMake Documentation](https://cmake.org/documentation/)
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [Conventional Commits](https://www.conventionalcommits.org/)

## 🎉 Recognition

Contributors will be recognized in:
- CHANGELOG.md
- Release notes
- Contributors section (coming soon)

## ❓ Questions?

- Open a [Discussion](https://github.com/RandyNorthrup/S.A.K.-Utility/discussions)
- Create an [Issue](https://github.com/RandyNorthrup/S.A.K.-Utility/issues)
- Contact the maintainers

Thank you for contributing to S.A.K. Utility! 🚀
