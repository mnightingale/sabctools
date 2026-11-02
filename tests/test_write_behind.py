import gc
import os
import threading

import pytest

import sabctools
from tests.test_decoder_sink import build_article, feed

# The pool is process-wide and its size is fixed once started
THREADS = 2
BUFFERS = 4
STAGING = 256 * 1024


@pytest.fixture(autouse=True)
def write_behind():
    sabctools.set_write_behind(THREADS, BUFFERS)
    yield
    sabctools.set_write_behind(0)


class TestConfiguration:
    def test_stats_report_the_pool(self):
        stats = sabctools.write_behind_stats()
        assert stats["enabled"] is True
        assert stats["threads"] == THREADS
        assert stats["buffers"] == BUFFERS

    def test_the_size_cannot_change_once_started(self):
        with pytest.raises(RuntimeError):
            sabctools.set_write_behind(THREADS + 1, BUFFERS)

    def test_enabling_needs_buffers(self):
        with pytest.raises(ValueError):
            sabctools.set_write_behind(THREADS)

    def test_disabling_turns_it_off(self):
        sabctools.set_write_behind(0)
        assert sabctools.write_behind_stats()["enabled"] is False


class TestOutput:
    def test_body_spanning_more_buffers_than_the_pool(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "big.bin"))
        payload = os.urandom((BUFFERS * 3) * STAGING + 12345)
        decoder = sabctools.Decoder(STAGING)
        decoder.expect("big", writer)
        response = feed(decoder, build_article(payload))[0]
        writer.flush()
        writer.close()

        assert response.bytes_decoded == len(payload)
        assert response.crc is not None
        assert open(writer.path, "rb").read() == payload

    def test_many_connections_share_the_pool(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "shared.bin"))
        parts = [os.urandom(700_000) for _ in range(24)]
        size = sum(len(part) for part in parts)
        decoders = [sabctools.Decoder(STAGING) for _ in range(8)]

        def connection(index):
            decoder = decoders[index]
            for part in range(index, len(parts), len(decoders)):
                decoder.expect(part, writer)
                begin = part * 700_000
                feed(decoder, build_article(parts[part], begin=begin, total=size, part=part + 1))

        threads = [threading.Thread(target=connection, args=(index,)) for index in range(len(decoders))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        writer.close()

        assert open(writer.path, "rb").read() == b"".join(parts)

    def test_buffers_return_to_the_pool(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "returned.bin"))
        decoder = sabctools.Decoder(STAGING)
        for index in range(10):
            decoder.expect(index, writer)
            feed(decoder, build_article(os.urandom(300_000), begin=index * 300_000, total=3_000_000))
        writer.flush()

        stats = sabctools.write_behind_stats()
        assert 0 < stats["allocated"] <= BUFFERS
        assert stats["free"] == stats["allocated"]
        writer.close()

    def test_a_dropped_writer_still_lands_its_writes(self, tmp_path):
        path = str(tmp_path / "dropped.bin")
        payload = os.urandom(2 * STAGING)
        writer = sabctools.FileWriter(path)
        decoder = sabctools.Decoder(STAGING)
        decoder.expect("dropped", writer)
        response = feed(decoder, build_article(payload))[0]
        del writer, response
        decoder.clear_expected()
        gc.collect()

        assert open(path, "rb").read() == payload

    def test_disabled_writes_inline(self, tmp_path):
        sabctools.set_write_behind(0)
        writer = sabctools.FileWriter(str(tmp_path / "inline.bin"))
        payload = os.urandom(100_000)
        decoder = sabctools.Decoder(STAGING)
        decoder.expect("inline", writer)
        feed(decoder, build_article(payload))

        # No flush or close: an inline write has completed when the response has
        assert open(writer.path, "rb").read() == payload
        writer.close()

    def test_more_connections_than_buffers_on_one_thread(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "crowded.bin"))
        connections = BUFFERS + 2
        payloads = [os.urandom(700_000) for _ in range(connections)]
        size = 700_000 * connections
        wires = []
        decoders = []
        for index, payload in enumerate(payloads):
            decoder = sabctools.Decoder(STAGING)
            decoder.expect(index, writer)
            decoders.append(decoder)
            wires.append(build_article(payload, begin=index * 700_000, total=size, part=index + 1))

        def round_robin():
            positions = [0] * connections
            while any(position < len(wire) for position, wire in zip(positions, wires)):
                for index, decoder in enumerate(decoders):
                    wire = wires[index]
                    if positions[index] >= len(wire):
                        continue
                    buffer = memoryview(decoder)
                    count = min(len(buffer), len(wire) - positions[index], 64 * 1024)
                    buffer[:count] = wire[positions[index] : positions[index] + count]
                    buffer.release()
                    decoder.process(count)
                    positions[index] += count
                    list(decoder)

        thread = threading.Thread(target=round_robin, daemon=True)
        thread.start()
        thread.join(timeout=20)
        assert not thread.is_alive(), "decoders holding every buffer between reads starved the rest"
        writer.close()

        assert open(writer.path, "rb").read() == b"".join(payloads)


class TestTrim:
    def write(self, writer, count=10, size=300_000):
        decoder = sabctools.Decoder(STAGING)
        payloads = [os.urandom(size) for _ in range(count)]
        for index, payload in enumerate(payloads):
            decoder.expect(index, writer)
            feed(decoder, build_article(payload, begin=index * size, total=count * size, part=index + 1))
        return b"".join(payloads)

    def test_idle_buffers_are_freed(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "idle.bin"))
        self.write(writer)
        writer.flush()
        sabctools.write_behind_trim()

        stats = sabctools.write_behind_stats()
        assert stats["allocated"] == 0
        assert stats["free"] == 0
        writer.close()

    def test_buffers_still_being_written_are_freed_when_done(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "in_flight.bin"))
        expected = self.write(writer, count=20)
        sabctools.write_behind_trim()
        writer.flush()

        assert sabctools.write_behind_stats()["allocated"] == 0
        writer.close()
        assert open(writer.path, "rb").read() == expected

    def test_decoding_after_a_trim_allocates_again(self, tmp_path):
        sabctools.write_behind_trim()
        writer = sabctools.FileWriter(str(tmp_path / "again.bin"))
        expected = self.write(writer)
        writer.flush()

        stats = sabctools.write_behind_stats()
        assert stats["allocated"] > 0
        assert stats["free"] == stats["allocated"]
        writer.close()
        assert open(writer.path, "rb").read() == expected


class TestErrors:
    def test_a_closed_sink_is_reported_on_the_response(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "closed.bin"))
        writer.close()
        decoder = sabctools.Decoder(STAGING)
        decoder.expect("doomed", writer)
        decoder.expect("fine", None)
        payload = os.urandom(1000)
        responses = feed(decoder, build_article(payload) + build_article(payload))

        assert responses[0].sink_failed
        assert isinstance(responses[0].sink_error, ValueError)
        assert not responses[1].sink_failed
        assert bytes(responses[1].data) == payload

    def test_flush_raises_nothing_after_success(self, tmp_path):
        writer = sabctools.FileWriter(str(tmp_path / "ok.bin"))
        decoder = sabctools.Decoder(STAGING)
        decoder.expect("ok", writer)
        feed(decoder, build_article(os.urandom(1000)))
        writer.flush()
        writer.close()
