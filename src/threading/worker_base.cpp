#include "sak/worker_base.h"
#include "sak/logger.h"

WorkerBase::WorkerBase(QObject* parent)
    : QThread(parent)
{
}

WorkerBase::~WorkerBase()
{
    if (isRunning()) {
        request_stop();
        if (!wait(15000)) {
            sak::log_error("Worker thread did not stop within 15s — potential resource leak");
        }
    }
}

void WorkerBase::request_stop() noexcept
{
    m_stop_requested.store(true, std::memory_order_release);
    requestInterruption();
}

bool WorkerBase::stop_requested() const noexcept
{
    return m_stop_requested.load(std::memory_order_acquire);
}

bool WorkerBase::is_running() const noexcept
{
    return m_is_running.load(std::memory_order_acquire);
}

void WorkerBase::run()
{
    m_is_running.store(true, std::memory_order_release);
    m_stop_requested.store(false, std::memory_order_release);
    
    Q_EMIT started();
    
    auto result = execute();
    
    m_is_running.store(false, std::memory_order_release);
    
    if (m_stop_requested.load(std::memory_order_acquire)) {
        Q_EMIT cancelled();
    } else if (result) {
        Q_EMIT finished();
    } else {
        Q_EMIT failed(
            static_cast<int>(result.error()),
            QString::fromStdString(std::string(sak::to_string(result.error())))
        );
    }
}

bool WorkerBase::check_stop()
{
    if (stop_requested()) {
        sak::log_info("Worker cancellation requested");
        return true;
    }
    return false;
}

void WorkerBase::report_progress(int current, int total, const QString& message)
{
    Q_EMIT progress(current, total, message);
}
