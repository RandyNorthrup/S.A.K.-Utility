#include "gui/undo_manager.h"
#include "sak/logger.h"
#include <QDateTime>

namespace sak {

// ============================================================================
// UndoCommand Implementation
// ============================================================================

UndoCommand::UndoCommand(const QString& text, QUndoCommand* parent)
    : QUndoCommand(text, parent)
    , m_timestamp(QDateTime::currentMSecsSinceEpoch())
{
}

// ============================================================================
// LambdaCommand Implementation
// ============================================================================

LambdaCommand::LambdaCommand(const QString& text, Action redo_action, Action undo_action)
    : UndoCommand(text)
    , m_redo_action(std::move(redo_action))
    , m_undo_action(std::move(undo_action))
{
}

void LambdaCommand::redo()
{
    try {
        if (m_redo_action) {
            m_redo_action();
            sak::log_debug("Executed redo: {}", text().toStdString());
        }
    }
    catch (const std::exception& e) {
        sak::log_error("Redo failed for '{}': {}", text().toStdString(), e.what());
    }
}

void LambdaCommand::undo()
{
    try {
        if (m_undo_action) {
            m_undo_action();
            sak::log_debug("Executed undo: {}", text().toStdString());
        }
    }
    catch (const std::exception& e) {
        sak::log_error("Undo failed for '{}': {}", text().toStdString(), e.what());
    }
}

// ============================================================================
// UndoManager Implementation
// ============================================================================

UndoManager& UndoManager::instance()
{
    static UndoManager s_instance;
    return s_instance;
}

UndoManager::UndoManager(QObject* parent)
    : QObject(parent)
    , m_undo_stack(std::make_unique<QUndoStack>(this))
{
    // Connect signals for state changes
    connect(m_undo_stack.get(), &QUndoStack::canUndoChanged,
            this, &UndoManager::canUndoChanged);
    connect(m_undo_stack.get(), &QUndoStack::canRedoChanged,
            this, &UndoManager::canRedoChanged);
    connect(m_undo_stack.get(), &QUndoStack::undoTextChanged,
            this, &UndoManager::undoTextChanged);
    connect(m_undo_stack.get(), &QUndoStack::redoTextChanged,
            this, &UndoManager::redoTextChanged);
    connect(m_undo_stack.get(), &QUndoStack::cleanChanged,
            this, &UndoManager::cleanChanged);

    // Set reasonable default undo limit (0 = unlimited)
    m_undo_stack->setUndoLimit(100);

    sak::log_info("UndoManager initialized with limit: {}", m_undo_stack->undoLimit());
}

void UndoManager::push(QUndoCommand* command)
{
    if (!command) {
        sak::log_warning("Attempted to push null command to undo stack");
        return;
    }

    m_undo_stack->push(command);
    Q_EMIT commandPushed();
    
    sak::log_debug("Pushed command to undo stack: {} (stack size: {})", 
                  command->text().toStdString(), m_undo_stack->count());
}

void UndoManager::pushLambda(const QString& text,
                            std::function<void()> redo_action,
                            std::function<void()> undo_action)
{
    auto* command = new LambdaCommand(text, std::move(redo_action), std::move(undo_action));
    push(command);
}

void UndoManager::undo()
{
    if (canUndo()) {
        const QString text = undoText();
        m_undo_stack->undo();
        sak::log_info("Undid: {}", text.toStdString());
    } else {
        sak::log_warning("Undo requested but no commands available");
    }
}

void UndoManager::redo()
{
    if (canRedo()) {
        const QString text = redoText();
        m_undo_stack->redo();
        sak::log_info("Redid: {}", text.toStdString());
    } else {
        sak::log_warning("Redo requested but no commands available");
    }
}

bool UndoManager::canUndo() const
{
    return m_undo_stack->canUndo();
}

bool UndoManager::canRedo() const
{
    return m_undo_stack->canRedo();
}

QString UndoManager::undoText() const
{
    return m_undo_stack->undoText();
}

QString UndoManager::redoText() const
{
    return m_undo_stack->redoText();
}

void UndoManager::clear()
{
    const int count = m_undo_stack->count();
    m_undo_stack->clear();
    sak::log_info("Cleared undo stack ({} commands removed)", count);
}

void UndoManager::setUndoLimit(int limit)
{
    m_undo_stack->setUndoLimit(limit);
    sak::log_info("Set undo limit to: {}", limit == 0 ? "unlimited" : std::to_string(limit));
}

int UndoManager::undoLimit() const
{
    return m_undo_stack->undoLimit();
}

void UndoManager::beginMacro(const QString& text)
{
    m_undo_stack->beginMacro(text);
    sak::log_debug("Begin macro: {}", text.toStdString());
}

void UndoManager::endMacro()
{
    m_undo_stack->endMacro();
    sak::log_debug("End macro");
}

int UndoManager::count() const
{
    return m_undo_stack->count();
}

int UndoManager::index() const
{
    return m_undo_stack->index();
}

// ============================================================================
// UndoMacroGuard Implementation
// ============================================================================

UndoMacroGuard::UndoMacroGuard(const QString& text)
{
    UndoManager::instance().beginMacro(text);
}

UndoMacroGuard::~UndoMacroGuard()
{
    UndoManager::instance().endMacro();
}

} // namespace sak
