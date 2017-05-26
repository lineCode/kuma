/* Copyright (c) 2014, Fengping Bao <jamol@live.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "EventLoopImpl.h"
#include "poll/IOPoll.h"
#include "util/kmqueue.h"
#include "util/kmtrace.h"
#include <thread>
#include <condition_variable>

KUMA_NS_BEGIN

IOPoll* createIOPoll(PollType poll_type);

EventLoop::Impl::Impl(PollType poll_type)
: poll_(createIOPoll(poll_type))
, timer_mgr_(new TimerManager(this))
{
    KM_SetObjKey("EventLoop");
}

EventLoop::Impl::~Impl()
{
    if(poll_) {
        delete poll_;
        poll_ = nullptr;
    }
}

bool EventLoop::Impl::init()
{
    if(!poll_->init()) {
        return false;
    }
    stop_loop_ = false;
    thread_id_ = std::this_thread::get_id();
    return true;
}

PollType EventLoop::Impl::getPollType() const
{
    if(poll_) {
        return poll_->getType();
    }
    return PollType::NONE;
}

bool EventLoop::Impl::isPollLT() const
{
    if(poll_) {
        return poll_->isLevelTriggered();
    }
    return false;
}

KMError EventLoop::Impl::registerFd(SOCKET_FD fd, uint32_t events, IOCallback cb)
{
    if(inSameThread()) {
        return poll_->registerFd(fd, events, std::move(cb));
    }
    return async([=] () mutable {
        auto ret = poll_->registerFd(fd, events, cb);
        if(ret != KMError::NOERR) {
            return ;
        }
    });
}

KMError EventLoop::Impl::updateFd(SOCKET_FD fd, uint32_t events)
{
    if(inSameThread()) {
        return poll_->updateFd(fd, events);
    }
    return async([=] {
        auto ret = poll_->updateFd(fd, events);
        if(ret != KMError::NOERR) {
            return ;
        }
    });
}

KMError EventLoop::Impl::unregisterFd(SOCKET_FD fd, bool close_fd)
{
    if(inSameThread()) {
        auto ret = poll_->unregisterFd(fd);
        if(close_fd) {
            closeFd(fd);
        }
        return ret;
    } else {
        auto ret = sync([=] {
            poll_->unregisterFd(fd);
            if(close_fd) {
                closeFd(fd);
            }
        });
        if(KMError::NOERR != ret) {
            return ret;
        }
        return KMError::NOERR;
    }
}

KMError EventLoop::Impl::appendObserver(ObserverCallback cb, EventLoopToken *token)
{
    if (token && token->eventLoop().get() != this) {
        return KMError::INVALID_PARAM;
    }
    LockGuard g(obs_mutex_);
    if (stop_loop_) {
        return KMError::INVALID_STATE;
    }
    auto obs_node = obs_queue_.enqueue(std::move(cb));
    if (token) {
        token->obs_token_ = obs_node;
        token->observed = true;
    }
    return KMError::NOERR;
}

KMError EventLoop::Impl::removeObserver(EventLoopToken *token)
{
    if (token) {
        if (token->eventLoop().get() != this) {
            return KMError::INVALID_STATE;
        }
        auto node = token->obs_token_.lock();
        if (node) {
            LockGuard g(obs_mutex_);
            obs_queue_.remove(node);
        }
        token->obs_token_.reset();
        token->observed = false;
    }
    return KMError::NOERR;
}

void EventLoop::Impl::processTasks()
{
    TaskQueue tq;
    task_mutex_.lock();
    task_queue_.swap(tq);
    
    while (auto node = tq.front_node()) {
        tq.pop_front();
        node->element_.state = TaskSlot::State::RUNNING;
        task_mutex_.unlock();
        task_run_mutex_.lock();
        if (node->element_.state != TaskSlot::State::INACTIVE) {
            node->element_();
            node->element_.state = TaskSlot::State::INACTIVE;
        }
        task_run_mutex_.unlock();
        task_mutex_.lock();
        if (node->element_.token) {
            node->element_.token->removeTaskNode(node);
        }
    }
    task_mutex_.unlock();
}

void EventLoop::Impl::loopOnce(uint32_t max_wait_ms)
{
    processTasks();
    unsigned long wait_ms = max_wait_ms;
    timer_mgr_->checkExpire(&wait_ms);
    if(wait_ms > max_wait_ms) {
        wait_ms = max_wait_ms;
    }
    poll_->wait((uint32_t)wait_ms);
}

void EventLoop::Impl::loop(uint32_t max_wait_ms)
{
    while (!stop_loop_) {
        loopOnce(max_wait_ms);
    }
    processTasks();
    
    {
        LockGuard g(obs_mutex_);
        ObserverCallback cb;
        while (obs_queue_.dequeue(cb)) {
            cb(LoopActivity::EXIT);
        }
    }
    KUMA_INFOXTRACE("loop, stopped");
}

void EventLoop::Impl::notify()
{
    poll_->notify();
}

void EventLoop::Impl::stop()
{
    KUMA_INFOXTRACE("stop");
    stop_loop_ = true;
    poll_->notify();
}

KMError EventLoop::Impl::appendTask(Task task, EventLoopToken *token)
{
    if (token && token->eventLoop().get() != this) {
        return KMError::INVALID_PARAM;
    }
    auto node = std::make_shared<TaskQueue::DLNode>(std::move(task), token);
    LockGuard g(task_mutex_);
    if (stop_loop_) {
        return KMError::INVALID_STATE;
    }
    task_queue_.enqueue(node);
    if (token) {
        token->appendTaskNode(node);
    }
    return KMError::NOERR;
}

KMError EventLoop::Impl::removeTask(EventLoopToken *token)
{
    if (!token || token->eventLoop().get() != this) {
        return KMError::INVALID_PARAM;
    }
    bool is_running = false;
    {
        LockGuard g(task_mutex_);
        for (auto &node : token->node_queue_) {
            if (node->element_.state == TaskSlot::State::RUNNING) {
                is_running = true;
                node->element_.state = TaskSlot::State::INACTIVE;
            }
            task_queue_.remove(node);
        }
        token->node_queue_.clear();
    }
    if (is_running && !inSameThread()) {
        // wait for end of running
        task_run_mutex_.lock();
        task_run_mutex_.unlock();
    }
    return KMError::NOERR;
}

KMError EventLoop::Impl::sync(Task task)
{
    if(inSameThread()) {
        task();
    } else {
        std::mutex m;
        std::condition_variable cv;
        bool ready = false;
        Task task_sync([&] {
            task();
            std::unique_lock<std::mutex> lk(m);
            ready = true;
            lk.unlock();
            cv.notify_one();
        });
        auto ret = queue(std::move(task_sync));
        if (ret != KMError::NOERR) {
            return ret;
        }
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&ready] { return ready; });
    }
    return KMError::NOERR;
}

KMError EventLoop::Impl::async(Task task, EventLoopToken *token)
{
    if(inSameThread()) {
        task();
        return KMError::NOERR;
    } else {
        return queue(std::move(task), token);
    }
}

KMError EventLoop::Impl::queue(Task task, EventLoopToken *token)
{
    auto ret = appendTask(std::move(task), token);
    if (ret != KMError::NOERR) {
        return ret;
    }
    poll_->notify();
    return KMError::NOERR;
}

/////////////////////////////////////////////////////////////////
// EventLoopToken
EventLoopToken::EventLoopToken()
{
    
}

EventLoopToken::~EventLoopToken()
{
    reset();
}

void EventLoopToken::eventLoop(const EventLoopPtr &loop)
{
    loop_ = loop;
}

EventLoopPtr EventLoopToken::eventLoop()
{
    return loop_.lock();
}

void EventLoopToken::appendTaskNode(TaskNodePtr &node)
{
    node_queue_.emplace_back(node);
}

void EventLoopToken::removeTaskNode(TaskNodePtr &node)
{
    for (auto it = node_queue_.begin(); it != node_queue_.end(); ++it) {
        if (*it == node) {
            node_queue_.erase(it);
            break;
        }
    }
}

bool EventLoopToken::expired()
{
    return loop_.expired() || (observed && obs_token_.expired());
}

void EventLoopToken::reset()
{
    auto loop = loop_.lock();
    if (loop) {
        if (!node_queue_.empty()) {
            loop->removeTask(this);
        }
        if (!obs_token_.expired()) {
            loop->removeObserver(this);
            obs_token_.reset();
        }
        loop_.reset();
    } else {
        node_queue_.clear();
    }
}

/////////////////////////////////////////////////////////////////
//
IOPoll* createEPoll();
IOPoll* createVPoll();
IOPoll* createKQueue();
IOPoll* createSelectPoll();
IOPoll* createIocpPoll();

#ifdef KUMA_OS_WIN
# include <MSWSock.h>
extern LPFN_CONNECTEX connect_ex;
extern LPFN_ACCEPTEX accept_ex;
#endif

IOPoll* createDefaultIOPoll()
{
#ifdef KUMA_OS_WIN
    if (connect_ex && accept_ex) {
        return createIocpPoll();
    }
    return createSelectPoll();
#elif defined(KUMA_OS_LINUX)
    return createEPoll();
#elif defined(KUMA_OS_MAC)
    return createKQueue();
    //return createVPoll();
#else
    return createSelectPoll();
#endif
}

IOPoll* createIOPoll(PollType poll_type)
{
    switch (poll_type)
    {
        case PollType::POLL:
            return createVPoll();
        case PollType::SELECT:
            return createSelectPoll();
        case PollType::KQUEUE:
#ifdef KUMA_OS_MAC
            return createKQueue();
#else
            return createDefaultIOPoll();
#endif
        case PollType::EPOLL:
#ifdef KUMA_OS_LINUX
            return createEPoll();
#else
            return createDefaultIOPoll();
#endif
        case PollType::IOCP:
#ifdef KUMA_OS_WIN
            return createIocpPoll();
#else
            return createDefaultIOPoll();
#endif
        default:
            return createDefaultIOPoll();
    }
}

KUMA_NS_END
