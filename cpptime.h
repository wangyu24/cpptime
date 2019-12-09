#ifndef CPPTIME_H_
#define CPPTIME_H_

/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Michael Egli
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * \author    Michael Egli
 * \copyright Michael Egli
 * \date      11-Jul-2015
 *
 * \file cpptime.h
 *
 * C++11 timer component
 * =====================
 *
 * A portable, header-only C++11 timer component.
 *
 * Overview
 * --------
 *
 * This component can be used to manage a set of timeouts. It is implemented in
 * pure C++11. It is therefore very portable given a compliant compiler.
 *
 * A timeout can be added with one of the `add` functions, and removed with the
 * `remove` function. A timeout can be either one-shot or periodic. In case a
 * timeout is one-shot, the callback is invoked once and the timeout event is
 * then automatically removed. If the timer is periodic, it is never
 * automatically removed, but always renewed.
 *
 * Removing a timeout is possible even from within the callback.
 *
 * Timeout Units
 * -------------
 *
 * The preferred functions for adding timeouts are those that take a
 * `std::chrono::...` argument. However, for convenience, there is also an API
 * that takes a uint64_t. When using this API, all values are expected to be
 * given in microseconds (us).
 *
 * For periodic timeouts, a separate timeout can be specified for the initial
 * (first) timeout, and the periodicity after that.
 *
 * To avoid drifts, times are added by simply adding the period to the intially
 * calculated (or provided) time. Also, we use `wait until` type of API to wait
 * for a timeout instead of a `wait for` API.
 *
 * Data Structure
 * --------------
 *
 * Internally, a std::vector is used to store timeout events. The timer_id
 * returned from the `add` functions are used as index to this vector.
 *
 * In addition, a std::multiset is used that holds all time points when
 * timeouts expire.
 *
 * Using a vector to store timeout events has some implications. It is very
 * fast to remove an event, because the timer_id is the vector's index. On the
 * other hand, this makes it also more complicated to manage the timer_ids. The
 * current solution is to keep track of ids that are freed in order to re-use
 * them. A stack is used for this.
 *
 * Examples
 * --------
 *
 * More examples can be found in the `tests` folder.
 *
 * ~~~
 * CppTime::Timer t;
 * t.add(std::chrono::seconds(1), [](CppTime::timer_id){ std::cout << "got it!"; });
 * std::this_thread::sleep_for(std::chrono::seconds(2));
 * ~~~
 */

// Includes
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <vector>
#include <stack>
#include <set>
#include <tuple>

namespace CppTime
{

// Public types
using timer_id = std::size_t;
using clock = std::chrono::steady_clock;
using timestamp = std::chrono::time_point<clock>;
using duration = std::chrono::microseconds;

// Private definitions. Do not rely on this namespace.
namespace detail
{
// The event structure that holds the information about a timer.
template< class Function, class... Args >
struct Event {
        timer_id id;
        timestamp start;
        duration period;
        Function f;
  std::tuple<Args...> args;
        bool valid;
        Event()
            : id(0), start(duration::zero()), period(duration::zero()), valid(false)
        {
        }
        Event(timer_id id, timestamp start, duration period, Function ff, Args... ss)
            : id(id), start(start), period(period), f(ff), args(std::forward<Args>(ss)...), valid(true)
        {
        }
        Event(Event &&r) = default;
        Event &operator=(Event &&ev) = default;
        Event(const Event &r) = default;
        Event &operator=(const Event &r) = default;
};

// A time event structure that holds the next timeout and a reference to its
// Event struct.
struct Time_event {
        timestamp next;
        timer_id ref;
};

inline bool operator<(const Time_event &l, const Time_event &r)
{
        return l.next < r.next;
}

} // end namespace detail

template< class Function, class... Args >
class Timer
{
        using scoped_m = std::unique_lock<std::mutex>;

        // Thread and locking variables.
        std::mutex m;
        std::condition_variable cond;
        std::thread worker;

        // Use to terminate the timer thread.
        bool done = false;

        // The vector that holds all active events.
        std::vector<detail::Event<Function, Args...>> events;
        // Sorted queue that has the next timeout at its top.
        std::multiset<detail::Time_event> time_events;

        // A list of ids to be re-used. If possible, ids are used from this pool.
        std::stack<CppTime::timer_id> free_ids;

public:
        Timer() : m{}, cond{}, worker{}, events{}, time_events{}, free_ids{}
        {
                scoped_m lock(m);
                done = false;
                worker = std::thread(&Timer::run, this);
        }
  Timer(const Timer& other) = default;
  Timer& operator=(Timer&& other) = default;
        Timer(Timer &&r) = default;
        Timer &operator=(const Timer &r) = default;

        ~Timer()
        {
                scoped_m lock(m);
                done = true;
                lock.unlock();
                cond.notify_all();
                worker.join();
                events.clear();
                time_events.clear();
                while(!free_ids.empty()) {
                        free_ids.pop();
                }
        }

  bool empty() {
   scoped_m lock(m);
   for (auto &e : events) {
     if (e.valid) {
       return false;
     }
   }
   return true;
  }

        /**
         * Add a new timer.
         *
         * \param when The time at which the handler is invoked.
         * \param handler The callable that is invoked when the timer fires.
         * \param period The periodicity at which the timer fires. Only used for periodic timers.
         */
        timer_id add(
            const timestamp &when, const duration &period, const Function& f, Args&&... args)
        {
                scoped_m lock(m);
                timer_id id = 0;
                // Add a new event. Prefer an existing and free id. If none is available, add
                // a new one.
                if(free_ids.empty()) {
                        id = events.size();
                        events.emplace_back(id, when, period, f, std::forward<Args>(args)...);
                } else {
                        id = free_ids.top();
                        free_ids.pop();
                        events[id] = {id, when, period, f, std::forward<Args>(args)...};
                }
                time_events.insert(detail::Time_event{when, id});
                lock.unlock();
                cond.notify_all();
                return id;
        }

        /**
         * Overloaded `add` function that uses a `std::chrono::duration` instead of a
         * `time_point` for the first timeout.
         */
        template <class Rep, class Period>
        inline timer_id add(const std::chrono::duration<Rep, Period> &when,
            const duration &period, const Function& f, Args&&... args)
        {
                return add(clock::now() + std::chrono::duration_cast<std::chrono::microseconds>(when),
                          period, f, std::forward<Args>(args)...);
        }

        /**
         * Overloaded `add` function that uses a uint64_t instead of a `time_point` for
         * the first timeout and the period.
         */
        inline timer_id add(const uint64_t when, const uint64_t period, const Function& f, Args&&... args)
        {
                return add(duration(when), duration(period), f, std::forward<Args>(args)...);
        }

        /**
         * Removes the timer with the given id.
         */
        bool remove(timer_id id)
        {
                scoped_m lock(m);
                if(events.size() == 0 || events.size() < id) {
                        return false;
                }
                events[id].valid = false;
                auto it = std::find_if(time_events.begin(), time_events.end(),
                    [&](const detail::Time_event &te) { return te.ref == id; });
                if(it != time_events.end()) {
                        free_ids.push(it->ref);
                        time_events.erase(it);
                }
                lock.unlock();
                cond.notify_all();
                return true;
        }

private:
        void run()
        {
                scoped_m lock(m);
                while(!done) {
                        if(time_events.empty()) {
                                // Wait for work
                                cond.wait(lock);
                        } else {
                                detail::Time_event te = *time_events.begin();
                                if(CppTime::clock::now() >= te.next) {

                                        // Remove time event
                                        time_events.erase(time_events.begin());

                                        // Invoke the handler
                                        lock.unlock();
          std::apply(events[te.ref].f, events[te.ref].args);
                                        lock.lock();

                                        if(events[te.ref].valid && events[te.ref].period.count() > 0) {
                                                // The event is valid and a periodic timer.
                                                te.next += events[te.ref].period;
                                                time_events.insert(te);
                                        } else {
                                                // The event is either no longer valid because it was removed in the
                                                // callback, or it is a one-shot timer.
                                                events[te.ref].valid = false;
                                                free_ids.push(te.ref);
                                        }
                                } else {
                                        cond.wait_until(lock, te.next);
                                }
                        }
                }
        }
};

} // end namespace CppTime

#endif // CPPTIME_H_
