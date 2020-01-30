#pragma once

#include "Thread.h"
#include "Timer.h"
#include <atomic>
#include <functional>

namespace WPEFramework {

namespace Core {

    class EXTERNAL WorkerPool {
    private:
        WorkerPool() = delete;
        WorkerPool(const WorkerPool&) = delete;
        WorkerPool& operator=(const WorkerPool&) = delete;

        class Job {
        public:
            Job()
                : _job()
            {
            }
            Job(const Job& copy)
                : _job(copy._job)
            {
            }
            Job(const Core::ProxyType<Core::IDispatch>& job)
                : _job(job)
            {
            }
            ~Job()
            {
            }
            Job& operator=(const Job& RHS)
            {
                _job = RHS._job;

                return (*this);
            }

        public:
            bool operator==(const Job& RHS) const
            {
                return (_job == RHS._job);
            }
            bool operator!=(const Job& RHS) const
            {
                return (!operator==(RHS));
            }
            uint64_t Timed(const uint64_t /* scheduledTime */)
            {
               WorkerPool::Instance().Submit(_job);
                _job.Release();

                // No need to reschedule, just drop it..
                return (0);
            }
            inline void Dispatch()
            {
                ASSERT(_job.IsValid() == true);
                _job->Dispatch();
                _job.Release();
            }
            inline uint64_t Id() {
                return reinterpret_cast<uint64_t>(_job.operator->());
            }

        private:
            Core::ProxyType<Core::IDispatch> _job;
        };

        template <typename IMPLEMENTATION>
        class EXTERNAL DispatcherType : public Core::IDispatch {
        public:
            DispatcherType() = delete;
            DispatcherType(const DispatcherType<IMPLEMENTATION>&) = delete;
            DispatcherType<IMPLEMENTATION>& operator=(const DispatcherType<IMPLEMENTATION>&) = delete;

        public:
            DispatcherType(IMPLEMENTATION* parent)
                : _implementation(*parent)
                , _submitted(false)
            {
            }

            virtual ~DispatcherType()
            {
                _submitted.store(false, std::memory_order_relaxed);
                Core::WorkerPool::Instance().Revoke(Core::ProxyType<Core::IDispatch>(*this));
            }

         public:
            void Submit()
            {
                bool expected = false;
                if (_submitted.compare_exchange_strong(expected, true) == true) {
                    Core::WorkerPool::Instance().Submit(Core::ProxyType<Core::IDispatch>(*this));
                }
            }

        private:
            virtual void Dispatch()
            {
                bool expected = true;
                if (_submitted.compare_exchange_strong(expected, false) == true) {
                    _implementation.Dispatch();
                }
            }

        private:
            IMPLEMENTATION& _implementation;
            std::atomic<bool> _submitted;
        };

    protected:
        class WorkerStatus {
        private:
            WorkerStatus(const WorkerStatus&) = delete;
            WorkerStatus& operator=(const WorkerStatus&) = delete;

        public:
            WorkerStatus() 
                : _jobRunning(true, true)
                , _adminLock()
                , _jobID()
            {
            }
            void JobStarted(uint64_t jobID)
            {
                _adminLock.Lock();   

                _jobRunning.ResetEvent();       
                _jobID = jobID;

                _adminLock.Unlock();
            }
            void JobFinished()
            {
                _jobID = 0;
                _jobRunning.SetEvent();
            }
            uint32_t WaitForJobDone(uint64_t jobID, uint32_t waitTimeMs)
            {
                uint32_t result = Core::ERROR_NONE;

                _adminLock.Lock();

                if (_jobID == jobID) {
                    result = _jobRunning.Lock(waitTimeMs);
                } else {
                    result = Core::ERROR_UNKNOWN_KEY;
                }

                _adminLock.Unlock();

                return result;
            }
        protected:
            Core::Event _jobRunning;
            Core::CriticalSection _adminLock;
            uint64_t _jobID;
        };
        class Minion : public Core::Thread {
        private:
            Minion(const Minion&) = delete;
            Minion& operator=(const Minion&) = delete;

        public:
            Minion()
                : Core::Thread(Core::Thread::DefaultStackSize(), nullptr)
                , _parent(nullptr)
                , _index(0)
            {
            }
            Minion(const uint32_t stackSize)
                : Core::Thread(stackSize, nullptr)
                , _parent(nullptr)
                , _index(0)
            {
            }
            virtual ~Minion()
            {
                Stop();
                Wait(Core::Thread::STOPPED, Core::infinite);
            }

        public:
            void Set(WorkerPool& parent, const uint8_t index)
            {
                _parent = &parent;
                _index = index;
            }
        private:
            virtual uint32_t Worker() override
            {
                _parent->Process(_index);
                Block();
                return (Core::infinite);
            }

        private:
            WorkerPool* _parent;
            uint8_t _index;
        };

        typedef Core::QueueType<Job> MessageQueue;

    public:
        struct Metadata {
            uint32_t Pending;
            uint32_t Occupation;
            uint8_t Slots;
            uint32_t* Slot;
        };

    public:
	static WorkerPool& Instance() {
            ASSERT(_instance != nullptr);
            return (*_instance);
	}
	static bool IsAvailable() {
            return (_instance != nullptr);
	}
        ~WorkerPool();

    public:
        inline void Submit(const Core::ProxyType<Core::IDispatch>& job)
        {
            _handleQueue.Insert(Job(job), Core::infinite);
        }
        inline void Schedule(const Core::Time& time, const Core::ProxyType<Core::IDispatch>& job)
        {
            _timer.Schedule(time, Job(job));
        }
        inline uint32_t Revoke(const Core::ProxyType<Core::IDispatch>& job, const uint32_t waitTime = Core::infinite)
        {
            Job compare(job);
            
            // Check if job is sheduled or run by timer
            uint32_t result = (_timer.Revoke(compare) == true || _handleQueue.Remove(compare) ? Core::ERROR_NONE : Core::ERROR_UNAVAILABLE);

            // Check if the job is run by any of the minions
            for (int i = 1; i < _metadata.Slots; ++i) {
                if (_workerStatuses[i].WaitForJobDone(compare.Id(), waitTime) == Core::ERROR_NONE) {
                    // Job was found running
                    result = Core::ERROR_NONE;
                }
            }

            return result;
        }
        inline const WorkerPool::Metadata& Snapshot()
        {
            _metadata.Occupation = _occupation.load();
            _metadata.Pending = _handleQueue.Length();
            return (_metadata);
        }
        void Join() 
        {
            Process(0);
        }
        void Run()
        {
            _handleQueue.Enable();
            for (uint8_t index = 1; index < _metadata.Slots; index++) {
                Minion& minion = Index(index);
                minion.Set(*this, index);
                minion.Run();
            }
        }
        void Stop()
        {
            _handleQueue.Disable();
            for (uint8_t index = 1; index < _metadata.Slots; index++) {
                Minion& minion = Index(index);
                minion.Block();
                minion.Wait(Core::Thread::BLOCKED | Core::Thread::STOPPED, Core::infinite);
            }
        }

        inline ThreadId Id(const uint8_t index) const {
            ThreadId result = 0;

            if (index == 0) {
                result = _timer.ThreadId();
            }
            else if (index == 1) {
                result = 0;
            }
            else if (index < _metadata.Slots) {
                result = const_cast<WorkerPool&>(*this).Index(index - 1).ThreadId();
            }

            return (result);
        }

    protected:
        WorkerPool(const uint8_t threadCount, uint32_t* counters);

        virtual Minion& Index(const uint8_t index) = 0;
        virtual bool Running() = 0;

        void Process(const uint8_t index)
        {
            Job newRequest;

            while ((Running() == true) && (_handleQueue.Extract(newRequest, Core::infinite) == true)) {
                _workerStatuses[index].JobStarted(newRequest.Id());

                _metadata.Slot[index]++;

                _occupation++;

                newRequest.Dispatch();

                _occupation--;

                _workerStatuses[index].JobFinished();
            }
        }

    private:
        MessageQueue _handleQueue;
        std::atomic<uint8_t> _occupation;
        Core::TimerType<Job> _timer;
        Metadata _metadata;
        WorkerStatus* _workerStatuses;
        static WorkerPool* _instance;
    };

    template <const uint8_t THREAD_COUNT>
    class WorkerPoolType : public WorkerPool {
    private:
        WorkerPoolType() = delete;
        WorkerPoolType(const WorkerPoolType<THREAD_COUNT>&) = delete;
        WorkerPoolType<THREAD_COUNT>& operator=(const WorkerPoolType<THREAD_COUNT>&) = delete;

    public:
        WorkerPoolType(const uint32_t stackSize)
            : WorkerPool(THREAD_COUNT, &(_counters[0]))
            , _minions()
        {
        }
        virtual ~WorkerPoolType()
        {
            Stop();
        }

        inline uint32_t ThreadId(const uint8_t index) const 
        {
            return (((index > 0) && (index < THREAD_COUNT)) ? _minions[index-1].ThreadId() : static_cast<uint32_t>(~0));
        }

    private:
        virtual Minion& Index(const uint8_t index) override 
        {
            return (_minions[index-1]);
        }
        virtual bool Running() override
        {
            return (true);
        }

    private:
        Minion _minions[THREAD_COUNT - 1];
        uint32_t _counters[THREAD_COUNT];
    };
}
}
