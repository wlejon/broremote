#include "broremote/latency.h"

#include <algorithm>

namespace broremote {

namespace {

constexpr size_t kMaxTraces = 512;
constexpr size_t kMaxSamples = 32;
constexpr auto kProbeTimeout = std::chrono::seconds(2);

double us_of(Clock::time_point t) {
    return double(std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

}  // namespace

int64_t marker_from_luma(const uint8_t (&luma)[kMarkerBits]) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < kMarkerBits; ++i) v = (v << 1) | (luma[i] >= 128 ? 1u : 0u);
    return int64_t(v);
}

int64_t read_marker(const DecodedFrame& f) {
    if (f.memory != brovideo::PictureMemory::Cpu || f.data.empty()) return -1;
    if (f.width < (3 + kMarkerBits) * kMarkerBlock || f.height < (kMarkerRow + 1) * kMarkerBlock) return -1;
    uint8_t luma[kMarkerBits];
    for (uint32_t i = 0; i < kMarkerBits; ++i) {
        uint32_t x = 0, y = 0;
        marker_point(i, x, y);
        if (f.format == PixelFormat::NV12) {
            luma[i] = f.data[size_t(y) * f.stride + x];  // limited range: black 16, white 235
        } else {
            const uint8_t* p = f.data.data() + size_t(y) * f.stride + size_t(x) * 4;
            luma[i] = uint8_t((p[0] + p[1] + p[2]) / 3);
        }
    }
    return marker_from_luma(luma);
}

void LatencyTracker::on_pong(Clock::time_point sent, Clock::time_point received, uint64_t server_us) {
    if (received < sent) return;
    Sample s;
    s.rtt_us = us_of(received) - us_of(sent);
    s.offset_us = double(server_us) - (us_of(sent) + us_of(received)) / 2;
    s.at = received;
    std::lock_guard<std::mutex> lk(m_);
    sum_.pongs += 1;
    sum_.rtt_mean += s.rtt_us / 1000.0;
    sum_.rtt_max = std::max(sum_.rtt_max, s.rtt_us / 1000.0);
    samples_.push_back(s);
    while (samples_.size() > kMaxSamples) samples_.pop_front();
}

double LatencyTracker::rtt_locked() const {
    if (samples_.empty()) return -1;
    double best = samples_.front().rtt_us;
    for (const Sample& s : samples_) best = std::min(best, s.rtt_us);
    return best / 1000.0;
}

double LatencyTracker::rtt_ms() const {
    std::lock_guard<std::mutex> lk(m_);
    return rtt_locked();
}

std::optional<Clock::time_point> LatencyTracker::to_local(uint64_t server_us) const {
    if (samples_.empty()) return std::nullopt;
    const Sample* best = &samples_.front();
    for (const Sample& s : samples_) {
        if (s.rtt_us < best->rtt_us) best = &s;
    }
    const double local_us = double(server_us) - best->offset_us;
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double, std::micro>(local_us)));
}

LatencyTracker::Trace* LatencyTracker::find(uint64_t frame_id) {
    for (auto it = traces_.rbegin(); it != traces_.rend(); ++it) {
        if (it->frame_id == frame_id) return &*it;
    }
    return nullptr;
}

void LatencyTracker::on_video(const VideoPacket& v, Clock::time_point received) {
    std::lock_guard<std::mutex> lk(m_);
    Trace t;
    t.frame_id = v.frame_id;
    t.timing = v.timing;
    t.bytes = v.data.size();
    t.received = received;
    traces_.push_back(t);
    while (traces_.size() > kMaxTraces) traces_.pop_front();
}

void LatencyTracker::on_frame_sent(const FrameSentMsg& f) {
    std::lock_guard<std::mutex> lk(m_);
    if (Trace* t = find(f.frame_id)) {
        t->have_sent = true;
        t->wait_us = f.wait_us;
        t->write_us = f.write_us;
    }
}

void LatencyTracker::on_decoded(uint64_t frame_id, Clock::time_point start, Clock::time_point done, int64_t marker) {
    std::lock_guard<std::mutex> lk(m_);
    Trace* t = find(frame_id);
    if (t) {
        t->dstart = start;
        t->decoded = done;
        t->have_decoded = true;
    }
    if (marker >= 0) {
        marker_ = std::max(marker_, marker);
        if (probe_ && !probe_->decoded && marker >= probe_->expect) {
            probe_->decoded = true;
            probe_->decoded_at = done;
            probe_->frame_id = frame_id;
        }
    }
}

void LatencyTracker::on_presented(uint64_t frame_id, Clock::time_point when) {
    std::lock_guard<std::mutex> lk(m_);
    Trace* t = find(frame_id);
    std::optional<Clock::time_point> submit;
    if (t && t->timing.valid) submit = to_local(t->timing.submit_us);
    if (t && t->have_decoded && submit) {
        const auto encoded = *submit + std::chrono::microseconds(t->timing.queue_us + t->timing.encode_us);
        const auto send_start = encoded + std::chrono::microseconds(t->have_sent ? t->wait_us : 0);
        sum_.frames += 1;
        sum_.queue += double(t->timing.queue_us) / 1000.0;
        sum_.encode += double(t->timing.encode_us) / 1000.0;
        sum_.wait += double(t->have_sent ? t->wait_us : 0) / 1000.0;
        sum_.net += ms(t->received - send_start);
        sum_.max_net = std::max(sum_.max_net, ms(t->received - send_start));
        sum_.max_kbytes = std::max(sum_.max_kbytes, double(t->bytes) / 1000.0);
        sum_.dwait += ms(t->dstart - t->received);
        sum_.decode += ms(t->decoded - t->dstart);
        sum_.present += ms(when - t->decoded);
        const double age = ms(when - *submit);
        sum_.age += age;
        sum_.max_age = std::max(sum_.max_age, age);
        sum_.kbytes += double(t->bytes) / 1000.0;
    }
    if (probe_ && probe_->decoded && frame_id >= probe_->frame_id) {
        Probe p;
        p.total_decoded = ms(probe_->decoded_at - probe_->sent);
        p.total_presented = ms(when - probe_->sent);
        p.rtt = rtt_locked();
        p.present = ms(when - probe_->decoded_at);
        if (Trace* a = find(probe_->frame_id); a && a->timing.valid) {
            if (auto s = to_local(a->timing.submit_us)) {
                const auto encoded = *s + std::chrono::microseconds(a->timing.queue_us + a->timing.encode_us);
                const auto send_start = encoded + std::chrono::microseconds(a->have_sent ? a->wait_us : 0);
                p.uplink = ms(*s - probe_->sent);
                p.queue = double(a->timing.queue_us) / 1000.0;
                p.encode = double(a->timing.encode_us) / 1000.0;
                p.wait = double(a->have_sent ? a->wait_us : 0) / 1000.0;
                p.net = ms(a->received - send_start);
                p.dwait = ms(a->dstart - a->received);
                p.decode = ms(a->decoded - a->dstart);
            }
        }
        probes_.push_back(p);
        probe_.reset();
    }
}

bool LatencyTracker::begin_probe(Clock::time_point now) {
    std::lock_guard<std::mutex> lk(m_);
    if (probe_) {
        if (now - probe_->sent < kProbeTimeout) return false;
        ++lost_;
        probe_.reset();
    }
    if (marker_ < 0) return false;  // no marker seen yet: not serve-test --latency, or no picture
    OpenProbe p;
    p.sent = now;
    p.expect = marker_ + 1;
    probe_ = p;
    return true;
}

bool LatencyTracker::probe_open(Clock::time_point now) {
    std::lock_guard<std::mutex> lk(m_);
    if (probe_ && now - probe_->sent >= kProbeTimeout) {
        ++lost_;
        probe_.reset();
    }
    return probe_.has_value();
}

std::vector<Probe> LatencyTracker::probes() const {
    std::lock_guard<std::mutex> lk(m_);
    return probes_;
}

uint64_t LatencyTracker::probes_lost() const {
    std::lock_guard<std::mutex> lk(m_);
    return lost_;
}

LatencyWindow LatencyTracker::take_window() {
    std::lock_guard<std::mutex> lk(m_);
    LatencyWindow w = sum_;
    sum_ = LatencyWindow{};
    if (w.pongs) w.rtt_mean /= double(w.pongs);
    if (w.frames) {
        const double n = double(w.frames);
        w.queue /= n;
        w.encode /= n;
        w.wait /= n;
        w.net /= n;
        w.dwait /= n;
        w.decode /= n;
        w.present /= n;
        w.age /= n;
        w.kbytes /= n;
    }
    w.rtt = rtt_locked();
    return w;
}

}  // namespace broremote
