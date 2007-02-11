/*
 *  Copyright 2001-2007 Internet2
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

/**
 * @file shibsp/TransactionLog.h
 * 
 * Interface to a synchronized logging object.
 */

#ifndef __shibsp_txlog_h__
#define __shibsp_txlog_h__

#include <shibsp/base.h>
#include <log4cpp/Category.hh>
#include <xmltooling/Lockable.h>
#include <xmltooling/util/Threads.h>

namespace shibsp {

    /**
     * Interface to a synchronized logging object.
     * 
     * <p>This is platform/logging specific, but we can at least hide the details here.
     */
    class SHIBSP_API TransactionLog : public virtual xmltooling::Lockable
    {
        MAKE_NONCOPYABLE(TransactionLog);
    public:
        TransactionLog()
            : log(log4cpp::Category::getInstance(SHIBSP_TX_LOGCAT)), m_lock(xmltooling::Mutex::create()) {
        }

        virtual ~TransactionLog() {
            delete m_lock;
        }
        
        xmltooling::Lockable* lock() {
            m_lock->lock();
            return this;
        }

        void unlock() {
            m_lock->unlock();
        }

        /** Logging object. */
        log4cpp::Category& log;

    private:
        xmltooling::Mutex* m_lock;
    };
};

#endif /* __shibsp_txlog_h__ */