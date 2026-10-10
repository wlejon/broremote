// The server's encode thread: take the pending frame, (re)configure the
// stream when the size or codec changes, encode, and queue the packet to
// every client that can use it.
//
// Between frames it repeats the last picture (Encoder::encode_repeat). A
// host that submits only when the screen changed (a compositor holding an
// unchanged screen) would otherwise leave the first frame after a big change
// as soft as rate control made it (the 50 ms HRD buffer: near 18 dB on a
// desktop at 20 Mbit/s, against 50 once settled) for as long as the screen
// stays still. So after each frame, while no newer one comes, the picture is
// encoded again once a frame interval, predicted, until two packets running
// are under a quarter of the per-frame budget (it has caught up) or a second
// has passed. And a keyframe asked for (a viewer joining, or one that lost
// sync) is made from the last picture at once rather than waiting for the
// screen to change.
#include "server_impl.h"

#include <algorithm>

namespace broremote {

void Server::Impl::encode_loop() {
    std::unique_ptr<Encoder> encoder;
    Codec enc_codec = Codec::Raw;
    uint32_t enc_kbps = 0;
    const uint32_t fps = std::max<uint32_t>(1, cfg.fps);
    const auto interval = std::chrono::microseconds(1000000 / fps);
    int repeats_left = 0;  // predicted repeats still allowed after the last frame
    int small_run = 0;     // repeats running whose packet was under the bar
    Clock::time_point repeat_at{};

    std::unique_lock<std::mutex> lk(m);

    // Queues a packet to every attached client that can use it (`m` held).
    // A client that has not had a keyframe of this stream cannot use a
    // predicted frame: it skips them until the next keyframe (which its
    // joining already requested).
    const auto send = [&](const EncodedPacket& pkt, const FrameTiming* timing) {
        const uint64_t id = next_frame_id++;
        const SharedMessage msg = std::make_shared<const std::string>(
            VideoPacket::encode_message(stream->stream_id, id, pkt.pts_ns, pkt.keyframe, pkt.data, timing));
        const Clock::time_point queued = Clock::now();
        for (auto& c : clients) {
            if (!c->attached || c->closing || c->dead) continue;
            if (!pkt.keyframe && !c->synced) continue;
            c->synced = true;
            queue(*c, msg);
            if (c->minor >= 1 && !c->dead) c->marks.push_back({msg.get(), id, queued});
            c->unacked.push_back(id);
        }
        if (pkt.keyframe) {
            ++stats.keyframes;
            last_keyframe = Clock::now();
        }
    };
    // The stream cannot go on: tell every client why and close it (`m` held).
    const auto fail = [&](Codec codec, const std::string& err) {
        ++stats.failed;
        encoder.reset();
        stream.reset();
        repeats_left = 0;
        const std::string why = std::string(codec_name(codec)) + " encoder: " + err;
        for (auto& c : clients) {
            if (c->attached && !c->closing && !c->dead) close_client(*c, ErrorCode::EncoderFailed, why);
        }
        waker.wake();
    };
    // After a frame or a keyframe repeat: sharpen it while nothing newer comes.
    // Raw is lossless: nothing to sharpen.
    const auto start_repeats = [&](Clock::time_point now) {
        repeats_left = enc_codec == Codec::Raw ? 0 : int(fps);
        small_run = 0;
        repeat_at = now + interval;
    };
    const auto frame_ready = [&] { return pending && (attached.load() == 0 || !paused()); };
    // The encoder's picture can be sent again as it is: the stream is the one
    // the clients want, and nobody is at its ack window.
    const auto can_repeat = [&] {
        return encoder && encoder->can_repeat() && stream && attached.load() > 0 && !paused() &&
               enc_codec == want_codec && enc_kbps == want_kbps;
    };

    for (;;) {
        if (stop) break;
        if (!frame_ready()) {
            // No frame to encode. With nobody attached it is released at
            // once; while a client is at its ack window it waits (a newer
            // submit replaces it) so the frame encoded on resume is the
            // newest. Meanwhile, the last picture again: a keyframe at once
            // when one was asked for, else a sharpening repeat when due.
            const bool key = keyframe_requested && can_repeat();
            const bool due = repeats_left > 0 && can_repeat();
            if (!key && !(due && Clock::now() >= repeat_at)) {
                if (repeats_left > 0 && !paused() && !can_repeat()) repeats_left = 0;  // the stream changed
                if (due) encode_cv.wait_until(lk, repeat_at);
                else encode_cv.wait(lk);  // a submit, an ack, a keyframe request, or stop
                continue;
            }
            keyframe_requested = false;
            const uint32_t bar = uint32_t(uint64_t(enc_kbps) * 1000 / 8 / fps / 4);
            lk.unlock();
            EncodedPacket pkt;
            std::string err;
            const bool ok = encoder->encode_repeat(key, pkt, &err);
            const Clock::time_point now = Clock::now();
            lk.lock();
            if (!ok) {
                fail(enc_codec, err);
                continue;
            }
            send(pkt, nullptr);  // no FrameTiming: no host frame behind it
            ++stats.repeats;
            if (pkt.keyframe) {
                start_repeats(now);
            } else {
                small_run = pkt.data.size() < bar ? small_run + 1 : 0;
                repeats_left = small_run >= 2 ? 0 : repeats_left - 1;
                repeat_at = now + interval;
            }
            waker.wake();
            continue;
        }

        Pending p = std::move(*pending);
        pending.reset();
        if (attached.load() == 0) {
            ++stats.unwatched;
            repeats_left = 0;
            lk.unlock();
            if (p.release) p.release();
            lk.lock();
            continue;
        }

        const Frame& f = p.frame;
        bool keyframe = keyframe_requested;
        keyframe_requested = false;
        if (cfg.keyframe_interval_s > 0 &&
            Clock::now() - last_keyframe >= std::chrono::seconds(cfg.keyframe_interval_s)) {
            keyframe = true;
        }
        const bool reconfigure = !encoder || !stream || stream->width != f.width || stream->height != f.height ||
                                 enc_codec != want_codec || enc_kbps != want_kbps;
        const Codec codec = want_codec;
        const uint32_t kbps = want_kbps;
        lk.unlock();
        const Clock::time_point encode_start = Clock::now();

        ReleaseOnce release(std::move(p.release));
        std::string err;
        bool ok = true;
        if (reconfigure) {
            EncoderConfig ec;
            ec.codec = codec;
            ec.width = f.width;
            ec.height = f.height;
            ec.fps = cfg.fps;
            ec.bitrate_kbps = kbps;
            // A new size or bitrate in the same codec reconfigures the
            // encoder in place (for a bitrate change the VA-API one keeps
            // its device, context and surfaces); a new codec, or a backend
            // that refuses, gets a new one.
            if (!encoder || enc_codec != codec || !encoder->reconfigure(ec, nullptr)) {
                encoder = brovideo::create_encoder(ec, &err);
            }
            ok = encoder != nullptr;
            enc_codec = codec;
            enc_kbps = kbps;
            keyframe = true;
        }
        EncodedPacket pkt;
        if (ok) ok = encoder->encode(f, keyframe, [&release] { release.run(); }, pkt, &err);
        release.run();
        const Clock::time_point encode_end = Clock::now();
        FrameTiming timing;
        timing.valid = true;
        timing.submit_us = mono_us(p.submitted);
        timing.queue_us = span_us(p.submitted, encode_start);
        timing.encode_us = span_us(encode_start, encode_end);

        lk.lock();
        if (!ok) {
            fail(codec, err);
            continue;
        }
        if (reconfigure) {
            StreamConfig sc;
            sc.stream_id = ++stream_counter;
            sc.codec = codec;
            sc.width = f.width;
            sc.height = f.height;
            sc.fps = cfg.fps;
            stream = sc;
            stream_kbps = kbps;
            ++stats.streams;
            queue_attached(std::make_shared<const std::string>(sc.encode()));
            for (auto& c : clients) c->synced = false;
        }
        send(pkt, &timing);
        ++stats.encoded;
        start_repeats(encode_end);
        waker.wake();
    }
}

}  // namespace broremote
