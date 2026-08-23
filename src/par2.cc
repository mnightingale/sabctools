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

#include "par2.h"

#include <par2/libpar2.h>

#include <map>
#include <ostream>
#include <set>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

/*
 * Drives the vendored par2cmdline as a library so that callers get typed results
 * instead of the repair tool's prose.
 *
 * Everything here goes through par2/libpar2.h and nothing else: Par2Verifier is a
 * pimpl and Par2Observer is the callback interface, so none of par2's internal
 * headers are included and none of its members are read. That is what makes a
 * re-vendor a rebuild rather than a rewrite.
 *
 * The sequence is load() -> set_known_blocks() -> verify() -> repair(), and a
 * verifier stays usable afterwards: load_more() adds recovery blocks that arrived
 * late and reassesses without reading the data files again.
 */

static PyObject* Par2Error = NULL;

static const char* const STAGE_LOADING = "loading";
static const char* const STAGE_VERIFYING = "verifying";
static const char* const STAGE_REPAIRING = "repairing";
static const char* const STAGE_VERIFYING_REPAIR = "verifying_repair";

class NullBuffer : public std::streambuf {
public:
    int overflow(int c) override { return c; }
};

class NullStream : public std::ostream {
public:
    NullStream() : std::ostream(&buffer) {}

private:
    NullBuffer buffer;
};

struct Par2RepairerObject;

/* Relays par2's progress and per-file results to the Python callbacks. */
class SabObserver final : public Par2::Par2Observer {
public:
    SabObserver() : owner(NULL) {}

    void SetOwner(Par2RepairerObject* o) { owner = o; }

    void OnFile(const std::string& filename) override;
    void OnProgress(Par2::u32 permille) override;
    void OnFileDone(const std::string& filename, Par2::u32 found, Par2::u32 needed) override;
    void OnRepairStart(void) override;

private:
    Par2RepairerObject* owner;
};

typedef struct Par2RepairerObject {
    PyObject_HEAD
    Par2::Par2Verifier* verifier;
    SabObserver* observer;
    NullStream* out;
    NullStream* err;

    PyObject* progress_callback;
    PyObject* file_done_callback;

    /* Verify takes these, so they are held from construction until it runs. */
    std::string* parfile;
    std::vector<std::string>* extrafiles;
    bool skip_data;
    unsigned long long skip_leaway;

    bool skip_repaired_verification;
    /* The names set_known_blocks() last vouched for, so a second call can retract
       what the first said about a file the new mapping does not mention. */
    std::set<std::string>* known;

    bool loaded;
    bool verified;
    bool cancelled;

    const char* stage;
    /* Last value passed to the callback, so the 0..1000 stream from par2 can be
       thinned before it reaches Python. */
    int last_progress;
} Par2RepairerObject;

/*
 * Invoke the user's progress callback. Called from par2's worker threads as well
 * as the calling thread, so the GIL has to be taken explicitly.
 *
 * par2 holds a lock across some of these calls, so the callback must stay short
 * and must not re-enter the verifier. An exception raised by the callback is
 * printed rather than propagated: there is no way to unwind through par2's C++
 * frames, and swallowing it silently would be worse.
 */
static void call_progress(Par2RepairerObject* self, const char* stage, const char* filename,
                          int progress) {
    if (!self || !self->progress_callback)
        return;

    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* result = PyObject_CallFunction(self->progress_callback, "ssi", stage,
                                             filename ? filename : "", progress);
    if (result == NULL) {
        PyErr_WriteUnraisable(self->progress_callback);
    } else {
        Py_DECREF(result);
    }

    PyGILState_Release(gstate);
}

void SabObserver::OnFile(const std::string& filename) {
    if (!owner)
        return;

    /* The rebuild reports only progress, so the first file after it means par2 has
       moved on to reading back what it wrote. */
    if (owner->stage == STAGE_REPAIRING)
        owner->stage = STAGE_VERIFYING_REPAIR;

    owner->last_progress = -1;
    call_progress(owner, owner->stage, filename.c_str(), 0);
}

void SabObserver::OnProgress(Par2::u32 permille) {
    if (!owner)
        return;

    /* par2 reports tenths of a percent and does so very often. Acquiring the GIL
       for each one would dominate the runtime, so only report whole percents. */
    int percent = (int)(permille / 10);
    if (percent == owner->last_progress)
        return;
    owner->last_progress = percent;

    call_progress(owner, owner->stage, NULL, percent);
}

/*
 * Fires once per file par2 finishes with, saying how many of that file's blocks it
 * could use. This is the structured form of par2's "found N of M data blocks from"
 * line - the only thing that identifies which files on disk actually contributed
 * data, which is how a caller learns that a set of joinable .001/.002 parts was
 * consumed. Both counts are zero for a par2 file, which has no blocks of its own.
 */
void SabObserver::OnFileDone(const std::string& filename, Par2::u32 found, Par2::u32 needed) {
    if (!owner || !owner->file_done_callback)
        return;

    PyGILState_STATE gstate = PyGILState_Ensure();
    PyObject* result = PyObject_CallFunction(owner->file_done_callback, "sII", filename.c_str(),
                                             (unsigned int)found, (unsigned int)needed);
    if (result == NULL) {
        PyErr_WriteUnraisable(owner->file_done_callback);
    } else {
        Py_DECREF(result);
    }
    PyGILState_Release(gstate);
}

void SabObserver::OnRepairStart(void) {
    if (!owner)
        return;
    owner->stage = STAGE_REPAIRING;
    owner->last_progress = -1;
}

/* ------------------------------------------------------------------------- */

/*
 * Run a libpar2 call with the GIL released, turning a C++ exception into Par2Error.
 * Returns false with a Python exception set.
 */
template <typename Call>
static bool run_step(Call call, Par2::Result* result) {
    bool threw = false;
    std::string message;

    Py_BEGIN_ALLOW_THREADS
    try {
        *result = call();
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    } catch (...) {
        threw = true;
        message = "unknown error in par2";
    }
    Py_END_ALLOW_THREADS

    if (threw) {
        PyErr_SetString(Par2Error, message.c_str());
        return false;
    }
    return true;
}

static PyObject* Par2Repairer_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    (void)args;
    (void)kwds;
    Par2RepairerObject* self = (Par2RepairerObject*)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    self->verifier = NULL;
    self->observer = NULL;
    self->out = NULL;
    self->err = NULL;
    self->progress_callback = NULL;
    self->file_done_callback = NULL;
    self->parfile = NULL;
    self->extrafiles = NULL;
    self->skip_data = true;
    self->skip_leaway = 0;
    self->skip_repaired_verification = true;
    self->known = NULL;
    self->loaded = false;
    self->verified = false;
    self->cancelled = false;
    self->stage = STAGE_LOADING;
    self->last_progress = -1;
    return (PyObject*)self;
}

static void Par2Repairer_dealloc(Par2RepairerObject* self) {
    /* Before the observer, which the verifier holds a pointer to. */
    delete self->verifier;
    delete self->observer;
    delete self->parfile;
    delete self->extrafiles;
    delete self->known;
    delete self->out;
    delete self->err;
    Py_XDECREF(self->progress_callback);
    Py_XDECREF(self->file_done_callback);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int Par2Repairer_init(Par2RepairerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"parfile",     "extrafiles",  "basepath",
                                   "memory_limit", "threads",     "file_threads",
                                   "skip_data",    "skip_leaway",
                                   "skip_repaired_verification", NULL};

    const char* parfile = NULL;
    PyObject* extrafiles = NULL;
    const char* basepath = NULL;
    unsigned long long memory_limit = 0;
    unsigned int threads = 0;
    unsigned int file_threads = 0;
    int skip_data = 1;
    unsigned long long skip_leaway = 0;
    int skip_repaired_verification = 1;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OsKIIpKp", (char**)kwlist, &parfile,
                                     &extrafiles, &basepath, &memory_limit, &threads,
                                     &file_threads, &skip_data, &skip_leaway,
                                     &skip_repaired_verification))
        return -1;

    std::vector<std::string> extras;
    if (extrafiles && extrafiles != Py_None) {
        PyObject* seq = PySequence_Fast(extrafiles, "extrafiles must be a sequence of str");
        if (seq == NULL)
            return -1;
        Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < count; i++) {
            const char* path = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
            if (path == NULL) {
                Py_DECREF(seq);
                return -1;
            }
            extras.push_back(path);
        }
        Py_DECREF(seq);
    }

    delete self->verifier;
    delete self->observer;
    delete self->parfile;
    delete self->extrafiles;
    delete self->known;
    delete self->out;
    delete self->err;
    self->verifier = NULL;
    self->observer = NULL;
    self->parfile = NULL;
    self->extrafiles = NULL;
    self->known = NULL;
    self->out = NULL;
    self->err = NULL;

    self->parfile = new std::string(parfile);
    self->extrafiles = new std::vector<std::string>(extras);
    self->known = new std::set<std::string>();
    self->skip_data = skip_data != 0;
    self->skip_leaway = skip_leaway;
    self->skip_repaired_verification = skip_repaired_verification != 0;
    self->loaded = false;
    self->verified = false;
    self->cancelled = false;
    self->stage = STAGE_LOADING;
    self->last_progress = -1;

    self->out = new NullStream();
    self->err = new NullStream();

    /*
     * nlSilent, because both streams are discarded anyway and the observer is
     * documented as unaffected by the noise level. That was not true of the older
     * subclassing approach, where several of the Sig* hooks and two of the
     * cancellation checks sat inside noise-level guards.
     *
     * An empty basepath is taken from the first par2 file added, which is what the
     * tool does; a memory limit of zero lets par2 size itself from physical memory.
     */
    self->verifier = new Par2::Par2Verifier(*self->out, *self->err, Par2::nlSilent,
                                            basepath ? basepath : "");
    self->verifier->SetMemoryLimit((size_t)memory_limit);
    self->verifier->SetThreadCounts(threads, file_threads);
    self->verifier->SetDataSkipping(self->skip_data, (Par2::u64)self->skip_leaway);

    self->observer = new SabObserver();
    self->observer->SetOwner(self);
    self->verifier->SetObserver(self->observer);
    return 0;
}

static bool ready(Par2RepairerObject* self) {
    if (self->verifier == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Par2Repairer is not initialised");
        return false;
    }
    return true;
}

static bool ready_and_loaded(Par2RepairerObject* self, const char* what) {
    if (!ready(self))
        return false;
    if (!self->loaded) {
        PyErr_Format(PyExc_RuntimeError, "call load() before %s()", what);
        return false;
    }
    return true;
}

static PyObject* Par2Repairer_load(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!ready(self))
        return NULL;

    self->stage = STAGE_LOADING;
    self->last_progress = -1;

    Par2::Result result;
    Par2::Par2Verifier* verifier = self->verifier;
    const std::string parfile = *self->parfile;
    if (!run_step([&] { return verifier->AddPar2File(parfile); }, &result))
        return NULL;

    if (result == Par2::eSuccess)
        self->loaded = true;
    if (result == Par2::eCancelled)
        self->cancelled = true;

    return PyLong_FromLong((long)result);
}

/* Scan the source files, leaving the outcome in self. False with a Python exception
   set. */
static bool do_verify(Par2RepairerObject* self, Par2::Result* result) {
    self->stage = STAGE_VERIFYING;
    self->last_progress = -1;

    Par2::Par2Verifier* verifier = self->verifier;
    const std::vector<std::string>& extras = *self->extrafiles;
    if (!run_step([&] { return verifier->Verify(extras); }, result))
        return false;

    if (*result == Par2::eCancelled)
        self->cancelled = true;
    else
        self->verified = true;
    return true;
}

static PyObject* Par2Repairer_verify(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!ready_and_loaded(self, "verify"))
        return NULL;

    Par2::Result result;
    if (!do_verify(self, &result))
        return NULL;
    return PyLong_FromLong((long)result);
}

/*
 * Scan one file as it becomes available, rather than waiting for the whole set.
 *
 * Nothing else is read, so files which are on disk at their final size but still
 * downloading are not scanned until the caller says they are finished. Calling it
 * again for the same file discards what the earlier scan of it found, which is what
 * makes that possible.
 */
static PyObject* Par2Repairer_verify_file(Par2RepairerObject* self, PyObject* arg) {
    if (!ready(self))
        return NULL;

    const char* filename = PyUnicode_AsUTF8(arg);
    if (filename == NULL)
        return NULL;

    self->stage = STAGE_VERIFYING;
    self->last_progress = -1;

    Par2::Result result;
    Par2::Par2Verifier* verifier = self->verifier;
    const std::string path = filename;
    if (!run_step([&] { return verifier->VerifyFile(path); }, &result))
        return NULL;

    if (result == Par2::eCancelled)
        self->cancelled = true;
    else if (result != Par2::eInsufficientCriticalData)
        self->verified = true;

    return PyLong_FromLong((long)result);
}

static PyObject* Par2Repairer_repair(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!ready_and_loaded(self, "repair"))
        return NULL;

    /* Repair works on the results of the verify that preceded it, and libpar2 says
       eLogicError rather than scanning on its own. Run the pass here so a caller
       that only wants the files fixed need not ask for it. */
    if (!self->verified) {
        Par2::Result verified;
        if (!do_verify(self, &verified))
            return NULL;
        if (verified == Par2::eCancelled || verified == Par2::eRepairNotPossible)
            return PyLong_FromLong((long)verified);
    }

    self->stage = STAGE_REPAIRING;
    self->last_progress = -1;

    /*
     * Optionally skip the pass par2 makes over the files it just rebuilt. The only
     * way a repair produces wrong data is a fault in the machine rather than in the
     * maths, and par2 checksums its own GF16 computation to catch exactly that. The
     * saving is re-reading and hashing the repaired files, so it scales with how
     * much was repaired rather than with the size of the set.
     *
     * Conditional on something having been vouched for: skipping the check on the
     * way out is only defensible if the caller's own checksums were trusted on the
     * way in, so a set that got a real source scan gets its repair verified too.
     */
    const bool verifyafter = !(self->skip_repaired_verification && !self->known->empty());

    Par2::Result result;
    Par2::Par2Verifier* verifier = self->verifier;
    if (!run_step([&] { return verifier->Repair(verifyafter); }, &result))
        return NULL;

    if (result == Par2::eCancelled)
        self->cancelled = true;

    return PyLong_FromLong((long)result);
}

/*
 * Add recovery blocks from further par2 files to an already-loaded verifier.
 *
 * This is the point of keeping a verifier alive across a "not enough blocks, go and
 * fetch more" cycle: the expensive verification pass is not repeated. Reassess works
 * out whether what the last verify found can now be repaired, without reading the
 * data files again.
 */
static PyObject* Par2Repairer_load_more(Par2RepairerObject* self, PyObject* parfiles) {
    if (!ready_and_loaded(self, "load_more"))
        return NULL;

    PyObject* sequence = PySequence_Fast(parfiles, "load_more() takes a sequence of str");
    if (sequence == NULL)
        return NULL;

    std::vector<std::string> paths;
    Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    for (Py_ssize_t i = 0; i < count; i++) {
        const char* path = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(sequence, i));
        if (path == NULL) {
            Py_DECREF(sequence);
            return NULL;
        }
        paths.push_back(path);
    }
    Py_DECREF(sequence);

    self->stage = STAGE_LOADING;
    self->last_progress = -1;

    Par2::Par2Verifier* verifier = self->verifier;
    for (size_t i = 0; i < paths.size(); i++) {
        Par2::Result result;
        const std::string& path = paths[i];
        if (!run_step([&] { return verifier->AddPar2File(path); }, &result))
            return NULL;
        /* AddPar2File shrugs off a file it cannot open while probing for optional
           sibling volumes, and reports eFileIOError only when the named file is
           absent and nothing new was read. Here the caller named the file, so that
           is a mistake worth reporting rather than a silent no-op. */
        if (result == Par2::eFileIOError) {
            PyErr_Format(Par2Error, "par2 could not read: %s", path.c_str());
            return NULL;
        }
    }

    if (self->verified) {
        Par2::Result reassessed;
        if (!run_step([&] { return verifier->Reassess(); }, &reassessed))
            return NULL;
    }

    Par2::Par2SetInfo info;
    if (!self->verifier->GetSetInfo(&info))
        return PyLong_FromUnsignedLong(0);
    return PyLong_FromUnsignedLong(info.recoveryblocks);
}

/*
 * Tell the verifier which blocks the caller already knows to be intact.
 *
 * Takes {filename: sequence of per-block truth values}, where filename is the name
 * as recorded in the par2 set and the sequence runs from block 0. Listed files are
 * not read or hashed during verify(); everything else is scanned normally, so a
 * partial map is fine and an empty one restores the default behaviour.
 *
 * Trust is the caller's to give: a block marked good here is taken at its word.
 * Call after load(), which is when the file names and block_size become known, and
 * before verify().
 */
static PyObject* Par2Repairer_set_known_blocks(Par2RepairerObject* self, PyObject* mapping) {
    if (!ready_and_loaded(self, "set_known_blocks"))
        return NULL;
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "set_known_blocks() takes a dict of {filename: blocks}");
        return NULL;
    }

    std::map<std::string, std::vector<char> > vouched;

    PyObject *key, *value;
    Py_ssize_t position = 0;
    while (PyDict_Next(mapping, &position, &key, &value)) {
        const char* filename = PyUnicode_AsUTF8(key);
        if (filename == NULL)
            return NULL;

        PyObject* sequence = PySequence_Fast(value, "block values must be a sequence");
        if (sequence == NULL)
            return NULL;

        Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
        std::vector<char> blocks;
        blocks.reserve((size_t)count);
        for (Py_ssize_t i = 0; i < count; i++) {
            int good = PyObject_IsTrue(PySequence_Fast_GET_ITEM(sequence, i));
            if (good < 0) {
                Py_DECREF(sequence);
                return NULL;
            }
            blocks.push_back(good ? 1 : 0);
        }
        Py_DECREF(sequence);

        vouched[filename] = blocks;
    }

    /* An empty vector is how libpar2 forgets what was said about a file, so retract
       anything the previous call named and this one does not. */
    for (std::set<std::string>::const_iterator it = self->known->begin();
         it != self->known->end(); ++it) {
        if (vouched.find(*it) == vouched.end())
            self->verifier->SetKnownBlocks(*it, std::vector<char>());
    }

    self->known->clear();
    for (std::map<std::string, std::vector<char> >::const_iterator it = vouched.begin();
         it != vouched.end(); ++it) {
        self->verifier->SetKnownBlocks(it->first, it->second);
        if (!it->second.empty())
            self->known->insert(it->first);
    }

    Py_RETURN_NONE;
}

static PyObject* Par2Repairer_cancel(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    if (self->verifier)
        self->verifier->Cancel();
    self->cancelled = true;
    Py_RETURN_NONE;
}

static PyMethodDef Par2Repairer_methods[] = {
    {"load", (PyCFunction)Par2Repairer_load, METH_NOARGS,
     "load() -> Par2Result\n\nRead the par2 packets and work out the file set. The volume\n"
     "files beside the named one are read too, so naming a set whose index file is\n"
     "missing still describes it."},
    {"load_more", (PyCFunction)Par2Repairer_load_more, METH_O,
     "load_more(parfiles) -> int\n\nAdd recovery blocks from further par2 files and return the\n"
     "new recovery_block_count. Does not re-scan the data files: if verify() has run, it\n"
     "only re-evaluates whether there are now enough blocks to repair."},
    {"set_known_blocks", (PyCFunction)Par2Repairer_set_known_blocks, METH_O,
     "set_known_blocks(mapping)\n\nTake {filename: per-block truth values} as already\n"
     "verified. Those files are not read or hashed during verify(). Call after load()."},
    {"verify_file", (PyCFunction)Par2Repairer_verify_file, METH_O,
     "verify_file(filename) -> Par2Result\n\nScan one file as it becomes available,\n"
     "reading nothing else. Call it again for the same file once it has finished\n"
     "downloading; what an earlier scan found for it is discarded first. May be called\n"
     "before load(), which returns INSUFFICIENT_CRITICAL_DATA and scans the file once\n"
     "the par2 packets arrive."},
    {"verify", (PyCFunction)Par2Repairer_verify, METH_NOARGS,
     "verify() -> Par2Result\n\nScan the source files. Requires load() first. May be called\n"
     "more than once; each call is a fresh pass."},
    {"repair", (PyCFunction)Par2Repairer_repair, METH_NOARGS,
     "repair() -> Par2Result\n\nRebuild whatever the preceding verify() found to be missing\n"
     "or damaged."},
    {"cancel", (PyCFunction)Par2Repairer_cancel, METH_NOARGS,
     "cancel()\n\nAsk an in-progress verify() or repair() to stop. Safe to call from another\n"
     "thread, including from progress_callback. The interrupted call returns\n"
     "Par2Result.CANCELLED, having removed any partly written files."},
    {NULL, NULL, 0, NULL}};

/* ------------------------------------------------------------------------- */

static bool set_info(Par2RepairerObject* self, Par2::Par2SetInfo* info) {
    return self->verifier != NULL && self->verifier->GetSetInfo(info);
}

static bool verify_result(Par2RepairerObject* self, Par2::Par2VerifyResult* result) {
    return self->verifier != NULL && self->verifier->GetVerifyResult(result);
}

static PyObject* get_missing_block_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.missingblockcount : 0);
}

static PyObject* get_available_block_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.availableblockcount : 0);
}

static PyObject* get_source_block_count(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyLong_FromUnsignedLong(set_info(self, &info) ? info.datablocks : 0);
}

/* From the set rather than the verify result, which is empty until a scan has run.
   The two agree once one has. */
static PyObject* get_recovery_block_count(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyLong_FromUnsignedLong(set_info(self, &info) ? info.recoveryblocks : 0);
}

static PyObject* get_recoverable_file_count(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyLong_FromUnsignedLong(set_info(self, &info) ? info.recoverablefilecount : 0);
}

static PyObject* get_complete_file_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.completefilecount : 0);
}

static PyObject* get_damaged_file_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.damagedfilecount : 0);
}

static PyObject* get_missing_file_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.missingfilecount : 0);
}

static PyObject* get_renamed_file_count(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    return PyLong_FromUnsignedLong(verify_result(self, &result) ? result.renamedfilecount : 0);
}

static PyObject* get_block_size(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyLong_FromUnsignedLongLong(set_info(self, &info) ? info.blocksize : 0);
}

static PyObject* get_data_size(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyLong_FromUnsignedLongLong(set_info(self, &info) ? info.datasize : 0);
}

static PyObject* get_setid(Par2RepairerObject* self, void*) {
    Par2::Par2SetInfo info;
    return PyUnicode_FromString(set_info(self, &info) ? info.setid.c_str() : "");
}

static PyObject* get_quick_verified_files(Par2RepairerObject* self, void*) {
    return PyLong_FromSize_t(self->known ? self->known->size() : 0);
}

static PyObject* get_cancelled(Par2RepairerObject* self, void*) {
    return PyBool_FromLong(self->cancelled);
}

/*
 * True when there is enough recovery data to rebuild what is missing. par2's own
 * "Repair is possible" line is derived the same way.
 */
static PyObject* get_repair_possible(Par2RepairerObject* self, void*) {
    Par2::Par2VerifyResult result;
    if (!verify_result(self, &result))
        Py_RETURN_FALSE;
    return PyBool_FromLong(result.recoveryblockcount >= result.missingblockcount);
}

/*
 * The files a verify found under a name other than the one the set records, as
 * {path_on_disk: path_it_belongs_under}. Both paths are absolute.
 *
 * Reads the same before and after repair(), and is emptied by the next verify().
 */
static PyObject* get_renames(Par2RepairerObject* self, void*) {
    if (self->verifier == NULL)
        Py_RETURN_NONE;

    PyObject* dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    std::vector<std::pair<std::string, std::string> > renamed;
    if (!self->verifier->GetRenamedFiles(&renamed))
        return dict;

    for (size_t i = 0; i < renamed.size(); i++) {
        PyObject* value = PyUnicode_FromString(renamed[i].second.c_str());
        if (value == NULL) {
            Py_DECREF(dict);
            return NULL;
        }
        if (PyDict_SetItemString(dict, renamed[i].first.c_str(), value) < 0) {
            Py_DECREF(value);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(value);
    }
    return dict;
}

/*
 * One dict per file in the set, in par2's own order:
 *   name   - the name recorded in the par2 set
 *   target - where that file belongs on this system, absolute
 *   size   - the file's size in bytes
 *   blocks - the file's block count, 0 if it cannot be recovered
 *
 * Both names are safe to use as they stand - an absolute path or one climbing out
 * with ".." is defused before either is reported - but either may still contain a
 * directory separator, because a set may describe files in subdirectories.
 */
static PyObject* get_files(Par2RepairerObject* self, void*) {
    if (self->verifier == NULL)
        Py_RETURN_NONE;

    PyObject* list = PyList_New(0);
    if (list == NULL)
        return NULL;

    std::vector<Par2::Par2FileInfo> files;
    if (!self->verifier->GetFileInfo(&files))
        return list;

    for (size_t i = 0; i < files.size(); i++) {
        PyObject* entry = Py_BuildValue("{s:s, s:s, s:K, s:I}",
                                        "name", files[i].filename.c_str(),
                                        "target", files[i].localfilename.c_str(),
                                        "size", (unsigned long long)files[i].filesize,
                                        "blocks", files[i].blockcount);
        if (entry == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        if (PyList_Append(list, entry) < 0) {
            Py_DECREF(entry);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(entry);
    }
    return list;
}

/*
 * The damaged files repair() renamed out of the way, which is what par2's own purge
 * option deletes. Files the caller supplied as extra files are never listed, even
 * where their blocks were used, because the caller may still want them.
 */
static PyObject* get_backup_files(Par2RepairerObject* self, void*) {
    if (self->verifier == NULL)
        Py_RETURN_NONE;

    std::vector<std::string> backups;
    if (!self->verifier->GetBackupFiles(&backups))
        return PyList_New(0);

    PyObject* list = PyList_New((Py_ssize_t)backups.size());
    if (list == NULL)
        return NULL;

    for (size_t i = 0; i < backups.size(); i++) {
        PyObject* name = PyUnicode_FromString(backups[i].c_str());
        if (name == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, name);
    }
    return list;
}

static PyObject* get_progress_callback(Par2RepairerObject* self, void*) {
    if (self->progress_callback == NULL)
        Py_RETURN_NONE;
    Py_INCREF(self->progress_callback);
    return self->progress_callback;
}

static int set_progress_callback(Par2RepairerObject* self, PyObject* value, void*) {
    if (value == NULL || value == Py_None) {
        Py_CLEAR(self->progress_callback);
        return 0;
    }
    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "progress_callback must be callable or None");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(self->progress_callback, value);
    return 0;
}

static PyObject* get_file_done_callback(Par2RepairerObject* self, void*) {
    if (self->file_done_callback == NULL)
        Py_RETURN_NONE;
    Py_INCREF(self->file_done_callback);
    return self->file_done_callback;
}

static int set_file_done_callback(Par2RepairerObject* self, PyObject* value, void*) {
    if (value == NULL || value == Py_None) {
        Py_CLEAR(self->file_done_callback);
        return 0;
    }
    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "file_done_callback must be callable or None");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(self->file_done_callback, value);
    return 0;
}

static PyGetSetDef Par2Repairer_getset[] = {
    {"missing_block_count", (getter)get_missing_block_count, NULL,
     "Blocks that need reconstructing.", NULL},
    {"available_block_count", (getter)get_available_block_count, NULL,
     "Undamaged source blocks found.", NULL},
    {"source_block_count", (getter)get_source_block_count, NULL, "Blocks in the complete set.",
     NULL},
    {"recovery_block_count", (getter)get_recovery_block_count, NULL,
     "Recovery blocks loaded from the par2 files.", NULL},
    {"recoverable_file_count", (getter)get_recoverable_file_count, NULL,
     "Files the set can recover.", NULL},
    {"complete_file_count", (getter)get_complete_file_count, NULL, "Files verified intact.", NULL},
    {"damaged_file_count", (getter)get_damaged_file_count, NULL, "Files present but damaged.",
     NULL},
    {"missing_file_count", (getter)get_missing_file_count, NULL, "Files not found.", NULL},
    {"renamed_file_count", (getter)get_renamed_file_count, NULL,
     "Files found under a different name.", NULL},
    {"block_size", (getter)get_block_size, NULL, "Block size of the set, in bytes.", NULL},
    {"data_size", (getter)get_data_size, NULL, "Total size of the recoverable files, in bytes.",
     NULL},
    {"setid", (getter)get_setid, NULL, "The par2 set id.", NULL},
    {"repair_possible", (getter)get_repair_possible, NULL,
     "Whether enough recovery blocks are available to repair.", NULL},
    {"cancelled", (getter)get_cancelled, NULL, "Whether cancel() stopped an operation.", NULL},
    {"quick_verified_files", (getter)get_quick_verified_files, NULL,
     "How many files verify() will take from set_known_blocks() instead of reading.", NULL},
    {"renames", (getter)get_renames, NULL,
     "{path_on_disk: path_it_belongs_under} for files par2 matched under another name.",
     NULL},
    {"files", (getter)get_files, NULL, "Per-file state as a list of dicts.", NULL},
    {"backup_files", (getter)get_backup_files, NULL,
     "Damaged files repair() renamed out of the way.", NULL},
    {"progress_callback", (getter)get_progress_callback, (setter)set_progress_callback,
     "Callable invoked as (stage, filename, percent), or None.", NULL},
    {"file_done_callback", (getter)get_file_done_callback, (setter)set_file_done_callback,
     "Callable invoked as (filename, blocks_found, blocks_total) once per file,\n"
     "or None. blocks_found > 0 means that file contributed data to the repair;\n"
     "both counts are 0 for a par2 file.", NULL},
    {NULL, NULL, NULL, NULL, NULL}};

static PyTypeObject Par2RepairerType = {
    PyVarObject_HEAD_INIT(NULL, 0) "sabctools.Par2Repairer",  // tp_name
    sizeof(Par2RepairerObject),                               // tp_basicsize
    0,                                                        // tp_itemsize
    (destructor)Par2Repairer_dealloc,                         // tp_dealloc
    0,                                                        // tp_vectorcall_offset
    0,                                                        // tp_getattr
    0,                                                        // tp_setattr
    0,                                                        // tp_as_async
    0,                                                        // tp_repr
    0,                                                        // tp_as_number
    0,                                                        // tp_as_sequence
    0,                                                        // tp_as_mapping
    0,                                                        // tp_hash
    0,                                                        // tp_call
    0,                                                        // tp_str
    0,                                                        // tp_getattro
    0,                                                        // tp_setattro
    0,                                                        // tp_as_buffer
    Py_TPFLAGS_DEFAULT,                                       // tp_flags
    "Par2Repairer(parfile, extrafiles=(), basepath='', ...)",  // tp_doc
    0,                                                        // tp_traverse
    0,                                                        // tp_clear
    0,                                                        // tp_richcompare
    0,                                                        // tp_weaklistoffset
    0,                                                        // tp_iter
    0,                                                        // tp_iternext
    Par2Repairer_methods,                                     // tp_methods
    0,                                                        // tp_members
    Par2Repairer_getset,                                      // tp_getset
    0,                                                        // tp_base
    0,                                                        // tp_dict
    0,                                                        // tp_descr_get
    0,                                                        // tp_descr_set
    0,                                                        // tp_dictoffset
    (initproc)Par2Repairer_init,                              // tp_init
    PyType_GenericAlloc,                                      // tp_alloc
    Par2Repairer_new,                                          // tp_new
};

/* ------------------------------------------------------------------------- */

int par2_init(PyObject* m) {
    if (PyType_Ready(&Par2RepairerType) < 0)
        return 0;

    /* Mirrors libpar2.h's Result enum. */
    PyObject* members = Py_BuildValue(
        "{s:i, s:i, s:i, s:i, s:i, s:i, s:i, s:i, s:i, s:i}",
        "SUCCESS", (int)Par2::eSuccess,
        "REPAIR_POSSIBLE", (int)Par2::eRepairPossible,
        "REPAIR_NOT_POSSIBLE", (int)Par2::eRepairNotPossible,
        "INVALID_COMMAND_LINE_ARGUMENTS", (int)Par2::eInvalidCommandLineArguments,
        "INSUFFICIENT_CRITICAL_DATA", (int)Par2::eInsufficientCriticalData,
        "REPAIR_FAILED", (int)Par2::eRepairFailed,
        "FILE_IO_ERROR", (int)Par2::eFileIOError,
        "LOGIC_ERROR", (int)Par2::eLogicError,
        "MEMORY_ERROR", (int)Par2::eMemoryError,
        "CANCELLED", (int)Par2::eCancelled);
    if (members == NULL)
        return 0;

    PyObject* enum_module = PyImport_ImportModule("enum");
    if (enum_module == NULL) {
        Py_DECREF(members);
        return 0;
    }

    PyObject* result_enum = PyObject_CallMethod(enum_module, "IntEnum", "(sO)", "Par2Result", members);
    Py_DECREF(enum_module);
    Py_DECREF(members);
    if (result_enum == NULL)
        return 0;

    if (PyModule_AddObject(m, "Par2Result", result_enum) < 0) {
        Py_DECREF(result_enum);
        return 0;
    }

    Par2Error = PyErr_NewException("sabctools.Par2Error", NULL, NULL);
    if (Par2Error == NULL)
        return 0;
    Py_INCREF(Par2Error);
    if (PyModule_AddObject(m, "Par2Error", Par2Error) < 0) {
        Py_DECREF(Par2Error);
        return 0;
    }

    if (PyModule_AddType(m, &Par2RepairerType) < 0)
        return 0;

    return 1;
}
