/*
 * Copyright 2007-2026 The SABnzbd-Team (sabnzbd.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "writebehind.h"
#include "yenc.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <sys/mman.h>
#endif

namespace {

/* Buffers come straight from the OS, so freeing one always returns its memory */
char *buffer_map() {
#if defined(_WIN32) || defined(__CYGWIN__)
    return static_cast<char *>(VirtualAlloc(NULL, YENC_STAGING_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void *buffer = mmap(nullptr, YENC_STAGING_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return buffer == MAP_FAILED ? nullptr : static_cast<char *>(buffer);
#endif
}

void buffer_unmap(char *buffer) {
    if (!buffer) return;
#if defined(_WIN32) || defined(__CYGWIN__)
    VirtualFree(buffer, 0, MEM_RELEASE);
#else
    munmap(buffer, YENC_STAGING_SIZE);
#endif
}

struct Job {
    FileWriter *writer;
    char *buffer;
    Py_ssize_t length;
    long long offset;
};

// Never destroyed, as writer threads are still waiting on it at exit
struct Pool {
    std::mutex lock;
    std::condition_variable job_ready;
    std::condition_variable buffer_free;
    std::condition_variable job_done;
    std::deque<Job> jobs;
    std::vector<char *> free_buffers;
    // Buffers that exist, free or in use, never more than pool_buffers
    Py_ssize_t allocated = 0;
    // Set by trim: buffers coming back are freed rather than kept, until the next acquire
    bool trimming = false;
};
Pool &pool = *new Pool();
std::mutex &pool_lock = pool.lock;
std::condition_variable &job_ready = pool.job_ready;
std::condition_variable &buffer_free = pool.buffer_free;
std::condition_variable &job_done = pool.job_done;
std::deque<Job> &jobs = pool.jobs;
std::vector<char *> &free_buffers = pool.free_buffers;
Py_ssize_t &allocated = pool.allocated;
bool &trimming = pool.trimming;
std::atomic<bool> enabled{false};
bool started = false;
int pool_threads = 0;
Py_ssize_t pool_buffers = 0;

std::atomic<uint64_t> acquire_waits{0};
std::atomic<uint64_t> acquire_wait_nanos{0};

/* Put a buffer back, or hand it back to be freed while trimming. Requires pool_lock. */
char *return_buffer_locked(char *buffer) {
    if (trimming) {
        allocated--;
        return buffer;
    }
    free_buffers.push_back(buffer);
    return nullptr;
}

void writer_thread() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(pool_lock);
            job_ready.wait(guard, [] { return !jobs.empty(); });
            job = jobs.front();
            jobs.pop_front();
        }

        bool was_closed = false;
        unsigned long error_code = 0;
        filewriter_write_raw(job.writer, job.buffer, job.length, job.offset, &was_closed, &error_code);
        if (was_closed) {
            job.writer->async_closed.store(true, std::memory_order_relaxed);
        } else if (error_code) {
            unsigned long none = 0;
            job.writer->async_error.compare_exchange_strong(none, error_code, std::memory_order_relaxed);
        }

        char *discard;
        {
            std::lock_guard<std::mutex> guard(pool_lock);
            discard = return_buffer_locked(job.buffer);
            job.writer->pending.fetch_sub(1, std::memory_order_release);
        }
        buffer_unmap(discard);
        buffer_free.notify_one();
        job_done.notify_all();
    }
}

} // namespace

bool writebehind_enabled() {
    return enabled.load(std::memory_order_relaxed);
}

Py_ssize_t writebehind_buffer_size() {
    return YENC_STAGING_SIZE;
}

char *writebehind_acquire(bool wait) {
    std::unique_lock<std::mutex> guard(pool_lock);
    trimming = false;
    if (free_buffers.empty() && allocated < pool_buffers) {
        allocated++;
        guard.unlock();
        char *buffer = buffer_map();
        if (!buffer) {
            guard.lock();
            allocated--;
        }
        return buffer;
    }
    if (free_buffers.empty()) {
        if (!wait) return nullptr;
        std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
        buffer_free.wait(guard, [] { return !free_buffers.empty(); });
        acquire_waits.fetch_add(1, std::memory_order_relaxed);
        acquire_wait_nanos.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at)
                .count(),
            std::memory_order_relaxed);
    }
    char *buffer = free_buffers.back();
    free_buffers.pop_back();
    return buffer;
}

void writebehind_release(char *buffer) {
    char *discard;
    {
        std::lock_guard<std::mutex> guard(pool_lock);
        discard = return_buffer_locked(buffer);
    }
    buffer_unmap(discard);
    buffer_free.notify_one();
}

PyObject *writebehind_trim(PyObject *Py_UNUSED(module), PyObject *Py_UNUSED(ignored)) {
    std::vector<char *> discard;
    {
        std::lock_guard<std::mutex> guard(pool_lock);
        trimming = true;
        discard.swap(free_buffers);
        allocated -= (Py_ssize_t)discard.size();
    }
    for (char *buffer : discard) buffer_unmap(buffer);
    Py_RETURN_NONE;
}

void writebehind_submit(FileWriter *writer, char *buffer, Py_ssize_t length, long long offset) {
    writer->pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(pool_lock);
        jobs.push_back(Job{writer, buffer, length, offset});
    }
    job_ready.notify_one();
}

void writebehind_drain(FileWriter *writer) {
    if (writer->pending.load(std::memory_order_acquire) == 0) return;
    std::unique_lock<std::mutex> guard(pool_lock);
    job_done.wait(guard, [writer] { return writer->pending.load(std::memory_order_acquire) == 0; });
}

PyObject *writebehind_configure(PyObject *Py_UNUSED(module), PyObject *args, PyObject *kwargs) {
    static const char *keywords[] = {"threads", "buffers", NULL};
    int threads = 0;
    Py_ssize_t buffers = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|n:set_write_behind", const_cast<char **>(keywords), &threads,
                                     &buffers))
        return NULL;

    if (threads < 0 || buffers < 0) {
        PyErr_SetString(PyExc_ValueError, "threads and buffers must not be negative");
        return NULL;
    }

    if (threads == 0) {
        enabled.store(false, std::memory_order_relaxed);
        Py_RETURN_NONE;
    }

    if (buffers == 0) {
        PyErr_SetString(PyExc_ValueError, "buffers must be given when enabling write-behind");
        return NULL;
    }

    if (started) {
        if (threads != pool_threads || buffers != pool_buffers) {
            PyErr_Format(PyExc_RuntimeError, "write-behind already started with %d threads and %zd buffers",
                         pool_threads, pool_buffers);
            return NULL;
        }
        enabled.store(true, std::memory_order_relaxed);
        Py_RETURN_NONE;
    }

    {
        std::lock_guard<std::mutex> guard(pool_lock);
        pool_buffers = buffers;
    }
    for (int i = 0; i < threads; i++) {
        std::thread(writer_thread).detach();
    }
    started = true;
    pool_threads = threads;
    enabled.store(true, std::memory_order_relaxed);
    Py_RETURN_NONE;
}

PyObject *writebehind_stats(PyObject *Py_UNUSED(module), PyObject *args, PyObject *kwargs) {
    static const char *keywords[] = {"reset", NULL};
    int reset = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|p:write_behind_stats", const_cast<char **>(keywords), &reset))
        return NULL;

    Py_ssize_t allocated_count, free_count, queued;
    {
        std::lock_guard<std::mutex> guard(pool_lock);
        allocated_count = allocated;
        free_count = (Py_ssize_t)free_buffers.size();
        queued = (Py_ssize_t)jobs.size();
    }

    unsigned long long waits, wait_nanos;
    if (reset) {
        waits = acquire_waits.exchange(0, std::memory_order_relaxed);
        wait_nanos = acquire_wait_nanos.exchange(0, std::memory_order_relaxed);
    } else {
        waits = acquire_waits.load(std::memory_order_relaxed);
        wait_nanos = acquire_wait_nanos.load(std::memory_order_relaxed);
    }

    return Py_BuildValue("{s:O,s:i,s:n,s:n,s:n,s:n,s:K,s:K}", "enabled", enabled.load() ? Py_True : Py_False,
                         "threads", pool_threads, "buffers", pool_buffers, "allocated", allocated_count, "free",
                         free_count, "queued", queued, "waits", waits, "wait_nanos", wait_nanos);
}
