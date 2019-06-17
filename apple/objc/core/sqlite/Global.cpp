/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <WCDB/Assertion.hpp>
#include <WCDB/Global.hpp>
#include <WCDB/Notifier.hpp>
#include <WCDB/SQLite.h>
#include <fcntl.h>

namespace WCDB {

static_assert((int) Global::PagerLock::None == SQLITE_LOCK_NONE, "");
static_assert((int) Global::PagerLock::Shared == SQLITE_LOCK_SHARED, "");
static_assert((int) Global::PagerLock::Reserved == SQLITE_LOCK_RESERVED, "");
static_assert((int) Global::PagerLock::Pending == SQLITE_LOCK_PENDING, "");
static_assert((int) Global::PagerLock::Exclusive == SQLITE_LOCK_EXCLUSIVE, "");

static_assert((int) Global::ShmLock::Shared == SQLITE_SHM_SHARED, "");
static_assert((int) Global::ShmLock::Exclusive == SQLITE_SHM_EXCLUSIVE, "");

void Global::initialize()
{
    Global::shared();
}

Global &Global::shared()
{
    static Global *s_shared = new Global;
    return *s_shared;
}

Global::Global()
{
    {
        int rc = sqlite3_lock_hook(
        Global::willLock, Global::lockDidChange, Global::willShmLock, Global::shmLockDidChange, this);
        WCTInnerAssert(rc == SQLITE_OK);
        staticAPIExit(rc);
    }

    {
        int rc = sqlite3_config(SQLITE_CONFIG_LOG, Global::log, this);
        WCTInnerAssert(rc == SQLITE_OK);
        staticAPIExit(rc);
    }

    {
        int rc = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
        WCTInnerAssert(rc == SQLITE_OK);
        staticAPIExit(rc);
    }

    {
        int rc = sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);
        WCTInnerAssert(rc == SQLITE_OK);
        staticAPIExit(rc);
    }

    {
        sqlite3_vfs *vfs = sqlite3_vfs_find(nullptr);
        WCTInnerAssert(vfs != nullptr);
        int rc = vfs->xSetSystemCall(vfs, "open", (sqlite3_syscall_ptr) Global::open);
        WCTInnerAssert(rc == SQLITE_OK);
        staticAPIExit(rc);
    }
}

void Global::staticAPIExit(int rc)
{
    if (Error::isError(rc)) {
        Error error;
        error.level = Error::Level::Fatal;
        error.setSQLiteCode(rc);
        Notifier::shared().notify(error);
    }
}

//    Global::shared().setMemoryMapSize(0x7fff0000, 0x7fff0000);
//void Global::setMemoryMapSize(int64_t defaultSizeLimit, int64_t maximumAllowedSizeLimit)
//{
//    int rc = sqlite3_config(SQLITE_CONFIG_MMAP_SIZE,
//                   (sqlite3_int64) defaultSizeLimit,
//                   (sqlite3_int64) maximumAllowedSizeLimit);
//    WCTInnerAssert(rc == SQLITE_OK);
//    staticAPIExit(rc);
//}

#pragma mark - Log
void Global::setNotificationForLog(const UnsafeStringView &name, const LogNotification &notification)
{
    LockGuard lockGuard(m_lock);
    if (notification != nullptr) {
        m_logNotifications[name] = notification;
    } else {
        m_logNotifications.erase(name);
    }
}

void Global::postLogNotification(int rc, const char *message)
{
    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_logNotifications) {
        WCTInnerAssert(iter.second != nullptr);
        iter.second(rc, message);
    }
}

void Global::log(void *parameter, int rc, const char *message)
{
    Global *global = static_cast<Global *>(parameter);
    global->postLogNotification(rc, message);
}

#pragma mark - File Opened
void Global::setNotificationWhenFileOpened(const UnsafeStringView &name,
                                           const FileOpenedNotification &notification)
{
    LockGuard lockGuard(m_lock);
    if (notification != nullptr) {
        m_fileOpenedNotifications[name] = notification;
    } else {
        m_fileOpenedNotifications.erase(name);
    }
}

int Global::open(const char *path, int flags, int mode)
{
    int fd = ::open(path, flags, mode);
    Global::shared().postFileOpenedNotification(fd, path, flags, mode);
    return fd;
}

void Global::postFileOpenedNotification(int fd, const char *path, int flags, int mode)
{
    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_fileOpenedNotifications) {
        WCTInnerAssert(iter.second != nullptr);
        iter.second(fd, path, flags, mode);
    }
}

#pragma mark - Lock
void Global::setNotificationForLockEvent(const UnsafeStringView &name,
                                         const WillLockNotification &willLock,
                                         const LockDidChangeNotification &lockDidChange,
                                         const WillShmLockNotification &willShmLock,
                                         const ShmLockDidChangeNotification &shmLockDidChange)
{
    LockGuard lockGuard(m_lock);
    LockEvent event;
    event.willLock = willLock;
    event.lockDidChange = lockDidChange;
    event.willShmLock = willShmLock;
    event.shmLockDidChange = shmLockDidChange;
    m_lockEventNotifications[name] = event;
}

void Global::willLock(void *parameter, const char *path, int type)
{
    reinterpret_cast<Global *>(parameter)->postWillLockNotification(path, type);
}

void Global::postWillLockNotification(const char *path_, int type_)
{
    UnsafeStringView path = path_;
    WCTInnerAssert(!path.empty());

    PagerLock type = (PagerLock) type_;
    WCTInnerAssert(type == PagerLock::Shared || type == PagerLock::Reserved
                   || type == PagerLock::Exclusive);

    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_lockEventNotifications) {
        if (iter.second.willLock != nullptr) {
            iter.second.willLock(path, type);
        }
    }
}

void Global::lockDidChange(void *parameter, const char *path, int type)
{
    reinterpret_cast<Global *>(parameter)->postLockDidChangeNotification(path, type);
}

void Global::postLockDidChangeNotification(const char *path_, int type_)
{
    UnsafeStringView path = path_;
    WCTInnerAssert(!path.empty());

    PagerLock type = (PagerLock) type_;
    WCTInnerAssert(type == PagerLock::None || type == PagerLock::Shared
                   || type == PagerLock::Reserved || type == PagerLock::Pending
                   || type == PagerLock::Exclusive);

    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_lockEventNotifications) {
        if (iter.second.lockDidChange != nullptr) {
            iter.second.lockDidChange(path, type);
        }
    }
}

void Global::willShmLock(void *parameter, const char *path, int flags, int mask)
{
    reinterpret_cast<Global *>(parameter)->postWillShmLockNotification(path, flags, mask);
}

void Global::postWillShmLockNotification(const char *path_, int flags, int mask)
{
    UnsafeStringView path = path_;
    WCTInnerAssert(!path.empty());

    ShmLock type;
    if ((flags & SQLITE_SHM_SHARED) != 0) {
        WCTInnerAssert((flags & SQLITE_SHM_EXCLUSIVE) == 0);
        type = ShmLock::Shared;
    } else {
        WCTInnerAssert((flags & SQLITE_SHM_EXCLUSIVE) != 0);
        type = ShmLock::Exclusive;
    }

    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_lockEventNotifications) {
        if (iter.second.willShmLock != nullptr) {
            iter.second.willShmLock(path, type, mask);
        }
    }
}

void Global::shmLockDidChange(void *parameter, const char *path, void *identifier, int sharedMask, int exclusiveMask)
{
    reinterpret_cast<Global *>(parameter)->postShmLockDidChangeNotification(
    path, identifier, sharedMask, exclusiveMask);
}

void Global::postShmLockDidChangeNotification(const char *path_,
                                              void *identifier,
                                              int sharedMask,
                                              int exclusiveMask)
{
    WCTInnerAssert(identifier != nullptr);

    UnsafeStringView path = path_;
    WCTInnerAssert(!path.empty());

    SharedLockGuard lockGuard(m_lock);
    for (const auto &iter : m_lockEventNotifications) {
        if (iter.second.shmLockDidChange != nullptr) {
            iter.second.shmLockDidChange(path, identifier, sharedMask, exclusiveMask);
        }
    }
}

} // namespace WCDB
