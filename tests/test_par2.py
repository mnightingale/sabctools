#!/usr/bin/python3 -OO
# Copyright 2007-2023 The SABnzbd-Team (sabnzbd.org)
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

import hashlib
import os
import shutil
import threading

import pytest

import sabctools

PAR2FILES = os.path.join(os.path.dirname(__file__), "par2files")
DATA_FILES = ("alpha.bin", "beta.bin", "gamma.bin")

# The fixture set has a 64 byte block size, 1920 source blocks and 576 recovery blocks
# (30% redundancy). Per file: alpha 960, beta 640, gamma 320. So losing gamma, or gamma
# plus some of alpha, is repairable; losing beta or alpha outright is not. Keep that
# budget in mind when changing which files a test damages.
RECOVERY_BLOCKS = 576


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


@pytest.fixture
def par2set(tmp_path):
    """A pristine copy of the fixture set, so tests can damage it freely."""
    for name in os.listdir(PAR2FILES):
        shutil.copy(os.path.join(PAR2FILES, name), tmp_path)
    return str(tmp_path)


@pytest.fixture
def digests():
    return {name: md5(os.path.join(PAR2FILES, name)) for name in DATA_FILES}


def repairer(basepath, **kwargs):
    return sabctools.Par2Repairer(os.path.join(basepath, "rec.par2"), basepath=basepath, **kwargs)


class TestPar2Load:
    def test_reads_the_set(self, par2set):
        rep = repairer(par2set)
        assert rep.load() == sabctools.Par2Result.SUCCESS
        assert rep.recoverable_file_count == 3
        assert rep.source_block_count > 0
        assert rep.recovery_block_count > 0
        assert rep.block_size > 0
        assert len(rep.setid) == 32

    def test_verify_before_load_is_rejected(self, par2set):
        rep = repairer(par2set)
        with pytest.raises(RuntimeError):
            rep.verify()

    def test_missing_parfile_is_reported_by_load(self, tmp_path):
        # Construction reads nothing, so a path that is not there surfaces as a load()
        # result rather than at construction
        rep = sabctools.Par2Repairer(str(tmp_path / "nope.par2"), basepath=str(tmp_path))
        assert rep.load() == sabctools.Par2Result.FILE_IO_ERROR

    @pytest.mark.parametrize(
        "kwargs",
        [
            {"memory_limit": 64 * 1024 * 1024},
            {"memory_limit": 1024},  # below a megabyte, must not become -m0
            {"threads": 2},
            {"file_threads": 1},
            {"skip_data": False},
            {"skip_leaway": 128},
            {"skip_repaired_verification": False},
        ],
    )
    def test_options_are_accepted(self, par2set, kwargs):
        rep = repairer(par2set, **kwargs)
        assert rep.load() == sabctools.Par2Result.SUCCESS

    def test_load_reports_block_budget(self, par2set):
        rep = repairer(par2set)
        rep.load()
        assert rep.recovery_block_count == RECOVERY_BLOCKS
        assert rep.source_block_count == 1920


class TestPar2Verify:
    def test_all_files_correct(self, par2set):
        rep = repairer(par2set)
        rep.load()
        assert rep.verify() == sabctools.Par2Result.SUCCESS
        assert rep.complete_file_count == 3
        assert rep.damaged_file_count == 0
        assert rep.missing_file_count == 0
        assert rep.missing_block_count == 0
        assert rep.repair_possible
        assert rep.renames == {}

    def test_damaged_file_is_detected(self, par2set):
        with open(os.path.join(par2set, "alpha.bin"), "r+b") as f:
            f.seek(20000)
            f.write(b"CORRUPTED" * 100)

        rep = repairer(par2set)
        rep.load()
        assert rep.verify() == sabctools.Par2Result.REPAIR_POSSIBLE
        assert rep.damaged_file_count == 1
        assert rep.complete_file_count == 2
        assert rep.missing_block_count > 0
        assert rep.repair_possible

    def test_missing_file_is_detected(self, par2set):
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()
        assert rep.verify() == sabctools.Par2Result.REPAIR_POSSIBLE
        assert rep.missing_file_count == 1
        assert rep.repair_possible

        gamma = [f for f in rep.files if f["name"] == "gamma.bin"][0]
        assert gamma["target"] == os.path.join(par2set, "gamma.bin")
        assert gamma["blocks"] == 320

    def test_file_details(self, par2set):
        rep = repairer(par2set)
        rep.load()
        rep.verify()
        assert sorted(f["name"] for f in rep.files) == sorted(DATA_FILES)
        for entry in rep.files:
            assert entry["blocks"] > 0
            assert entry["size"] > 0
            assert entry["target"] == os.path.join(par2set, entry["name"])


class TestPar2Repair:
    def test_repairs_damaged_and_missing(self, par2set, digests):
        with open(os.path.join(par2set, "alpha.bin"), "r+b") as f:
            f.seek(20000)
            f.write(b"CORRUPTED" * 100)
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()
        rep.verify()
        assert rep.repair() == sabctools.Par2Result.SUCCESS

        for name, expected in digests.items():
            assert md5(os.path.join(par2set, name)) == expected

    def test_repair_without_verify(self, par2set, digests):
        """repair() runs the verification pass itself when verify() was not called."""
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()
        assert rep.repair() == sabctools.Par2Result.SUCCESS
        assert md5(os.path.join(par2set, "gamma.bin")) == digests["gamma.bin"]

    def test_single_large_file_loss_exceeds_recovery(self, par2set):
        # beta.bin is 640 blocks against 576 recovery blocks
        os.remove(os.path.join(par2set, "beta.bin"))

        rep = repairer(par2set)
        rep.load()
        assert rep.verify() == sabctools.Par2Result.REPAIR_NOT_POSSIBLE
        assert not rep.repair_possible
        assert rep.missing_block_count == 640

    def test_insufficient_blocks(self, par2set):
        # Two of three files gone is more than 30% redundancy can rebuild
        os.remove(os.path.join(par2set, "alpha.bin"))
        os.remove(os.path.join(par2set, "beta.bin"))

        rep = repairer(par2set)
        rep.load()
        assert rep.verify() == sabctools.Par2Result.REPAIR_NOT_POSSIBLE
        assert not rep.repair_possible
        assert rep.missing_block_count > rep.recovery_block_count
        # The shortfall drives how many extra par2 blocks a caller needs to fetch
        assert rep.missing_block_count - rep.recovery_block_count > 0
        assert rep.repair() == sabctools.Par2Result.REPAIR_NOT_POSSIBLE


class TestPar2VerifyFile:
    """Feeding files in one at a time, which is how a download arrives."""

    def test_scans_only_the_named_file(self, par2set):
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()

        seen = []
        rep.file_done_callback = lambda name, found, total: seen.append(name)

        rep.verify_file(os.path.join(par2set, "alpha.bin"))
        assert seen == ["alpha.bin"]
        assert rep.complete_file_count == 1

    def test_rescan_replaces_what_the_first_scan_found(self, par2set, digests):
        # right size, wrong contents: what a file still downloading looks like
        target = os.path.join(par2set, "gamma.bin")
        good = open(target, "rb").read()
        with open(target, "r+b") as f:
            f.seek(1000)
            f.write(b"\0" * 4000)

        rep = repairer(par2set)
        rep.load()
        rep.verify_file(target)
        assert rep.damaged_file_count == 1
        partial = rep.available_block_count

        # it finishes, and is scanned again
        with open(target, "wb") as f:
            f.write(good)
        rep.verify_file(target)
        assert rep.damaged_file_count == 0
        assert rep.complete_file_count == 1
        assert rep.available_block_count > partial

    def test_file_scanned_before_load_is_replayed(self, par2set):
        rep = repairer(par2set)
        # nothing describes it yet
        assert rep.verify_file(os.path.join(par2set, "alpha.bin")) == (sabctools.Par2Result.INSUFFICIENT_CRITICAL_DATA)
        # loading the packets scans it
        rep.load()
        assert rep.complete_file_count == 1

    def test_repair_works_from_incremental_scans(self, par2set, digests):
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()
        for name in ("alpha.bin", "beta.bin"):
            rep.verify_file(os.path.join(par2set, name))

        assert rep.repair_possible
        assert rep.repair() == sabctools.Par2Result.SUCCESS
        assert md5(os.path.join(par2set, "gamma.bin")) == digests["gamma.bin"]


class TestPar2LoadMore:
    """Adding recovery blocks to a live repairer, the 'fetch more blocks' path.

    par2 finds sibling rec.vol*.par2 files by name during load(), so these tests stage
    the volume file only after loading to keep it out of the initial set.
    """

    @staticmethod
    def without_volumes(par2set):
        volumes = [f for f in os.listdir(par2set) if "vol" in f]
        held = {}
        for name in volumes:
            path = os.path.join(par2set, name)
            with open(path, "rb") as f:
                held[name] = f.read()
            os.remove(path)
        return held

    def test_more_blocks_make_repair_possible(self, par2set, digests):
        held = self.without_volumes(par2set)
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        assert rep.load() == sabctools.Par2Result.SUCCESS
        assert rep.recovery_block_count == 0
        assert rep.verify() == sabctools.Par2Result.REPAIR_NOT_POSSIBLE
        assert not rep.repair_possible
        assert rep.missing_block_count == 320

        # SABnzbd would have fetched these in the meantime
        restored = []
        for name, data in held.items():
            path = os.path.join(par2set, name)
            with open(path, "wb") as f:
                f.write(data)
            restored.append(path)

        assert rep.load_more(restored) == RECOVERY_BLOCKS
        assert rep.repair_possible
        assert rep.repair() == sabctools.Par2Result.SUCCESS
        assert md5(os.path.join(par2set, "gamma.bin")) == digests["gamma.bin"]

    def test_repair_after_load_more_does_not_reverify(self, par2set):
        held = self.without_volumes(par2set)
        os.remove(os.path.join(par2set, "gamma.bin"))

        rep = repairer(par2set)
        rep.load()
        rep.verify()

        restored = []
        for name, data in held.items():
            path = os.path.join(par2set, name)
            with open(path, "wb") as f:
                f.write(data)
            restored.append(path)
        rep.load_more(restored)

        # Only watch the repair; a re-verify would show up as "verifying" events
        stages = []
        rep.progress_callback = lambda stage, filename, percent: stages.append(stage)
        assert rep.repair() == sabctools.Par2Result.SUCCESS
        assert "verifying" not in stages
        assert "repairing" in stages

    def test_load_more_before_load_is_rejected(self, par2set):
        rep = repairer(par2set)
        with pytest.raises(RuntimeError):
            rep.load_more([])

    def test_load_more_is_idempotent(self, par2set):
        rep = repairer(par2set)
        rep.load()
        before = rep.recovery_block_count
        # Already pulled in by load(); loading it again must not double-count
        assert rep.load_more([os.path.join(par2set, "rec.vol000+576.par2")]) == before

    def test_load_more_rejects_a_missing_file(self, par2set):
        rep = repairer(par2set)
        rep.load()
        with pytest.raises(sabctools.Par2Error):
            rep.load_more([os.path.join(par2set, "does-not-exist.par2")])


class TestPar2Renames:
    def test_obfuscated_file_is_matched(self, par2set):
        obfuscated = os.path.join(par2set, "abc123def456ghi789.tmp")
        os.rename(os.path.join(par2set, "beta.bin"), obfuscated)

        rep = repairer(par2set, extrafiles=[obfuscated])
        rep.load()
        rep.verify()

        assert rep.renamed_file_count == 1
        assert rep.renames == {obfuscated: os.path.join(par2set, "beta.bin")}

    def test_repair_applies_the_rename(self, par2set, digests):
        obfuscated = os.path.join(par2set, "abc123def456ghi789.tmp")
        os.rename(os.path.join(par2set, "beta.bin"), obfuscated)

        rep = repairer(par2set, extrafiles=[obfuscated])
        rep.load()
        rep.verify()
        assert rep.repair() == sabctools.Par2Result.SUCCESS

        assert not os.path.exists(obfuscated)
        assert md5(os.path.join(par2set, "beta.bin")) == digests["beta.bin"]


class TestPar2Progress:
    def test_stages_and_percentages(self, par2set):
        os.remove(os.path.join(par2set, "gamma.bin"))

        events = []
        rep = repairer(par2set)
        rep.progress_callback = lambda stage, filename, percent: events.append((stage, filename, percent))
        rep.load()
        rep.verify()
        rep.repair()

        stages = {stage for stage, _, _ in events}
        assert {"loading", "verifying", "repairing", "verifying_repair"} <= stages
        for stage, _, percent in events:
            assert 0 <= percent <= 100, (stage, percent)

        # Loading reports the par2 files it opens, but no percentage - par2 signals a
        # byte offset rather than a fraction for that stage
        assert {f for stage, f, _ in events if stage == "loading"} == {"rec.par2", "rec.vol000+576.par2"}

    def test_callback_can_be_cleared(self, par2set):
        rep = repairer(par2set)
        rep.progress_callback = lambda *a: None
        assert rep.progress_callback is not None
        rep.progress_callback = None
        assert rep.progress_callback is None
        rep.load()
        assert rep.verify() == sabctools.Par2Result.SUCCESS

    def test_non_callable_is_rejected(self, par2set):
        rep = repairer(par2set)
        with pytest.raises(TypeError):
            rep.progress_callback = "not callable"

    @pytest.mark.filterwarnings("ignore::pytest.PytestUnraisableExceptionWarning")
    def test_callback_exception_does_not_break_the_run(self, par2set):
        def boom(stage, filename, percent):
            raise RuntimeError("callback blew up")

        rep = repairer(par2set)
        rep.progress_callback = boom
        rep.load()
        # Exceptions cannot unwind through par2's C++ frames, so they are reported
        # as unraisable and the run continues
        assert rep.verify() == sabctools.Par2Result.SUCCESS


class TestPar2Cancel:
    def test_cancel_from_callback(self, par2set):
        rep = repairer(par2set)
        rep.load()

        def cancel_once(stage, filename, percent):
            if stage == "verifying":
                rep.cancel()

        rep.progress_callback = cancel_once
        rep.verify()
        assert rep.cancelled

    def test_cancel_from_another_thread(self, par2set):
        rep = repairer(par2set)
        rep.load()

        started = threading.Event()
        rep.progress_callback = lambda *a: started.set()

        def canceller():
            started.wait(timeout=10)
            rep.cancel()

        thread = threading.Thread(target=canceller)
        thread.start()
        rep.verify()
        thread.join()
        assert rep.cancelled

    def test_cancel_before_running_is_harmless(self, par2set):
        rep = repairer(par2set)
        rep.cancel()
        assert rep.cancelled
