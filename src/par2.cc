/*
 * Copyright 2007-2023 The SABnzbd-Team (sabnzbd.org)
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

/*
 * Must precede every include. par2/commandline.h pulls in <windows.h> without first
 * defining NOMINMAX - unlike osinfo/cpuid.h, gf16/threadqueue.h and gf16/x86_jit.h,
 * which all guard it - and the resulting min/max macros then break the std::min in
 * filechecksummer.h. par2's own sources avoid this by including libpar2internal.h
 * first, which sets both of these; this file is the one translation unit that does
 * not, so it has to set them itself.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "par2.h"

#include <par2/libpar2.h>
#include <par2/commandline.h>
#include <par2/par2repairer.h>
#include <par2/descriptionpacket.h>
#include <par2/verificationpacket.h>
#include <par2/diskfile.h>
#include <par2/datablock.h>

#include <map>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>
#include <utility>

/*
 * Drives the vendored par2cmdline-turbo as a library so that callers get typed
 * results instead of the repair tool's prose. Everything a caller needs is read
 * straight off Par2Repairer's members, which is why SabRepairer subclasses it.
 *
 * The sequence is CommandLine::Parse -> PreProcess -> Process. Calling Process on
 * its own segfaults on this fork: the packet loading and the basepath assignment
 * both live in PreProcess, so mainpacket stays NULL. That also rules out the stock
 * Par2::par2repair() entry point, which never calls PreProcess.
 */

static PyObject* Par2Error = NULL;

/* Progress callback stages. */
static const char* const STAGE_LOADING = "loading";
static const char* const STAGE_VERIFYING = "verifying";
static const char* const STAGE_REPAIRING = "repairing";
static const char* const STAGE_VERIFYING_REPAIR = "verifying_repair";

/* par2cmdline writes progress and prose to two ostreams. We want neither. */
class NullBuffer : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

class NullStream : public std::ostream {
public:
    NullStream() : std::ostream(&buffer) {}

private:
    NullBuffer buffer;
};

struct Par2RepairerObject;

class SabRepairer final : public Par2::Par2Repairer {
public:
    /*
     * nlNormal, not nlSilent, even though both output streams are discarded. Several
     * of the Sig* hooks sit inside `if (noiselevel > nlSilent)` / `> nlQuiet` blocks,
     * and so do two of the `if (cancelled) break;` checks - at nlSilent we would lose
     * most progress reporting and the ability to interrupt loading or scanning.
     * nlNormal is also what the CLI runs at, so it is the best-tested path.
     */
    SabRepairer(std::ostream& out, std::ostream& err)
        : Par2::Par2Repairer(out, err, Par2::nlNormal), owner(NULL), stage(STAGE_LOADING) {}

    void SetOwner(Par2RepairerObject* o) { owner = o; }

    /* Reading protected state is the whole point of subclassing; expose what the
       Python layer needs and nothing more. */
    Par2::u32 MissingBlockCount() const { return missingblockcount; }
    Par2::u32 AvailableBlockCount() const { return availableblockcount; }
    Par2::u32 SourceBlockCount() const { return sourceblockcount; }
    Par2::u32 CompleteFileCount() const { return completefilecount; }
    Par2::u32 RenamedFileCount() const { return renamedfilecount; }
    Par2::u32 DamagedFileCount() const { return damagedfilecount; }
    Par2::u32 MissingFileCount() const { return missingfilecount; }
    Par2::u64 BlockSize() const { return blocksize; }
    std::string SetId() const { return setid.print(); }
    size_t RecoveryPacketCount() const { return recoverypacketmap.size(); }
    Par2::u32 RecoverableFileCount() const {
        return mainpacket ? mainpacket->RecoverableFileCount() : 0;
    }
    const std::vector<Par2::Par2RepairerSourceFile*>& SourceFiles() const { return sourcefiles; }

    void Cancel() { cancelled = true; }
    bool WasCancelled() const { return cancelled; }

    /*
     * Pull in another par2 file's packets after load(). Safe to call repeatedly:
     * LoadPacketsFromFile skips anything already in the diskFileMap, and packets from a
     * different set are rejected on setid. Only recoverypacketmap grows in practice,
     * which is what makes "fetch more blocks and retry" possible without re-verifying:
     * Process() re-runs CheckVerificationResults() on every call, but skips the
     * verification pass once alreadyloaded is set.
     */
    bool LoadMore(const std::string& filename) { return LoadPacketsFromFile(filename); }

    void SetStage(const char* s) { stage = s; }
    const char* Stage() const { return stage; }

    /*
     * Blocks the caller already knows to be good, keyed on the filename as it appears
     * in the par2 set. Anything listed here is taken on trust and never read from disk;
     * see ScanDataFile below.
     */
    void SetKnownBlocks(const std::string& filename, std::vector<bool> blocks) {
        knownBlocks[filename] = std::move(blocks);
    }
    void ClearKnownBlocks() { knownBlocks.clear(); }
    Par2::u32 QuickVerifiedFiles() const { return quickVerifiedFiles; }

protected:
    void SigFilename(std::string filename) override;
    void SigProgress(int progress) override;
    void SigDone(std::string filename, int available, int total) override;
    void BeginRepair() override;
    bool ScanDataFile(Par2::DiskFile* diskfile,
                      std::string basepath,
                      const bool renameonly,
                      Par2::Par2RepairerSourceFile*& sourcefile,
                      Par2::MatchType& matchtype,
                      Par2::MD5Hash& hashfull,
                      Par2::MD5Hash& hash16k,
                      Par2::u32& count) override;

private:
    Par2RepairerObject* owner;
    const char* stage;
    std::map<std::string, std::vector<bool>> knownBlocks;
    Par2::u32 quickVerifiedFiles = 0;
};

typedef struct Par2RepairerObject {
    PyObject_HEAD
    SabRepairer* repairer;
    NullStream* out;
    NullStream* err;
    Par2::CommandLine* cmdline;
    PyObject* progress_callback;
    PyObject* file_done_callback;
    bool loaded;
    /* Last value passed to the callback, so the 0..1000 stream from par2 can be
       thinned before it reaches Python. */
    int last_progress;
} Par2RepairerObject;

/*
 * Invoke the user's progress callback. Called from par2's worker threads as well
 * as the calling thread, so the GIL has to be taken explicitly.
 *
 * par2 holds its output_lock across some of these calls, so the callback must stay
 * short and must not re-enter the repairer. An exception raised by the callback is
 * printed rather than propagated: there is no way to unwind through par2's C++
 * frames, and swallowing it silently would be worse.
 */
static void call_progress(Par2RepairerObject* self, const char* stage, const char* filename, int progress) {
    if (!self || !self->progress_callback)
        return;

    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* result = PyObject_CallFunction(
        self->progress_callback, "ssi", stage, filename ? filename : "", progress);
    if (result == NULL) {
        PyErr_WriteUnraisable(self->progress_callback);
    } else {
        Py_DECREF(result);
    }

    PyGILState_Release(gstate);
}

void SabRepairer::SigFilename(std::string filename) {
    /* Nothing else marks the start of the post-repair verification pass, but the
       repair itself only ever emits progress, so the first filename after
       BeginRepair() means par2 has moved on to re-checking what it wrote. */
    if (stage == STAGE_REPAIRING)
        stage = STAGE_VERIFYING_REPAIR;

    if (owner) {
        reinterpret_cast<Par2RepairerObject*>(owner)->last_progress = -1;
        call_progress(reinterpret_cast<Par2RepairerObject*>(owner), stage, filename.c_str(), 0);
    }
}

void SabRepairer::SigProgress(int progress) {
    if (!owner)
        return;

    /* During loading, par2 passes a raw byte offset rather than the per-mille value
       it uses everywhere else - it even computes the fraction for its own printout
       and then signals the byte count instead. There is no file size here to
       normalise against, so no percentage is reported for this stage; the filename
       callbacks still are, and loading is brief next to verify and repair. */
    if (stage == STAGE_LOADING)
        return;

    Par2RepairerObject* self = reinterpret_cast<Par2RepairerObject*>(owner);

    /* par2 reports tenths of a percent and does so very often. Acquiring the GIL
       for each one would dominate the runtime, so only report whole percents. */
    int percent = progress / 10;
    if (percent == self->last_progress)
        return;
    self->last_progress = percent;

    call_progress(self, stage, NULL, percent);
}

/*
 * Fires once per file par2 finishes scanning, with how many of that file's blocks it
 * could use. This is the structured form of par2's "found N of M data blocks from" line
 * - the only thing that identifies which files on disk actually contributed data, which
 * is how a caller learns that a set of joinable .001/.002 parts was consumed.
 */
void SabRepairer::SigDone(std::string filename, int available, int total) {
    if (!owner)
        return;

    Par2RepairerObject* self = reinterpret_cast<Par2RepairerObject*>(owner);
    if (!self->file_done_callback)
        return;

    PyGILState_STATE gstate = PyGILState_Ensure();
    PyObject* result =
        PyObject_CallFunction(self->file_done_callback, "sii", filename.c_str(), available, total);
    if (result == NULL) {
        PyErr_WriteUnraisable(self->file_done_callback);
    } else {
        Py_DECREF(result);
    }
    PyGILState_Release(gstate);
}

/*
 * Substitute the caller's knowledge of which blocks are good for reading and hashing
 * the file.
 *
 * The caller already checksummed every article as it arrived, so for a file it has
 * ranges for there is nothing on disk worth re-reading: point each good block's
 * DataBlock at its offset and report the match. DataBlock::SetLocation is exactly what
 * par2's own scanner calls once a block's CRC and MD5 check out, so from here on the
 * repair proceeds identically.
 *
 * Only ever applied to a source file being checked in place. ScanDataFile is also used
 * to test unrelated files against a guessed sourcefile while hunting for renamed
 * content, and the caller's block map says nothing about those - hence the target
 * checks below. Anything not covered falls through to the real scan.
 */
bool SabRepairer::ScanDataFile(Par2::DiskFile* diskfile,
                               std::string basepath,
                               const bool renameonly,
                               Par2::Par2RepairerSourceFile*& sourcefile,
                               Par2::MatchType& matchtype,
                               Par2::MD5Hash& hashfull,
                               Par2::MD5Hash& hash16k,
                               Par2::u32& count) {
    /* Only during the source scan. par2 verifies again after repairing, and the block
       map describes what was on disk *before* the repair - reusing it there would
       report the freshly rebuilt file as still damaged. Those files are worth reading
       properly anyway, and by then the expensive pass has already been skipped. */
    if (stage == STAGE_VERIFYING && !knownBlocks.empty() && diskfile && sourcefile && !renameonly &&
        sourcefile->GetTargetExists() && sourcefile->GetDescriptionPacket() &&
        diskfile->FileName() == sourcefile->TargetFileName()) {

        std::map<std::string, std::vector<bool>>::const_iterator known =
            knownBlocks.find(sourcefile->GetDescriptionPacket()->FileName());

        if (known != knownBlocks.end()) {
            const std::vector<bool>& good = known->second;
            Par2::u32 blockcount = sourcefile->BlockCount();
            std::vector<Par2::DataBlock>::iterator blocks = sourcefile->SourceBlocks();

            Par2::u32 available = 0;
            for (Par2::u32 i = 0; i < blockcount && i < good.size(); i++) {
                if (good[i]) {
                    blocks[i].SetLocation(diskfile, (Par2::u64)i * blocksize);
                    available++;
                }
            }

            count = available;
            matchtype = available == blockcount ? Par2::eFullMatch
                        : available > 0        ? Par2::ePartialMatch
                                               : Par2::eNoMatch;

            std::string name;
            Par2::DiskFile::SplitFilename(diskfile->FileName(), basepath, name);
            SigFilename(name);
            SigDone(name, available, blockcount);
            SigProgress(1000);

            quickVerifiedFiles++;
            return true;
        }
    }

    return Par2::Par2Repairer::ScanDataFile(
        diskfile, basepath, renameonly, sourcefile, matchtype, hashfull, hash16k, count);
}

void SabRepairer::BeginRepair() {
    stage = STAGE_REPAIRING;
    if (owner)
        reinterpret_cast<Par2RepairerObject*>(owner)->last_progress = -1;
}

/* ------------------------------------------------------------------------- */

static PyObject* Par2Repairer_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    (void)args;
    (void)kwds;
    Par2RepairerObject* self = (Par2RepairerObject*)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    self->repairer = NULL;
    self->out = NULL;
    self->err = NULL;
    self->cmdline = NULL;
    self->progress_callback = NULL;
    self->file_done_callback = NULL;
    self->loaded = false;
    self->last_progress = -1;
    return (PyObject*)self;
}

static void Par2Repairer_dealloc(Par2RepairerObject* self) {
    delete self->repairer;
    delete self->cmdline;
    delete self->out;
    delete self->err;
    Py_XDECREF(self->progress_callback);
    Py_XDECREF(self->file_done_callback);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int Par2Repairer_init(Par2RepairerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"parfile",    "extrafiles", "basepath",  "memory_limit",
                                   "threads",    "file_threads", "skip_data", "skip_leaway",
                                   "purge_files", "rename_only", NULL};

    const char* parfile = NULL;
    PyObject* extrafiles = NULL;
    const char* basepath = NULL;
    unsigned long long memory_limit = 0;
    unsigned int threads = 0;
    unsigned int file_threads = 0;
    int skip_data = 1;
    unsigned long long skip_leaway = 0;
    int purge_files = 0;
    int rename_only = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OsKIIpKpp", (char**)kwlist, &parfile, &extrafiles,
                                     &basepath, &memory_limit, &threads, &file_threads, &skip_data,
                                     &skip_leaway, &purge_files, &rename_only))
        return -1;

    /* Build the argv that CommandLine::Parse expects. Going through CommandLine
       rather than setting fields directly is deliberate: it is what derives the
       memory limit from physical RAM, normalises the basepath and validates the
       combination. Passing memorylimit=0 straight to Process gives a zero-sized
       transfer buffer and a segfault. */
    std::vector<std::string> argv;
    argv.push_back("par2");
    argv.push_back("r");

    if (skip_data)
        argv.push_back("-N");
    if (purge_files)
        argv.push_back("-p");
    if (rename_only)
        argv.push_back("-O");  /* -R is recursive-on-create, not rename-only */
    if (memory_limit) {
        /* par2 takes megabytes and rejects -m0, so never round a non-zero limit
           down to nothing. Pass 0 to let it size itself from physical memory. */
        unsigned long long megabytes = memory_limit / 1048576;
        argv.push_back("-m" + std::to_string(megabytes ? megabytes : 1));
    }
    if (threads)
        argv.push_back("-t" + std::to_string(threads));
    if (file_threads)
        argv.push_back("-T" + std::to_string(file_threads));
    if (skip_leaway)
        argv.push_back("-S" + std::to_string(skip_leaway));
    if (basepath && *basepath) {
        argv.push_back("-B");
        argv.push_back(basepath);
    }

    argv.push_back(parfile);

    if (extrafiles && extrafiles != Py_None) {
        PyObject* seq = PySequence_Fast(extrafiles, "extrafiles must be a sequence of str");
        if (seq == NULL)
            return -1;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            const char* path = PyUnicode_AsUTF8(item);
            if (path == NULL) {
                Py_DECREF(seq);
                return -1;
            }
            argv.push_back(path);
        }
        Py_DECREF(seq);
    }

    std::vector<const char*> cargv;
    cargv.reserve(argv.size());
    for (size_t i = 0; i < argv.size(); i++)
        cargv.push_back(argv[i].c_str());

    delete self->repairer;
    delete self->cmdline;
    delete self->out;
    delete self->err;
    self->repairer = NULL;
    self->cmdline = NULL;
    self->out = NULL;
    self->err = NULL;
    self->loaded = false;

    self->cmdline = new Par2::CommandLine();
    if (!self->cmdline->Parse((int)cargv.size(), cargv.data())) {
        delete self->cmdline;
        self->cmdline = NULL;
        PyErr_SetString(PyExc_ValueError, "par2 rejected the given options");
        return -1;
    }

    self->out = new NullStream();
    self->err = new NullStream();
    self->repairer = new SabRepairer(*self->out, *self->err);
    self->repairer->SetOwner(self);
    return 0;
}

/* Runs a Par2Repairer step with the GIL released. */
static PyObject* run_step(Par2RepairerObject* self, bool load, bool dorepair) {
    if (self->repairer == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Par2Repairer is not initialised");
        return NULL;
    }
    if (!load && !self->loaded) {
        PyErr_SetString(PyExc_RuntimeError, "call load() before verify() or repair()");
        return NULL;
    }

    Par2::Result result;
    bool threw = false;
    std::string message;

    self->last_progress = -1;

    Py_BEGIN_ALLOW_THREADS
    try {
        if (load) {
            self->repairer->SetStage(STAGE_LOADING);
            result = self->repairer->PreProcess(*self->cmdline);
        } else {
            self->repairer->SetStage(STAGE_VERIFYING);
            result = self->repairer->Process(
                self->cmdline->GetMemoryLimit(), self->cmdline->GetBasePath(),
                self->cmdline->GetNumThreads(), self->cmdline->GetFileThreads(),
                self->cmdline->GetParFilename(), self->cmdline->GetExtraFiles(), dorepair,
                self->cmdline->GetPurgeFiles(), self->cmdline->GetRenameOnly(),
                self->cmdline->GetSkipData(), self->cmdline->GetSkipLeaway());
        }
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
        result = Par2::eLogicError;
    } catch (...) {
        threw = true;
        message = "unknown error in par2";
        result = Par2::eLogicError;
    }
    Py_END_ALLOW_THREADS

    if (threw) {
        PyErr_SetString(Par2Error, message.c_str());
        return NULL;
    }

    if (load && result == Par2::eSuccess)
        self->loaded = true;

    return PyLong_FromLong((long)result);
}

static PyObject* Par2Repairer_load(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    return run_step(self, true, false);
}

static PyObject* Par2Repairer_verify(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    return run_step(self, false, false);
}

static PyObject* Par2Repairer_repair(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    return run_step(self, false, true);
}

/*
 * Add recovery blocks from further par2 files to an already-loaded repairer.
 *
 * This is the point of keeping a repairer alive across a "not enough blocks, go and
 * fetch more" cycle: the expensive verification pass is not repeated, only the new
 * packets are read, and the next repair() re-evaluates repairability against the larger
 * recoverypacketmap.
 */
static PyObject* Par2Repairer_load_more(Par2RepairerObject* self, PyObject* parfiles) {
    if (self->repairer == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Par2Repairer is not initialised");
        return NULL;
    }
    if (!self->loaded) {
        PyErr_SetString(PyExc_RuntimeError, "call load() before load_more()");
        return NULL;
    }

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
        /* LoadPacketsFromFile shrugs off a file it cannot open, because PreProcess uses
           it to probe for optional sibling volumes. Here the caller named the file, so
           a missing one is a mistake worth reporting rather than a silent no-op that
           leaves the block count unchanged. */
        if (!Par2::DiskFile::FileExists(path)) {
            PyErr_Format(Par2Error, "no such par2 file: %s", path);
            Py_DECREF(sequence);
            return NULL;
        }
        paths.push_back(path);
    }
    Py_DECREF(sequence);

    bool ok = true;
    bool threw = false;
    std::string message;

    self->repairer->SetStage(STAGE_LOADING);

    Py_BEGIN_ALLOW_THREADS
    try {
        for (size_t i = 0; i < paths.size() && ok; i++)
            ok = self->repairer->LoadMore(paths[i]);
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
        return NULL;
    }
    if (!ok) {
        PyErr_SetString(Par2Error, "par2 could not read one of the given files");
        return NULL;
    }

    return PyLong_FromSize_t(self->repairer->RecoveryPacketCount());
}

/*
 * Tell the repairer which blocks the caller already knows to be intact.
 *
 * Takes {filename: sequence of per-block truth values}, where filename is the name as
 * recorded in the par2 set and the sequence runs from block 0. Listed files are not
 * read or hashed during verify(); everything else is scanned normally, so a partial
 * map is fine and an empty one restores the default behaviour.
 *
 * Trust is the caller's to give: a block marked good here is taken at its word. Call
 * after load(), which is when block_size becomes known, and before verify().
 */
static PyObject* Par2Repairer_set_known_blocks(Par2RepairerObject* self, PyObject* mapping) {
    if (self->repairer == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Par2Repairer is not initialised");
        return NULL;
    }
    if (!self->loaded) {
        PyErr_SetString(PyExc_RuntimeError, "call load() before set_known_blocks()");
        return NULL;
    }
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "set_known_blocks() takes a dict of {filename: blocks}");
        return NULL;
    }

    self->repairer->ClearKnownBlocks();

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
        std::vector<bool> blocks;
        blocks.reserve((size_t)count);
        for (Py_ssize_t i = 0; i < count; i++) {
            int good = PyObject_IsTrue(PySequence_Fast_GET_ITEM(sequence, i));
            if (good < 0) {
                Py_DECREF(sequence);
                return NULL;
            }
            blocks.push_back(good != 0);
        }
        Py_DECREF(sequence);

        self->repairer->SetKnownBlocks(filename, std::move(blocks));
    }

    Py_RETURN_NONE;
}

static PyObject* Par2Repairer_cancel(Par2RepairerObject* self, PyObject* Py_UNUSED(ignored)) {
    if (self->repairer)
        self->repairer->Cancel();
    Py_RETURN_NONE;
}

static PyMethodDef Par2Repairer_methods[] = {
    {"load", (PyCFunction)Par2Repairer_load, METH_NOARGS,
     "load() -> Par2Result\n\nRead the par2 packets and work out the file set."},
    {"load_more", (PyCFunction)Par2Repairer_load_more, METH_O,
     "load_more(parfiles) -> int\n\nAdd recovery blocks from further par2 files and return the\n"
     "new recovery_block_count. Does not re-verify: a following repair() reuses the\n"
     "existing verification and only re-checks whether it now has enough blocks."},
    {"set_known_blocks", (PyCFunction)Par2Repairer_set_known_blocks, METH_O,
     "set_known_blocks(mapping)\n\nTake {filename: per-block truth values} as already\n"
     "verified. Those files are not read or hashed during verify(). Call after load()."},
    {"verify", (PyCFunction)Par2Repairer_verify, METH_NOARGS,
     "verify() -> Par2Result\n\nScan the source files. Requires load() first."},
    {"repair", (PyCFunction)Par2Repairer_repair, METH_NOARGS,
     "repair() -> Par2Result\n\nRepair the file set, reusing an earlier verify() if there was one."},
    {"cancel", (PyCFunction)Par2Repairer_cancel, METH_NOARGS,
     "cancel()\n\nAsk an in-progress verify() or repair() to stop. Safe to call from another\n"
     "thread, including from progress_callback. The interrupted call returns\n"
     "Par2Result.FILE_IO_ERROR, so check the `cancelled` attribute to tell a\n"
     "cancellation apart from a genuine I/O failure."},
    {NULL, NULL, 0, NULL}};

/* ------------------------------------------------------------------------- */

#define REPAIRER_OR_NONE(self)                    \
    if ((self)->repairer == NULL) {               \
        Py_RETURN_NONE;                           \
    }

static PyObject* get_missing_block_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->MissingBlockCount());
}

static PyObject* get_available_block_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->AvailableBlockCount());
}

static PyObject* get_source_block_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->SourceBlockCount());
}

static PyObject* get_recovery_block_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromSize_t(self->repairer->RecoveryPacketCount());
}

static PyObject* get_recoverable_file_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->RecoverableFileCount());
}

static PyObject* get_complete_file_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->CompleteFileCount());
}

static PyObject* get_damaged_file_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->DamagedFileCount());
}

static PyObject* get_missing_file_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->MissingFileCount());
}

static PyObject* get_renamed_file_count(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->RenamedFileCount());
}

static PyObject* get_block_size(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLongLong(self->repairer->BlockSize());
}

static PyObject* get_setid(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyUnicode_FromString(self->repairer->SetId().c_str());
}

static PyObject* get_quick_verified_files(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)
    return PyLong_FromUnsignedLong(self->repairer->QuickVerifiedFiles());
}

static PyObject* get_cancelled(Par2RepairerObject* self, void*) {
    if (self->repairer == NULL)
        Py_RETURN_FALSE;
    return PyBool_FromLong(self->repairer->WasCancelled());
}

/*
 * True when there is enough recovery data to rebuild what is missing. par2's own
 * "Repair is possible" line is derived the same way.
 */
static PyObject* get_repair_possible(Par2RepairerObject* self, void*) {
    if (self->repairer == NULL)
        Py_RETURN_FALSE;
    return PyBool_FromLong(self->repairer->MissingBlockCount() <=
                           self->repairer->RecoveryPacketCount());
}

/*
 * The renames par2 will apply, as {path_on_disk: path_it_will_get}.
 *
 * A misnamed file is one par2 matched to a source file by content while nothing sits
 * at the expected path: GetTargetFile() is null but GetCompleteFile() is not. That is
 * the same condition RenameTargetFiles() acts on. Note TargetFileName() is always the
 * *wanted* path (basepath + the name in the description packet), never the name found
 * on disk, so it cannot be used to detect a rename on its own.
 *
 * Only meaningful between verify() and repair(); repair() performs the renames and
 * clears the state.
 */
static PyObject* get_renames(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)

    PyObject* dict = PyDict_New();
    if (dict == NULL)
        return NULL;

    const std::vector<Par2::Par2RepairerSourceFile*>& files = self->repairer->SourceFiles();
    for (size_t i = 0; i < files.size(); i++) {
        Par2::Par2RepairerSourceFile* sf = files[i];
        if (sf == NULL)
            continue;
        if (sf->GetTargetFile() != NULL || sf->GetCompleteFile() == NULL)
            continue;

        std::string found = sf->GetCompleteFile()->FileName();
        std::string wanted = sf->TargetFileName();
        if (found.empty() || wanted.empty() || found == wanted)
            continue;

        PyObject* value = PyUnicode_FromString(wanted.c_str());
        if (value == NULL) {
            Py_DECREF(dict);
            return NULL;
        }
        if (PyDict_SetItemString(dict, found.c_str(), value) < 0) {
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
 *   name     - the name recorded in the par2 set
 *   target   - the path it should occupy
 *   found    - where a complete copy actually is, "" if there isn't one
 *   exists   - whether something occupies the target path
 *   complete - whether a verified-intact copy was found, under any name
 *   blocks   - the file's block count
 */
static PyObject* get_files(Par2RepairerObject* self, void*) {
    REPAIRER_OR_NONE(self)

    PyObject* list = PyList_New(0);
    if (list == NULL)
        return NULL;

    const std::vector<Par2::Par2RepairerSourceFile*>& files = self->repairer->SourceFiles();
    for (size_t i = 0; i < files.size(); i++) {
        Par2::Par2RepairerSourceFile* sf = files[i];
        if (sf == NULL || sf->GetDescriptionPacket() == NULL)
            continue;

        Par2::DiskFile* complete = sf->GetCompleteFile();
        PyObject* entry = Py_BuildValue(
            "{s:s, s:s, s:s, s:O, s:O, s:I}",
            "name", sf->GetDescriptionPacket()->FileName().c_str(),
            "target", sf->TargetFileName().c_str(),
            "found", complete ? complete->FileName().c_str() : "",
            "exists", sf->GetTargetExists() ? Py_True : Py_False,
            "complete", complete ? Py_True : Py_False,
            "blocks", sf->BlockCount());
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
    {"damaged_file_count", (getter)get_damaged_file_count, NULL,
     "Files present but damaged.", NULL},
    {"missing_file_count", (getter)get_missing_file_count, NULL, "Files not found.", NULL},
    {"renamed_file_count", (getter)get_renamed_file_count, NULL,
     "Files found under a different name.", NULL},
    {"block_size", (getter)get_block_size, NULL, "Block size of the set, in bytes.", NULL},
    {"setid", (getter)get_setid, NULL, "The par2 set id.", NULL},
    {"repair_possible", (getter)get_repair_possible, NULL,
     "Whether enough recovery blocks are available to repair.", NULL},
    {"cancelled", (getter)get_cancelled, NULL, "Whether cancel() was called.", NULL},
    {"quick_verified_files", (getter)get_quick_verified_files, NULL,
     "How many files verify() took from set_known_blocks() instead of reading.", NULL},
    {"renames", (getter)get_renames, NULL,
     "{name_on_disk: name_in_set} for files par2 matched under another name.", NULL},
    {"files", (getter)get_files, NULL, "Per-file state as a list of dicts.", NULL},
    {"progress_callback", (getter)get_progress_callback, (setter)set_progress_callback,
     "Callable invoked as (stage, filename, percent), or None.", NULL},
    {"file_done_callback", (getter)get_file_done_callback, (setter)set_file_done_callback,
     "Callable invoked as (filename, blocks_found, blocks_total) once per scanned file,\n"
     "or None. blocks_found > 0 means that file contributed data to the repair.", NULL},
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
    Par2Repairer_new,                                         // tp_new
};

/* ------------------------------------------------------------------------- */

int par2_init(PyObject* m) {
    if (PyType_Ready(&Par2RepairerType) < 0)
        return 0;

    /* Mirrors libpar2.h's Result enum. */
    PyObject* members = Py_BuildValue(
        "{s:i, s:i, s:i, s:i, s:i, s:i, s:i, s:i, s:i}",
        "SUCCESS", (int)Par2::eSuccess,
        "REPAIR_POSSIBLE", (int)Par2::eRepairPossible,
        "REPAIR_NOT_POSSIBLE", (int)Par2::eRepairNotPossible,
        "INVALID_COMMAND_LINE_ARGUMENTS", (int)Par2::eInvalidCommandLineArguments,
        "INSUFFICIENT_CRITICAL_DATA", (int)Par2::eInsufficientCriticalData,
        "REPAIR_FAILED", (int)Par2::eRepairFailed,
        "FILE_IO_ERROR", (int)Par2::eFileIOError,
        "LOGIC_ERROR", (int)Par2::eLogicError,
        "MEMORY_ERROR", (int)Par2::eMemoryError);
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
