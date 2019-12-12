/*

***********************************************************************************
* Copyright (C) 2016 - 2019, BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef __FAST_LOCK_H__
#define __FAST_LOCK_H__

#include <atomic>

class FastLock
{
public:
    explicit FastLock(std::atomic_flag &);
    ~FastLock();

    FastLock(const FastLock&) = delete;
    FastLock& operator = (FastLock) = delete;

    FastLock(FastLock&&) = delete;
    FastLock& operator = (FastLock&&) = delete;

private:
     std::atomic_flag   &flag_;
};

class FastTryLock
{
public:
   class LockingFailed {};

   explicit FastTryLock(std::atomic_flag &);
   ~FastTryLock();

   FastTryLock(const FastTryLock&) = delete;
   FastTryLock& operator = (FastTryLock) = delete;

   FastTryLock(FastTryLock&&) = delete;
   FastTryLock& operator = (FastTryLock&&) = delete;

private:
   std::atomic_flag   &flag_;
};

#endif // __FAST_LOCK_H__
