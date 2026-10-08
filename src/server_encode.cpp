// The server's encode thread: take the pending frame, (re)configure the
// stream when the size or codec changes, encode, and queue the packet to
// every client that can use it.
#include "server_impl.h"

namespace broremote {

void Server::Impl::encode_loop() {
    std::unique_ptr<Encoder> encoder;
    Codec enc_codec = Codec::Raw;
    uint32_t enc_kbps = 0;

    std::unique_lock<std::mutex> lk(m);
    for (;;) {
        // Wait for a frame to encode. With nobody attached it is released at
        // once; while a client is at its ack window it waits (a newer submit
        // replaces it) so the frame encoded on resume is the newest.
        encode_cv.wait(lk, [&] { return stop || (pending && (attached.load() == 0 || !paused())); });
        if (stop) break;
        Pending p = std::move(*pending);
        pending.reset();
        if (attached.load() == 0) {
            ++stats.unwatched;
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
            ec.width = f.width;
            ec.height = f.height;
            ec.fps = cfg.fps;
            ec.bitrate_kbps = kbps;
            encoder = create_encoder(codec, ec, &err);
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
            ++stats.failed;
            encoder.reset();
            stream.reset();
            // The stream cannot go on: tell every client why and close it.
            const std::string why = std::string(codec_name(codec)) + " encoder: " + err;
            for (auto& c : clients) {
                if (c->attached && !c->closing && !c->dead) close_client(*c, ErrorCode::EncoderFailed, why);
            }
            waker.wake();
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
        const uint64_t id = next_frame_id++;
        const SharedMessage msg = std::make_shared<const std::string>(
            VideoPacket::encode_message(stream->stream_id, id, pkt.pts_ns, pkt.keyframe, pkt.data, &timing));
        // A client that has not had a keyframe of this stream cannot use a
        // predicted frame: it skips them until the next keyframe (which its
        // joining already requested).
        const Clock::time_point queued = Clock::now();
        for (auto& c : clients) {
            if (!c->attached || c->closing || c->dead) continue;
            if (!pkt.keyframe && !c->synced) continue;
            c->synced = true;
            queue(*c, msg);
            if (c->minor >= 1 && !c->dead) c->marks.push_back({msg.get(), id, queued});
            c->unacked.push_back(id);
        }
        ++stats.encoded;
        if (pkt.keyframe) {
            ++stats.keyframes;
            last_keyframe = Clock::now();
        }
        waker.wake();
    }
}

}  // namespace broremote
