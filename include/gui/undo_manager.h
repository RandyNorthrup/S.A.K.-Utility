#pragma once

#include <QObject>
#include <QUndoStack>
#include <QUndoCommand>
#include <QString>
#include <memory>
#include <functional>

namespace sak {

/**
 * @brief Base class for undoable commands
 * 
 * Extends QUndoCommand with additional metadata and error handling.
 * All application commands should derive from this class.
 */
class UndoCommand : public QUndoCommand {
public:
    /**
     * @brief Constructor
     * @param text Description of the command
     * @param parent Parent command for macro grouping
     */
    explicit UndoCommand(const QString& text, QUndoCommand* parent = nullptr);

    /**
     * @brief Get command timestamp
     * @return Timestamp when command was created
     */
    [[nodiscard]] qint64 timestamp() const { return m_timestamp; }

    /**
     * @brief Check if command can be safely undone
     * @return True if undo is safe
     */
    [[nodiscard]] virtual bool canUndo() const { return true; }

    /**
     * @brief Check if command can be safely redone
     * @return True if redo is safe
     */
    [[nodiscard]] virtual bool canRedo() const { return true; }

protected:
    qint64 m_timestamp;
};

/**
 * @brief Lambda-based undo command for simple operations
 * 
 * Allows creating undo commands from lambda functions without
 * creating a new class for each command type.
 */
class LambdaCommand : public UndoCommand {
public:
    using Action = std::function<void()>;

    /**
     * @brief Constructor
     * @param text Description
     * @param redo_action Function to execute on redo
     * @param undo_action Function to execute on undo
     */
    LambdaCommand(const QString& text, Action redo_action, Action undo_action);

    void redo() override;
    void undo() override;

private:
    Action m_redo_action;
    Action m_undo_action;
};

/**
 * @brief Global undo/redo manager
 * 
 * Provides application-wide undo/redo functionality using Qt's
 * QUndoStack with additional features:
 * - Command history persistence
 * - Undo limits
 * - Transaction grouping (macros)
 * - Conditional undo/redo
 * 
 * Thread-Safety: Must be used from main Qt thread
 */
class UndoManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Get singleton instance
     * @return UndoManager instance
     */
    static UndoManager& instance();

    // Disable copy and move
    UndoManager(const UndoManager&) = delete;
    UndoManager& operator=(const UndoManager&) = delete;
    UndoManager(UndoManager&&) = delete;
    UndoManager& operator=(UndoManager&&) = delete;

    /**
     * @brief Push command onto undo stack
     * @param command Command to execute and add to stack
     */
    void push(QUndoCommand* command);

    /**
     * @brief Create and push lambda command
     * @param text Description
     * @param redo_action Redo function
     * @param undo_action Undo function
     */
    void pushLambda(const QString& text, 
                   std::function<void()> redo_action,
                   std::function<void()> undo_action);

    /**
     * @brief Undo last command
     */
    void undo();

    /**
     * @brief Redo last undone command
     */
    void redo();

    /**
     * @brief Check if undo is available
     * @return True if there are commands to undo
     */
    [[nodiscard]] bool canUndo() const;

    /**
     * @brief Check if redo is available
     * @return True if there are commands to redo
     */
    [[nodiscard]] bool canRedo() const;

    /**
     * @brief Get undo text description
     * @return Description of command that will be undone
     */
    [[nodiscard]] QString undoText() const;

    /**
     * @brief Get redo text description
     * @return Description of command that will be redone
     */
    [[nodiscard]] QString redoText() const;

    /**
     * @brief Clear all undo/redo history
     */
    void clear();

    /**
     * @brief Set undo limit
     * @param limit Maximum number of commands in history (0 = unlimited)
     */
    void setUndoLimit(int limit);

    /**
     * @brief Get undo limit
     * @return Current undo limit
     */
    [[nodiscard]] int undoLimit() const;

    /**
     * @brief Begin macro (transaction)
     * @param text Description of macro
     * 
     * All commands pushed until endMacro() are grouped as one command
     */
    void beginMacro(const QString& text);

    /**
     * @brief End macro (transaction)
     */
    void endMacro();

    /**
     * @brief Get command count
     * @return Number of commands in history
     */
    [[nodiscard]] int count() const;

    /**
     * @brief Get command index
     * @return Current position in history
     */
    [[nodiscard]] int index() const;

    /**
     * @brief Access underlying QUndoStack
     * @return Pointer to undo stack
     */
    [[nodiscard]] QUndoStack* undoStack() { return m_undo_stack.get(); }

Q_SIGNALS:
    /**
     * @brief Emitted when undo/redo availability changes
     * @param can_undo True if undo is available
     */
    void canUndoChanged(bool can_undo);

    /**
     * @brief Emitted when undo/redo availability changes
     * @param can_redo True if redo is available
     */
    void canRedoChanged(bool can_redo);

    /**
     * @brief Emitted when undo text changes
     * @param text New undo text
     */
    void undoTextChanged(const QString& text);

    /**
     * @brief Emitted when redo text changes
     * @param text New redo text
     */
    void redoTextChanged(const QString& text);

    /**
     * @brief Emitted when command is pushed
     */
    void commandPushed();

    /**
     * @brief Emitted when stack is cleaned
     */
    void cleanChanged(bool clean);

private:
    explicit UndoManager(QObject* parent = nullptr);
    ~UndoManager() override = default;

    std::unique_ptr<QUndoStack> m_undo_stack;
};

/**
 * @brief RAII macro (transaction) guard
 * 
 * Automatically begins macro on construction and ends on destruction.
 * Perfect for grouping multiple operations into one undoable action.
 */
class UndoMacroGuard {
public:
    /**
     * @brief Constructor - begins macro
     * @param text Description of transaction
     */
    explicit UndoMacroGuard(const QString& text);

    /**
     * @brief Destructor - ends macro
     */
    ~UndoMacroGuard();

    // Disable copy and move
    UndoMacroGuard(const UndoMacroGuard&) = delete;
    UndoMacroGuard& operator=(const UndoMacroGuard&) = delete;
    UndoMacroGuard(UndoMacroGuard&&) = delete;
    UndoMacroGuard& operator=(UndoMacroGuard&&) = delete;
};

} // namespace sak
