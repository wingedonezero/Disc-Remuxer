/*
 * disc-remuxer: the Matroska writer (see mkv.h).
 *
 * The rules are mkvmerge's (MKVToolNix 102.0, read from its source), so a
 * file written here equals the one mkvmerge writes from the same streams:
 *
 * - layout: EBML head; Segment (8-byte size); a 4096-byte void that takes
 *   the SeekHead at the end; Info (written with its default values);
 *   Tracks; a void of 1024 bytes plus the room the track headers' default
 *   values would take, less what the headers grew by once the first frames
 *   were known; a void for the chapters (their size + 100); the clusters;
 *   at the end the Cues, the chapters (into their void), the Tags, the
 *   SeekHead (Info, Tracks, Cues, Tags, Chapters), the sizes and the EBML
 *   head's DocType versions as the elements used need them.
 * - times: each packet's time and duration rounded to the timestamp scale
 *   (1 ms) to the nearest before libmatroska sees them (libmatroska cuts).
 * - packets go out in time order across the tracks (the lowest time of the
 *   tracks' next packets; the first track on a tie).
 * - clusters: a new one at a video key frame, after 5 s, 65535 blocks or
 *   1.5 MB, or when a block's offset would not fit 16 bits.
 * - blocks: SimpleBlock, a BlockGroup when a duration must be written;
 *   audio key frames laced (libmatroska's choice of lacing); a duration is
 *   written when the block's rounded duration differs from the default.
 * - cues: every video key frame; audio only without video (every 0.5 s);
 *   with the block's position inside its cluster.
 * - tags: per track BPS, DURATION, NUMBER_OF_FRAMES, NUMBER_OF_BYTES and
 *   the statistics' writing application, date and list.
 * - H.264 (as mkvmerge's AVC elementary-stream packetizer): access unit
 *   delimiters, end of sequence / stream and filler data left out, every
 *   other NAL unit kept with a 4-byte size; the configuration record from
 *   the SPS and PPS before the first slice; the display size from the SPS's
 *   sample aspect ratio; the default duration the frames' most common
 *   duration, snapped to a common frame rate within 20 µs.
 * - PCM: blocks of rate / 25 samples (40 ms), times counted in samples from
 *   the first frame's time, counted again from a frame whose time is more
 *   than 1 ms off.
 */

#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxChapters.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxClusterData.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTag.h>
#include <matroska/KaxTags.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackEntryData.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTracks.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sys/random.h>

#include <zlib.h>

extern "C" {
#include "base/crc16.h"
#include "mkv/mkv.h"
}

using namespace libebml;
using namespace libmatroska;

namespace {

const int64_t SCALE          = 1000000;           // the timestamp scale: 1 ms
const int64_t MAX_NS_CLUSTER = 5000000000LL;      // min(32700 x scale, 5 s)
const int     MAX_BLOCKS     = 65535;
const int     MAX_CONTENT    = 1500000;
const int64_t SPARSE_WAIT    = 10000000000LL;     // how far the others run ahead of a subtitle track

int64_t round_scale(int64_t ns)
{
    return std::llround(static_cast<double>(ns) / SCALE) * SCALE;
}

// ---- UIDs: random and non-zero, or 1, 2, 3 ... per kind (fixed_ids) ----

enum { UID_TRACK, UID_EDITION, UID_CHAPTER, UID_KINDS };

struct Uids {
    bool fixed = false;
    uint64_t next[UID_KINDS] = { 1, 1, 1 };
    uint64_t make(int kind)
    {
        if (fixed)
            return next[kind]++;
        uint64_t v = 0;
        while (!v)
            if (getrandom(&v, sizeof(v), 0) != sizeof(v))
                v = (uint64_t)time(nullptr) * 2654435761u + next[kind]++;
        return v;
    }
};

// ---- H.264 (ITU-T H.264 7.3.2.1.1: the SPS fields the track header needs) ----

struct Bits {
    std::vector<uint8_t> b;
    size_t pos = 0;
    explicit Bits(const uint8_t *p, size_t n)
    {
        // the RBSP: emulation prevention bytes (00 00 03) left out
        for (size_t i = 0; i < n; i++) {
            if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0 && b.size() >= 2 && !b[b.size() - 1] &&
                !b[b.size() - 2])
                continue;
            b.push_back(p[i]);
        }
    }
    bool left() const { return pos < b.size() * 8; }
    unsigned u(int n)
    {
        unsigned v = 0;
        while (n--) {
            v = (v << 1) | (left() ? (b[pos >> 3] >> (7 - (pos & 7))) & 1 : 0);
            pos++;
        }
        return v;
    }
    unsigned ue()
    {
        int z = 0;
        while (left() && !u(1))
            z++;
        return ((1u << z) - 1) + u(z);
    }
    int se()
    {
        unsigned k = ue();
        return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2);
    }
};

struct H264Sps {
    unsigned id = 0;
    int width = 0, height = 0;
    unsigned sar_num = 0, sar_den = 0;      // 0: no aspect ratio in the VUI
};

const unsigned sar_table[17][2] = {
    { 0, 0 }, { 1, 1 }, { 12, 11 }, { 10, 11 }, { 16, 11 }, { 40, 33 }, { 24, 11 }, { 20, 11 }, { 32, 11 },
    { 80, 33 }, { 18, 11 }, { 15, 11 }, { 64, 33 }, { 160, 99 }, { 4, 3 }, { 3, 2 }, { 2, 1 },
};

bool parse_sps(const uint8_t *nal, size_t n, H264Sps &s)
{
    Bits r(nal + 1, n - 1);
    unsigned profile = r.u(8);
    r.u(16);
    s.id = r.ue();
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = r.ue();
        if (chroma == 3)
            r.u(1);
        r.ue();
        r.ue();
        r.u(1);
        if (r.u(1))
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
                if (r.u(1)) {
                    int last = 8, next = 8;
                    for (int j = 0; j < (i < 6 ? 16 : 64); j++) {
                        if (next)
                            next = (last + r.se() + 256) % 256;
                        last = next ? next : last;
                    }
                }
    }
    r.ue();
    unsigned poc_type = r.ue();
    if (poc_type == 0)
        r.ue();
    else if (poc_type == 1) {
        r.u(1);
        r.se();
        r.se();
        unsigned k = r.ue();
        for (unsigned i = 0; i < k; i++)
            r.se();
    }
    r.ue();
    r.u(1);
    unsigned mbs_w = r.ue() + 1, map_h = r.ue() + 1;
    unsigned frame_mbs_only = r.u(1);
    if (!frame_mbs_only)
        r.u(1);
    r.u(1);
    unsigned crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (r.u(1)) {
        crop_l = r.ue();
        crop_r = r.ue();
        crop_t = r.ue();
        crop_b = r.ue();
    }
    unsigned sub_w = chroma == 1 || chroma == 2 ? 2 : 1, sub_h = chroma == 1 ? 2 : 1;
    unsigned unit_x = chroma ? sub_w : 1, unit_y = (chroma ? sub_h : 1) * (2 - frame_mbs_only);
    s.width  = (int)(mbs_w * 16 - unit_x * (crop_l + crop_r));
    s.height = (int)((2 - frame_mbs_only) * map_h * 16 - unit_y * (crop_t + crop_b));
    if (r.u(1) && r.u(1)) {                     // vui_parameters_present, aspect_ratio_info_present
        unsigned idc = r.u(8);
        if (idc == 255) {
            s.sar_num = r.u(16);
            s.sar_den = r.u(16);
        } else if (idc < 17) {
            s.sar_num = sar_table[idc][0];
            s.sar_den = sar_table[idc][1];
        }
    }
    return r.left();
}

// the NAL units of an access unit: each start code's payload, trailing zero bytes left out
void nal_units(const uint8_t *p, size_t n, std::vector<std::pair<const uint8_t *, size_t>> &out)
{
    std::vector<size_t> sc;
    out.clear();
    for (size_t i = 0; i + 3 <= n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) {
            sc.push_back(i);
            i += 2;
        }
    for (size_t k = 0; k < sc.size(); k++) {
        size_t s = sc[k] + 3, e = k + 1 < sc.size() ? sc[k + 1] : n;
        while (e > s && !p[e - 1])
            e--;
        if (e > s)
            out.push_back({ p + s, e - s });
    }
}

// ---- a packet: a block's worth of a track ----

struct Packet {
    std::vector<uint8_t> data;
    int     track = 0;
    int64_t timestamp = 0, duration = -1;   // as given (ns)
    int64_t assigned = 0;                   // the time it is placed at (rounded when placed)
    int64_t unmodified_assigned = 0, unmodified_duration = -1;
    int     key = -1, discardable = -1;     // -1: not said
    bool    duration_mandatory = false;
    uint64_t raw_size = 0;                  // the data's size before compression (the statistics count it)
    bool has_duration() const { return duration >= 0; }
    int64_t get_duration() const { return has_duration() ? duration : 0; }
    int64_t get_unmodified_duration() const { return has_duration() ? unmodified_duration : 0; }
    bool is_key_frame() const { return key != 0; }
};
using PacketPtr = std::shared_ptr<Packet>;

// ---- libmatroska with mkvmerge's additions ----

// a cluster whose smallest / largest block times are set from outside
class Cluster : public KaxCluster {
public:
    Cluster() { PreviousTimecode = 0; }
    void set_min(int64_t t) { MinTimecode = t; }
    void set_max(int64_t t) { MaxTimecode = t; }
    void delete_non_blocks()
    {
        for (unsigned i = 0; i < ListSize(); i++) {
            EbmlElement *e = (*this)[i];
            if (!dynamic_cast<KaxBlockGroup *>(e) && !dynamic_cast<KaxSimpleBlock *>(e))
                delete e;
        }
        RemoveAll();
    }
    ~Cluster() override { delete_non_blocks(); }
};

// a block blob whose SimpleBlock flags come from the packet
class BlockBlob : public KaxBlockBlob {
public:
    explicit BlockBlob(BlockBlobType mode) : KaxBlockBlob(mode) {}
    bool add_frame(const KaxTrackEntry &track, uint64 ts, DataBuffer &buf, LacingType lacing, int key,
                   int discardable)
    {
        if (SimpleBlockMode == BLOCK_BLOB_ALWAYS_SIMPLE || SimpleBlockMode == BLOCK_BLOB_SIMPLE_AUTO) {
            if (!Block.simpleblock) {
                Block.simpleblock = new KaxSimpleBlock();
                Block.simpleblock->SetParent(*ParentCluster);
            }
            bool more = Block.simpleblock->AddFrame(track, ts, buf, lacing);
            if (key >= 0 || discardable >= 0) {
                Block.simpleblock->SetKeyframe(key > 0);
                Block.simpleblock->SetDiscardable(discardable > 0);
            } else {
                Block.simpleblock->SetKeyframe(true);
                Block.simpleblock->SetDiscardable(false);
            }
            return more;
        }
        replace_simple_by_group();
        KaxBlock &block = GetChild<KaxBlock>(*Block.group);
        block.SetParent(*ParentCluster);
        Block.group->SetParentTrack(track);
        return block.AddFrame(track, ts, buf, lacing);
    }
    bool replace_simple_by_group()
    {
        if (SimpleBlockMode == BLOCK_BLOB_ALWAYS_SIMPLE)
            return false;
        if (!bUseSimpleBlock) {
            if (!Block.group)
                Block.group = new KaxBlockGroup();
        } else {
            Block.group = new KaxBlockGroup();
        }
        if (ParentCluster)
            Block.group->SetParent(*ParentCluster);
        bUseSimpleBlock = false;
        return true;
    }
    void set_block_duration(uint64 ns)
    {
        if (replace_simple_by_group())
            Block.group->SetBlockDuration(ns);
    }
};
using BlobPtr = std::shared_ptr<BlockBlob>;

// a track's blocks inside one cluster being built
struct RenderGroup {
    int track;
    std::vector<BlobPtr> groups;
    std::vector<int64_t> durations;
    bool more_data = false, duration_mandatory = false;
    int64_t expected_next = -1;
    bool has_first_rounding = false;
    int64_t first_rounding = 0;
    bool follows_gap(const Packet &p) const
    {
        return expected_next != -1 && std::llabs(p.assigned - expected_next) > 2 * SCALE;
    }
};

struct Stats {
    int64_t min_ts = INT64_MAX, max_end = INT64_MIN;
    uint64_t frames = 0, bytes = 0;
};

struct CuePoint {
    int64_t timestamp;
    uint64_t track, cluster_position, relative_position;
};

// one block whose position goes into a cue point once its cluster is written
struct CueBlock {
    int64_t timestamp;
    uint64_t track;
    std::shared_ptr<BlockBlob> blob;
};

std::string format_ts(int64_t ns)
{
    char buf[64];
    bool neg = ns < 0;
    if (neg)
        ns = -ns;
    snprintf(buf, sizeof(buf), "%s%02lld:%02lld:%02lld.%09lld", neg ? "-" : "", (long long)(ns / 3600000000000LL),
             (long long)(ns / 60000000000LL % 60), (long long)(ns / 1000000000LL % 60),
             (long long)(ns % 1000000000LL));
    return buf;
}

std::string bcp47_of(const char *iso639_2);
std::string bibliographic(const std::string &iso639_2);

}  // namespace

// ---- the writer ----

struct MkvTrackState {
    MkvTrackSpec spec{};
    std::string language, language_ietf, name;
    int number = 0;
    uint64_t uid = 0;
    KaxTrackEntry *entry = nullptr;
    std::deque<PacketPtr> queue;
    bool done = false;              // no packet comes any more
    bool sparse = false;            // subtitles: a packet now and then (the others do not wait for one)
    bool header_ready = false;      // its header's late fields (H.264: configuration record) are known
    int64_t default_duration = -1;  // ns
    int64_t last_cue = -1;
    Stats stats;
    // H.264
    std::vector<std::vector<uint8_t>> sps, pps;
    std::vector<unsigned> sps_ids, pps_ids;
    std::vector<uint8_t> avcc;
    H264Sps first_sps;
    bool have_sps = false;
    bool config_ready = false, ps_changed = false;  // the first slice was seen; a parameter set changed after it
    std::map<int64_t, int> duration_count;
    std::deque<PacketPtr> held;     // H.264 frames until the configuration record is made
    // AC-3 / DTS: packets counted in samples from a reference time (a frame's own time when the count is
    // more than 1 ms off it)
    PacketPtr pending;              // AC-3: the frame being built (an independent frame and its dependent ones)
    int pending_samples = 0, pending_rate = 0;
    int64_t pending_time = 0;
    uint64_t ac3_layout = 0;        // the first packet's speakers and LFE channels (its channel count)
    int ac3_lfe = 0;
    int64_t ref_time = 0, samples_since = 0;
    int count_rate = 0;
    bool counting = false;
    int64_t dropped_negative = 0;
    // PCM
    std::vector<uint8_t> pcm;
    int64_t samples_out = 0;
    int samples_per_packet = 0, packet_size = 0, frame_bytes = 0;
    bool pcm_started = false;
    std::string codec_id;
};

struct MkvWriter {
    std::string path;
    StdIOCallback *out = nullptr;
    MkvFileSpec spec{};
    std::string muxing_app, writing_app;
    std::vector<MkvChapter> est_chapters;
    std::vector<std::string> est_names;
    std::vector<MkvTrackState> tracks;
    Uids uids;
    bool started = false;           // the headers are written
    int video = -1;                 // the first video track (key frames start clusters)
    int64_t latest = INT64_MIN;     // the latest time of any packet queued
    int64_t last_out = INT64_MIN;   // the latest time of any packet written
    bool failed = false;
    int error = 0;

    EbmlHead head;
    KaxSegment segment;
    EbmlVoid seek_void, tracks_void, chapters_void;
    KaxSeekHead seek;
    KaxInfo *info = nullptr;
    KaxDuration *duration = nullptr;
    KaxTracks *ktracks = nullptr;
    bool used_ietf = false;

    // the cluster being built (mkvmerge's cluster helper)
    std::unique_ptr<Cluster> cluster;
    std::vector<PacketPtr> packets;
    int content_size = 0;
    int64_t previous_cluster_ts = -1, min_in_cluster = -1, max_in_cluster = -1;
    bool first_video_key_seen = false;
    int64_t max_ts_and_duration = 0, min_ts_in_file = INT64_MAX;
    std::vector<CuePoint> cue_points;
    std::vector<std::unique_ptr<Cluster>> written;  // clusters written (their blocks may be referenced)
    MkvStats st{};
};

namespace {

int fail(MkvWriter *w, int err)
{
    if (!w->failed) {
        w->failed = true;
        w->error = err ? err : EIO;
    }
    return -w->error;
}

// ---- the track headers (mkvmerge's set_headers order) ----

void remove_deprecated(EbmlMaster &m)
{
    for (unsigned i = 0; i < m.ListSize();) {
        EbmlElement *e = m[i];
        const EbmlId &id = EbmlId(*e);
        if (id == EBML_ID(KaxTrackMinCache) || id == EBML_ID(KaxTrackMaxCache) || id == EBML_ID(KaxCodecDecodeAll) ||
            id == EBML_ID(KaxTrackTimecodeScale) || id == EBML_ID(KaxTrackOffset)) {
            delete e;
            m.Remove(i);
            continue;
        }
        if (auto sub = dynamic_cast<EbmlMaster *>(e))
            remove_deprecated(*sub);
        i++;
    }
}

void set_track_headers(MkvWriter *w, MkvTrackState &t, bool late)
{
    KaxTrackEntry &e = *t.entry;
    if (!late) {
        GetChild<KaxTrackNumber>(e).SetValue(t.number);
        GetChild<KaxTrackUID>(e).SetValue(t.uid);
        GetChild<KaxTrackType>(e).SetValue(t.spec.codec == MKV_CODEC_H264 ? track_video :
                                           t.spec.codec == MKV_CODEC_PGS ? track_subtitle : track_audio);
        GetChild<KaxCodecID>(e).SetValue(t.codec_id);
        if (t.default_duration > 0)
            GetChild<KaxTrackDefaultDuration>(e).SetValue(t.default_duration);
        GetChild<KaxTrackLanguage>(e).SetValue(t.language);
        GetChild<KaxLanguageIETF>(e).SetValue(t.language_ietf);
        w->used_ietf = true;
        if (!t.name.empty())
            GetChild<KaxTrackName>(e).SetValueUTF8(t.name);
        if (t.spec.forced)
            GetChild<KaxTrackFlagForced>(e).SetValue(1);
        if (t.spec.codec == MKV_CODEC_H264) {
            // the picture's size as the SPS gives it (cropped), as mkvmerge takes it
            if (t.have_sps && t.first_sps.width > 0 && t.first_sps.height > 0) {
                t.spec.width  = t.first_sps.width;
                t.spec.height = t.first_sps.height;
            }
            KaxTrackVideo &v = GetChild<KaxTrackVideo>(e);
            GetChild<KaxVideoPixelWidth>(v).SetValue(t.spec.width);
            GetChild<KaxVideoPixelHeight>(v).SetValue(t.spec.height);
            GetChild<KaxVideoDisplayWidth>(v).SetValue(t.spec.width);
            GetChild<KaxVideoDisplayHeight>(v).SetValue(t.spec.height);
            GetChild<KaxVideoDisplayWidth>(v).SetDefaultSize(4);
            GetChild<KaxVideoDisplayHeight>(v).SetDefaultSize(4);
            e.EnableLacing(false);
        } else if (t.spec.codec == MKV_CODEC_PGS) {
            // the compression last of the common fields, then no lacing (as mkvmerge's PGS track)
            if (t.spec.zlib) {
                KaxContentEncoding &c = GetChild<KaxContentEncoding>(GetChild<KaxContentEncodings>(e));
                GetChild<KaxContentEncodingOrder>(c).SetValue(0);
                GetChild<KaxContentEncodingType>(c).SetValue(0);    // a compression
                GetChild<KaxContentEncodingScope>(c).SetValue(1);   // of the frames
                GetChild<KaxContentCompAlgo>(GetChild<KaxContentCompression>(c)).SetValue(0);   // zlib
            }
            e.EnableLacing(false);
        } else {
            KaxTrackAudio &a = GetChild<KaxTrackAudio>(e);
            GetChild<KaxAudioSamplingFreq>(a).SetValue((double)t.spec.sample_rate);
            GetChild<KaxAudioChannels>(a).SetValue(t.spec.channels);
            if (t.spec.bits > 0)
                GetChild<KaxAudioBitDepth>(a).SetValue(t.spec.bits);
        }
        return;
    }
    // H.264's late fields: the configuration record and the display size from the SPS
    GetChild<KaxCodecPrivate>(e).CopyBuffer(t.avcc.data(), t.avcc.size());
    if (t.first_sps.sar_den) {
        KaxTrackVideo &v = GetChild<KaxTrackVideo>(e);
        int64_t w_ = t.spec.width, h_ = t.spec.height, n = t.first_sps.sar_num, d = t.first_sps.sar_den;
        if (n >= d)
            w_ = (t.spec.width * n * 2 + d) / (2 * d);
        else
            h_ = (t.spec.height * d * 2 + n) / (2 * n);
        GetChild<KaxVideoDisplayWidth>(v).SetValue(w_);
        GetChild<KaxVideoDisplayHeight>(v).SetValue(h_);
    }
}

// ---- writing the headers (once every track's late fields are known) ----

int64_t chapters_size(MkvWriter *w, const MkvChapter *ch, int n, const std::vector<std::string> *names);

void build_chapters(KaxChapters &chapters, Uids &uids, const MkvChapter *ch, int n)
{
    // as mkvmerge writes chapters given as XML: EditionUID; per atom ChapterTimeStart, ChapterUID and a
    // display with the name, its language (eng) and its IETF language (en)
    chapters.RemoveAll();
    KaxEditionEntry &ed = AddNewChild<KaxEditionEntry>(chapters);
    while (ed.ListSize()) {
        delete ed[0];
        ed.Remove(0);
    }
    GetChild<KaxEditionUID>(ed).SetValue(uids.make(UID_EDITION));
    for (int i = 0; i < n; i++) {
        KaxChapterAtom &a = AddNewChild<KaxChapterAtom>(ed);
        while (a.ListSize()) {
            delete a[0];
            a.Remove(0);
        }
        GetChild<KaxChapterTimeStart>(a).SetValue(ch[i].start);
        GetChild<KaxChapterUID>(a).SetValue(uids.make(UID_CHAPTER));
        KaxChapterDisplay &d = GetChild<KaxChapterDisplay>(a);
        while (d.ListSize()) {
            delete d[0];
            d.Remove(0);
        }
        char def[32];
        snprintf(def, sizeof(def), "Chapter %02d", i + 1);
        GetChild<KaxChapterString>(d).SetValueUTF8(ch[i].name && *ch[i].name ? ch[i].name : def);
        GetChild<KaxChapterLanguage>(d).SetValue("eng");
        GetChild<KaxChapLanguageIETF>(d).SetValue("en");
    }
}

void render_void(MkvWriter *w, EbmlVoid &v, int64_t total)
{
    int64_t actual = total;
    v.SetSize(total);
    v.UpdateSize();
    while ((int64_t)v.ElementSize() > total)
        v.SetSize(--actual);
    if ((int64_t)v.ElementSize() < total)
        v.SetSizeLength(total - actual - 1);
    v.Render(*w->out);
}

int write_headers(MkvWriter *w)
{
    try {
        w->out = new StdIOCallback(w->path.c_str(), MODE_CREATE);
    } catch (...) {
        return fail(w, errno);
    }
    try {
        // EBML head with every default (its DocType versions raised at the end)
        GetChild<EDocType>(w->head).SetValue("matroska");
        GetChild<EDocTypeVersion>(w->head).SetValue(1);
        GetChild<EDocTypeReadVersion>(w->head).SetValue(1);
        w->head.Render(*w->out, true);

        w->segment.WriteHead(*w->out, 8);
        w->seek_void.SetSize(4096);
        w->seek_void.Render(*w->out);

        // Info: TimestampScale, MuxingApp, WritingApp (made with it), Duration, DateUTC, SegmentUID
        w->info = &GetChild<KaxInfo>(w->segment);
        w->duration = new KaxDuration;
        w->duration->SetPrecision(w->video >= 0 ? EbmlFloat::FLOAT_32 : EbmlFloat::FLOAT_64);
        w->duration->SetValue(0.0);
        w->info->PushElement(*w->duration);
        GetChild<KaxTimecodeScale>(*w->info).SetValue(SCALE);
        GetChild<KaxMuxingApp>(*w->info).SetValueUTF8(w->muxing_app);
        GetChild<KaxWritingApp>(*w->info).SetValueUTF8(w->writing_app);
        GetChild<KaxDateUTC>(*w->info).SetEpochDate(w->spec.date);
        uint8_t suid[16] = { 0 };
        if (!w->uids.fixed && getrandom(suid, sizeof(suid), 0) != sizeof(suid))
            return fail(w, EIO);
        GetChild<KaxSegmentUID>(*w->info).CopyBuffer(suid, sizeof(suid));
        w->info->Render(*w->out, true);
        w->seek.IndexThis(*w->info, w->segment);

        // Tracks: rendered as they are before the first frames (the late fields not yet there), to size the
        // void after them as mkvmerge does, then with the late fields
        w->ktracks = &GetChild<KaxTracks>(w->segment);
        for (size_t i = 0; i < w->tracks.size(); i++) {
            MkvTrackState &t = w->tracks[i];
            t.entry = i == 0 ? &GetChild<KaxTrackEntry>(*w->ktracks)
                             : &GetNextChild<KaxTrackEntry>(*w->ktracks, *w->tracks[i - 1].entry);
            t.entry->SetGlobalTimecodeScale(SCALE);
            remove_deprecated(*t.entry);
            set_track_headers(w, t, false);
        }
        w->ktracks->UpdateSize(true);
        int64_t full = w->ktracks->ElementSize(true);
        w->ktracks->UpdateSize(false);
        int64_t initial = w->ktracks->ElementSize(false);
        bool late = false;
        for (auto &t : w->tracks)
            if (t.spec.codec == MKV_CODEC_H264) {
                set_track_headers(w, t, true);
                late = true;
            }
        w->ktracks->UpdateSize(false);
        w->ktracks->Render(*w->out, false);
        w->seek.IndexThis(*w->ktracks, w->segment);
        {
            EbmlVoid first;
            first.SetSize(1024 + full - initial);
            first.UpdateSize();
            int64_t first_total = first.ElementSize();
            int64_t total = late ? initial + first_total - (int64_t)w->ktracks->ElementSize(false) : first_total;
            if (late && total < 4)
                total = 1024;
            render_void(w, w->tracks_void, total);
        }

        // the chapters' room: their size (from the title list) + 100
        if (!w->est_chapters.empty()) {
            int64_t size = chapters_size(w, w->est_chapters.data(), (int)w->est_chapters.size(), &w->est_names);
            w->chapters_void.SetSize(size + 100);
            w->chapters_void.Render(*w->out);
        }
    } catch (...) {
        return fail(w, errno ? errno : EIO);
    }
    w->started = true;
    return 0;
}

int64_t chapters_size(MkvWriter *w, const MkvChapter *ch, int n, const std::vector<std::string> *)
{
    KaxChapters c;
    Uids u = w->uids;               // the same UIDs as the chapters to come
    build_chapters(c, u, ch, n);
    c.UpdateSize(true);
    return c.ElementSize(true);
}

// ---- clusters (mkvmerge's cluster helper) ----

int64_t cluster_timestamp(MkvWriter *w)
{
    return w->packets.empty() ? 0 : w->packets.front()->assigned;
}

void prepare_cluster(MkvWriter *w)
{
    w->cluster.reset(new Cluster);
    w->content_size = 0;
    w->packets.clear();
    w->cluster->SetParent(w->segment);
    w->cluster->SetPreviousTimecode(std::max<int64_t>(0, w->previous_cluster_ts), SCALE);
}

bool must_duration_be_set(MkvWriter *w, RenderGroup *rg, const Packet &p)
{
    int64_t block = 0, def = w->tracks[p.track].default_duration;
    int64_t n = rg ? (int64_t)rg->durations.size() : 0;
    if (rg)
        for (int64_t d : rg->durations)
            block += d;
    block += p.get_duration();
    if ((rg && rg->duration_mandatory) || p.duration_mandatory)
        return (block == 0 && (n || (p.duration_mandatory && p.has_duration()))) ||
               (block > 0 && round_scale(block) != round_scale((n + 1) * def));
    return def > 0 && block > 0 && round_scale(block) != round_scale((n + 1) * def);
}

void set_duration(MkvWriter *w, RenderGroup &rg)
{
    if (rg.durations.empty())
        return;
    BlockBlob *g = rg.groups.back().get();
    int64_t def = w->tracks[rg.track].default_duration, block = 0;
    for (int64_t d : rg.durations)
        block += d;
    if (rg.duration_mandatory) {
        if (block == 0 || (block > 0 && round_scale(block) != round_scale((int64_t)rg.durations.size() * def)))
            g->set_block_duration(round_scale(block));
    } else if (def > 0 && block > 0 && round_scale(block) != round_scale((int64_t)rg.durations.size() * def))
        g->set_block_duration(round_scale(block));
}

bool add_to_cues(MkvWriter *w, const Packet &p)
{
    MkvTrackState &t = w->tracks[p.track];
    bool add = false;
    if (t.spec.codec == MKV_CODEC_H264 || t.spec.codec == MKV_CODEC_PGS)
        add = p.is_key_frame();     // video and subtitles: every key frame
    else if (w->video < 0)
        add = p.is_key_frame() && (t.last_cue < 0 || p.assigned - t.last_cue >= 500000000);
    if (add)
        t.last_cue = p.assigned;
    return add;
}

int render_cluster(MkvWriter *w)
{
    std::vector<std::unique_ptr<RenderGroup>> groups;
    std::vector<CueBlock> cue_blocks;
    int64_t min_ts = INT64_MAX, max_ts = 0;
    int elements = 0;
    bool added_to_cues = false;

    for (auto &pp : w->packets) {
        Packet &p = *pp;
        MkvTrackState &t = w->tracks[p.track];
        RenderGroup *rg = nullptr;
        for (auto &g : groups)
            if (g->track == p.track)
                rg = g.get();
        if (!rg) {
            groups.emplace_back(new RenderGroup);
            groups.back()->track = p.track;
            rg = groups.back().get();
        }
        min_ts = std::min(p.assigned, min_ts);
        max_ts = std::max(p.assigned, max_ts);

        bool new_group = !rg->more_data || !p.is_key_frame() || rg->follows_gap(p) ||
                         must_duration_be_set(w, nullptr, p);
        BlockBlob *blob = rg->groups.empty() ? nullptr : rg->groups.back().get();
        if (new_group) {
            set_duration(w, *rg);
            rg->durations.clear();
            rg->duration_mandatory = false;
            rg->has_first_rounding = false;
            BlockBlobType type = must_duration_be_set(w, rg, p) ? BLOCK_BLOB_NO_SIMPLE : BLOCK_BLOB_ALWAYS_SIMPLE;
            rg->groups.emplace_back(new BlockBlob(type));
            blob = rg->groups.back().get();
            w->cluster->AddBlockBlob(blob);
            blob->SetParent(*w->cluster);
            added_to_cues = false;
        }
        if (!rg->has_first_rounding) {
            rg->has_first_rounding = true;
            rg->first_rounding = p.unmodified_assigned - p.assigned;
        }
        // the block frees the buffer (not the data: the packet keeps it until the cluster is gone)
        auto *buf = new DataBuffer(p.data.data(), (uint32)p.data.size());
        rg->more_data = blob->add_frame(*t.entry, p.assigned, *buf, LACING_AUTO, p.key, p.discardable);
        w->min_ts_in_file = std::min(w->min_ts_in_file, p.assigned);
        w->max_ts_and_duration = std::max(w->max_ts_and_duration, p.assigned + p.get_duration());
        if (!p.is_key_frame() || !t.entry->LacingEnabled())
            rg->more_data = false;
        if (p.has_duration())
            rg->durations.push_back(p.get_unmodified_duration());
        rg->duration_mandatory |= p.duration_mandatory;
        rg->expected_next = p.assigned + p.get_duration();
        elements++;
        if (new_group && !added_to_cues) {
            added_to_cues = add_to_cues(w, p);
            if (added_to_cues)
                cue_blocks.push_back({ p.assigned, (uint64_t)t.number, rg->groups.back() });
        }
        t.stats.frames++;
        t.stats.bytes += p.raw_size ? p.raw_size : p.data.size();
        t.stats.min_ts = std::min(t.stats.min_ts, p.assigned);
        t.stats.max_end = std::max(t.stats.max_end, p.assigned + p.get_duration());
        w->st.blocks += new_group;
    }
    if (elements) {
        for (auto &g : groups)
            set_duration(w, *g);
        w->cluster->SetPreviousTimecode((uint64)(min_ts - 1), SCALE);
        w->cluster->set_min(min_ts);
        w->cluster->set_max(max_ts);
        KaxCues cues;
        cues.SetGlobalTimecodeScale(SCALE);
        try {
            w->cluster->Render(*w->out, cues);
        } catch (...) {
            return fail(w, errno ? errno : EIO);
        }
        w->st.clusters++;
        w->previous_cluster_ts = w->cluster->GlobalTimecode();
        // cue points: the block's cluster and its place in the cluster
        uint64_t cluster_pos = w->segment.GetRelativePosition(*w->cluster);
        uint64_t data_start = w->cluster->GetDataStart();
        for (auto &cb : cue_blocks) {
            KaxInternalBlock &b = *cb.blob;
            uint64_t pos = cb.blob->IsSimpleBlock() ? static_cast<KaxSimpleBlock &>(*cb.blob).GetElementPosition()
                                                    : static_cast<KaxBlockGroup &>(*cb.blob).GetElementPosition();
            (void)b;
            w->cue_points.push_back({ cb.timestamp, cb.track, cluster_pos, std::max(pos, data_start) - data_start });
        }
        cues.RemoveAll();
    } else
        w->previous_cluster_ts = -1;
    w->cluster->delete_non_blocks();
    // the blobs (and their blocks) go with their groups
    w->cluster.reset();
    w->min_in_cluster = w->max_in_cluster = -1;
    return 0;
}

int add_packet(MkvWriter *w, const PacketPtr &pp)
{
    Packet &p = *pp;
    if (!w->cluster)
        prepare_cluster(w);
    // the times rounded to the timestamp scale
    p.unmodified_assigned = p.assigned;
    p.unmodified_duration = p.duration;
    p.assigned = round_scale(p.assigned);
    if (p.has_duration())
        p.duration = round_scale(p.duration);

    // a cluster ends before this packet?
    {
        int64_t ts = cluster_timestamp(w);
        int64_t hi = (p.assigned > w->max_in_cluster || w->max_in_cluster == -1) ? p.assigned : w->max_in_cluster;
        int64_t lo = (w->min_in_cluster == -1 || p.assigned < w->min_in_cluster) ? p.assigned : w->min_in_cluster;
        int64_t delay = (hi - lo) / SCALE;
        bool video_key = p.track == w->video && p.is_key_frame();
        bool render = delay > 32767 || delay < -32768 ||
                      (std::max<int64_t>(0, w->min_in_cluster) > w->previous_cluster_ts &&
                       p.assigned > w->min_in_cluster && (w->video < 0 || !video_key || w->first_video_key_seen) &&
                       ((p.assigned - ts) > MAX_NS_CLUSTER || video_key));
        if (video_key)
            w->first_video_key_seen = true;
        if (render) {
            if (render_cluster(w) < 0)
                return -w->error;
            prepare_cluster(w);
        }
    }
    w->packets.push_back(pp);
    w->content_size += (int)p.data.size();
    if (p.assigned > w->max_in_cluster)
        w->max_in_cluster = p.assigned;
    if (w->min_in_cluster == -1 || p.assigned < w->min_in_cluster)
        w->min_in_cluster = p.assigned;
    // ... or after it
    if (p.assigned - cluster_timestamp(w) > MAX_NS_CLUSTER || (int)w->packets.size() > MAX_BLOCKS ||
        w->content_size > MAX_CONTENT) {
        if (render_cluster(w) < 0)
            return -w->error;
        prepare_cluster(w);
    }
    return 0;
}

// the tracks' next packets go out in time order once every track has one (or has ended). A subtitle track
// is not waited for once the other tracks have come SPARSE_WAIT past the next packet: its next display set
// cannot be earlier than that, as the disc delivers a set before its time (a set that comes later anyway is
// counted in MkvStats.late)
int pump(MkvWriter *w, bool flushing)
{
    for (auto &t : w->tracks)
        if (!t.queue.empty())
            w->latest = std::max(w->latest, t.queue.back()->assigned);
    for (;;) {
        int winner = -1;
        bool waiting = false;
        for (size_t i = 0; i < w->tracks.size(); i++) {
            MkvTrackState &t = w->tracks[i];
            if (t.queue.empty()) {
                if (!t.done && !flushing) {
                    if (!t.sparse)
                        return 0;
                    waiting = true;
                }
                continue;
            }
            if (winner < 0 || t.queue.front()->assigned < w->tracks[winner].queue.front()->assigned)
                winner = (int)i;
        }
        if (winner < 0)
            return 0;
        if (waiting && w->latest - w->tracks[winner].queue.front()->assigned < SPARSE_WAIT)
            return 0;
        if (!w->started && write_headers(w) < 0)
            return -w->error;
        PacketPtr p = w->tracks[winner].queue.front();
        w->tracks[winner].queue.pop_front();
        w->last_out = std::max(w->last_out, p->assigned);
        if (add_packet(w, p) < 0)
            return -w->error;
    }
}

// ---- the codecs ----

int64_t snap_default_duration(int64_t d)
{
    static const int64_t common[] = {
        1000000000LL / 50, 1000000000LL / 25, 1000000000LL / 60, 1000000000LL / 30,
        1000000000LL * 1001 / 48000, 1000000000LL * 1001 / 24000, 1000000000LL * 1001 / 60000,
        1000000000LL * 1001 / 30000,
    };
    int64_t best = d, diff = INT64_MAX;
    for (int64_t c : common)
        if (std::llabs(d - c) < 20000 && std::llabs(d - c) < diff) {
            best = c;
            diff = std::llabs(d - c);
        }
    return best;
}

void h264_make_header(MkvTrackState &t)
{
    // ISO/IEC 14496-15 5.3.3.1: version, profile, compatibility, level, 4-byte sizes, the SPS, the PPS
    const std::vector<uint8_t> &s = t.sps.front();
    t.avcc = { 1, s[1], s[2], s[3], 0xFF };
    t.avcc.push_back(0xE0 | (uint8_t)t.sps.size());
    for (auto &x : t.sps) {
        t.avcc.push_back((uint8_t)(x.size() >> 8));
        t.avcc.push_back((uint8_t)x.size());
        t.avcc.insert(t.avcc.end(), x.begin(), x.end());
    }
    t.avcc.push_back((uint8_t)t.pps.size());
    for (auto &x : t.pps) {
        t.avcc.push_back((uint8_t)(x.size() >> 8));
        t.avcc.push_back((uint8_t)x.size());
        t.avcc.insert(t.avcc.end(), x.begin(), x.end());
    }
    // the most common duration of the frames held so far, snapped to a common frame rate
    int64_t most = -1;
    int count = 0;
    for (auto &dc : t.duration_count)
        if (dc.second > count) {
            most = dc.first;
            count = dc.second;
        }
    t.default_duration = most > 0 ? snap_default_duration(most) : 1000000000LL / 25;
    t.header_ready = true;
}

// a recovery point SEI message in an SEI NAL unit (H.264 D.1, payload type 6)
bool has_recovery_point(const uint8_t *nal, size_t n)
{
    Bits r(nal + 1, n - 1);
    while (r.pos + 16 <= r.b.size() * 8) {
        unsigned type = 0, size = 0, b;
        while ((b = r.u(8)) == 0xFF)
            type += 255;
        type += b;
        while ((b = r.u(8)) == 0xFF)
            size += 255;
        size += b;
        if (type == 6)
            return true;
        r.pos += (size_t)size * 8;
        if (r.b.size() * 8 - r.pos <= 8)        // the rbsp_trailing_bits
            break;
    }
    return false;
}

// One frame of the title: a block per picture in it, as mkvmerge makes its frames (H.264 7.4.1.2.4: a new
// picture at an access unit delimiter or at a slice that starts a picture): a frame of two field
// pictures gives a block per field, each with its own time (the frame's, then one field later), its
// own key flag (an IDR slice or a recovery point in it; the first field takes the frame's) and its own
// discardable flag (its first slice's nal_ref_idc 0). The SPS / PPS lists follow every parameter set
// (by id, the newest content); once one changed after the first slice, each key frame's own parameter
// sets are replaced by the whole lists in front of it (as mkvmerge does: the key frame then holds every
// current SPS and PPS).
int h264_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time, int64_t dur, int key)
{
    std::vector<std::pair<const uint8_t *, size_t>> nals;
    nal_units(data, size, nals);
    using Nal = std::pair<const uint8_t *, size_t>;
    struct Pic {
        std::vector<Nal> prefix, body;  // the units before its first slice, and from there on
        bool idr = false, recovery = false;
        int discardable = -1;
    };
    std::vector<Pic> pics;
    bool slice_in_pic = false;
    int track = (int)(&t - w->tracks.data());
    for (auto &n : nals) {
        int type = n.first[0] & 31;
        bool slice = type == 1 || type == 5;
        bool starts = pics.empty() || (slice_in_pic && (type == 9 || (slice && (Bits(n.first + 1, n.second - 1).ue() == 0))));
        if (starts) {
            pics.push_back(Pic());
            slice_in_pic = false;
        }
        Pic &pic = pics.back();
        if (type == 9 || type == 10 || type == 11 || type == 12)
            continue;               // access unit delimiter, end of sequence / stream, filler data
        if (type == 7 || type == 8) {
            // the lists: a new id or new content (after the first slice: the lists changed)
            std::vector<uint8_t> v(n.first, n.first + n.second);
            std::vector<std::vector<uint8_t>> &list = type == 7 ? t.sps : t.pps;
            std::vector<unsigned> &ids = type == 7 ? t.sps_ids : t.pps_ids;
            unsigned id;
            if (type == 7) {
                H264Sps sps;
                parse_sps(n.first, n.second, sps);
                id = sps.id;
                if (!t.have_sps) {
                    t.first_sps = sps;
                    t.have_sps = true;
                }
            } else {
                Bits r(n.first + 1, n.second - 1);
                id = r.ue();
            }
            auto it = std::find(ids.begin(), ids.end(), id);
            if (it == ids.end()) {
                ids.push_back(id);
                list.push_back(v);
                t.ps_changed |= t.config_ready;
            } else if (list[it - ids.begin()] != v) {
                list[it - ids.begin()] = v;
                t.ps_changed |= t.config_ready;
            }
        }
        if (type == 6 && has_recovery_point(n.first, n.second))
            pic.recovery = true;
        if (slice && !slice_in_pic) {
            pic.discardable = !(n.first[0] >> 5);   // nal_ref_idc 0
            pic.idr = type == 5;
            if (!t.config_ready && !t.sps.empty() && !t.pps.empty())
                t.config_ready = true;
        }
        if (slice)
            slice_in_pic = true;
        (slice_in_pic ? pic.body : pic.prefix).push_back(n);
    }
    // pieces with no slice (data after the last picture) go with the picture before them
    for (size_t k = pics.size(); k-- > 1;)
        if (pics[k].discardable < 0) {
            Pic &prev = pics[k - 1];
            prev.body.insert(prev.body.end(), pics[k].prefix.begin(), pics[k].prefix.end());
            prev.body.insert(prev.body.end(), pics[k].body.begin(), pics[k].body.end());
            pics.erase(pics.begin() + k);
        }
    int n = (int)pics.size();
    std::vector<PacketPtr> out;
    for (int k = 0; k < n; k++) {
        Pic &pic = pics[k];
        auto p = std::make_shared<Packet>();
        p->track = track;
        p->timestamp = p->assigned = time + (dur > 0 ? dur * k / n : 0);
        p->duration = dur > 0 ? dur / n : -1;
        p->key = k == 0 ? key : (pic.idr || pic.recovery);
        p->discardable = pic.discardable < 0 ? 0 : pic.discardable;
        auto put = [&](const uint8_t *d, size_t len) {
            uint8_t h[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
            p->data.insert(p->data.end(), h, h + 4);
            p->data.insert(p->data.end(), d, d + len);
        };
        if (k == 0 && key && t.ps_changed) {
            for (auto &x : t.sps)
                put(x.data(), x.size());
            for (auto &x : t.pps)
                put(x.data(), x.size());
            for (auto &x : pic.prefix)
                if ((x.first[0] & 31) != 7 && (x.first[0] & 31) != 8)
                    put(x.first, x.second);
        } else
            for (auto &x : pic.prefix)
                put(x.first, x.second);
        for (auto &x : pic.body)
            put(x.first, x.second);
        out.push_back(p);
    }
    if (!t.header_ready) {
        for (auto &p : out) {
            if (p->duration > 0)
                t.duration_count[p->duration]++;
            t.held.push_back(p);
        }
        // the header is made at the second key frame (when mkvmerge's parser gives out the first GOP): the
        // configuration record takes the parameter sets as they are then, the default duration the first
        // GOP's most common frame duration
        if (!(key && t.held.size() > (size_t)n))
            return 0;
        if (t.sps.empty() || t.pps.empty())
            return fail(w, EINVAL);
        h264_make_header(t);
        for (auto &h : t.held)
            t.queue.push_back(h);
        t.held.clear();
        return pump(w, false);
    }
    for (auto &p : out)
        t.queue.push_back(p);
    return pump(w, false);
}

// ---- counted audio (AC-3, DTS): as mkvmerge's timestamp calculator ----

// A packet of `samples` samples at `rate` that came with a frame at `time`: its time counted from the
// reference (a frame whose time is more than 1 ms off the count starts it again). Packets before 0 go,
// as mkvmerge drops them.
void counted_packet(MkvWriter *w, MkvTrackState &t, PacketPtr p, int64_t time, int samples, int rate,
                    int64_t duration)
{
    if (!t.counting || rate != t.count_rate) {
        t.counting = true;
        t.count_rate = rate;
        t.ref_time = time;
        t.samples_since = 0;
    } else if (time != INT64_MIN) {
        int64_t expected = t.ref_time + t.samples_since * 1000000000LL / rate;
        if (std::llabs(time - expected) > 1000000) {
            t.ref_time = time;
            t.samples_since = 0;
        }
    }
    p->timestamp = p->assigned = t.ref_time + t.samples_since * 1000000000LL / rate;
    p->duration = duration;
    t.samples_since += samples;
    if (p->timestamp < 0) {
        t.dropped_negative++;
        return;
    }
    t.queue.push_back(p);
}

// ---- AC-3 / E-AC-3 (ATSC A/52; as mkvmerge reads their headers) ----

enum : uint64_t {
    SP_FL = 1 << 0, SP_FR = 1 << 1, SP_FC = 1 << 2, SP_LFE = 1 << 3, SP_BL = 1 << 4, SP_BR = 1 << 5,
    SP_FLC = 1 << 6, SP_FRC = 1 << 7, SP_BC = 1 << 8, SP_SL = 1 << 9, SP_SR = 1 << 10, SP_TC = 1 << 11,
    SP_TFL = 1 << 12, SP_TFC = 1 << 13, SP_TFR = 1 << 14, SP_TBL = 1 << 15, SP_TBR = 1 << 17,
    SP_SDL = 1 << 18, SP_SDR = 1 << 19, SP_WL = 1 << 20, SP_WR = 1 << 21, SP_LFE2 = 1 << 22,
};

struct Ac3Header {
    bool eac3 = false, dependent = false;
    int bsid = 0, rate = 0, samples = 1536, lfeon = 0;
    size_t bytes = 0;
    uint64_t layout = 0;
    int dialnorm_pos = -1, dialnorm2_pos = -1;      // bit positions of dialnorm (and dialnorm2: dual mono)
    unsigned dialnorm = 0, dialnorm2 = 0;
};

bool ac3_header(const uint8_t *p, size_t n, Ac3Header &h)
{
    static const uint64_t acmod_layout[8] = {
        SP_FL | SP_FR, SP_FC, SP_FL | SP_FR, SP_FL | SP_FR | SP_FC, SP_FL | SP_FR | SP_BC,
        SP_FL | SP_FR | SP_FC | SP_BC, SP_FL | SP_FR | SP_SL | SP_SR, SP_FL | SP_FR | SP_FC | SP_SL | SP_SR,
    };
    static const uint64_t custom_layout[16] = {
        SP_FL, SP_FC, SP_FR, SP_SL, SP_SR, SP_FLC | SP_FRC, SP_BL | SP_BR, SP_BC, SP_TC, SP_SDL | SP_SDR,
        SP_WL | SP_WR, SP_TFL | SP_TFR, SP_TFC, SP_TBL | SP_TBR, SP_LFE2, SP_LFE,
    };
    static const uint16_t frame_words[38][3] = {
        { 64, 69, 96 }, { 64, 70, 96 }, { 80, 87, 120 }, { 80, 88, 120 }, { 96, 104, 144 }, { 96, 105, 144 },
        { 112, 121, 168 }, { 112, 122, 168 }, { 128, 139, 192 }, { 128, 140, 192 }, { 160, 174, 240 },
        { 160, 175, 240 }, { 192, 208, 288 }, { 192, 209, 288 }, { 224, 243, 336 }, { 224, 244, 336 },
        { 256, 278, 384 }, { 256, 279, 384 }, { 320, 348, 480 }, { 320, 349, 480 }, { 384, 417, 576 },
        { 384, 418, 576 }, { 448, 487, 672 }, { 448, 488, 672 }, { 512, 557, 768 }, { 512, 558, 768 },
        { 640, 696, 960 }, { 640, 697, 960 }, { 768, 835, 1152 }, { 768, 836, 1152 }, { 896, 975, 1344 },
        { 896, 976, 1344 }, { 1024, 1114, 1536 }, { 1024, 1115, 1536 }, { 1152, 1253, 1728 },
        { 1152, 1254, 1728 }, { 1280, 1393, 1920 }, { 1280, 1394, 1920 },
    };
    if (n < 18 || p[0] != 0x0B || p[1] != 0x77)
        return false;
    Bits r(p, 18);
    r.b.assign(p, p + 18);  // A/52 has no emulation prevention
    r.pos = 40;
    h = Ac3Header();
    h.bsid = (int)r.u(5);
    r.pos = 16;
    if (h.bsid <= 8) {                                    // AC-3
        static const int rates[3] = { 48000, 44100, 32000 };
        r.u(16);
        unsigned fscod = r.u(2), frmsizecod = r.u(6);
        if (fscod == 3 || frmsizecod >= 38)
            return false;
        r.u(5);
        r.u(3);
        unsigned acmod = r.u(3);
        if ((acmod & 1) && acmod != 1)
            r.u(2);
        if (acmod & 4)
            r.u(2);
        if (acmod == 2)
            r.u(2);
        h.lfeon = (int)r.u(1);
        h.dialnorm_pos = (int)r.pos;
        h.dialnorm = r.u(5);
        if (r.u(1))         // compre
            r.u(8);
        if (r.u(1))         // langcode
            r.u(8);
        if (r.u(1))         // audprodie
            r.u(7);
        if (acmod == 0) {   // dual mono
            h.dialnorm2_pos = (int)r.pos;
            h.dialnorm2 = r.u(5);
        }
        h.rate = rates[fscod] >> (std::max(h.bsid, 8) - 8);
        h.bytes = (size_t)frame_words[frmsizecod][fscod] * 2;
        h.layout = acmod_layout[acmod];
        return true;
    }
    if (h.bsid > 16 || h.bsid <= 10)
        return false;
    static const int rates[3] = { 48000, 44100, 32000 }, rates2[3] = { 24000, 22050, 16000 };
    static const int samples[4] = { 256, 512, 768, 1536 };
    unsigned type = r.u(2);
    if (type == 3)
        return false;
    r.u(3);
    h.bytes = (size_t)(r.u(11) + 1) * 2;
    unsigned fscod = r.u(2), fscod2 = r.u(2);
    if (fscod == 3 && fscod2 == 3)
        return false;
    unsigned acmod = r.u(3);
    h.lfeon = (int)r.u(1);
    r.u(5);
    h.dialnorm_pos = (int)r.pos;
    h.dialnorm = r.u(5);
    if (r.u(1))
        r.u(8);
    if (acmod == 0) {
        h.dialnorm2_pos = (int)r.pos;
        h.dialnorm2 = r.u(5);
        if (r.u(1))
            r.u(8);
    }
    h.eac3 = true;
    h.dependent = type == 1;
    if (h.dependent && r.u(1)) {
        unsigned chanmap = r.u(16);
        for (int i = 0; i < 16; i++)
            if (chanmap & (1u << (15 - i)))
                h.layout |= custom_layout[i];
    } else
        h.layout = acmod_layout[acmod];
    h.rate = fscod == 3 ? rates2[fscod2] : rates[fscod];
    h.samples = fscod == 3 ? 1536 : samples[fscod2];
    return true;
}

int ac3_channels(uint64_t layout, int lfe)
{
    return __builtin_popcountll(layout) + lfe;
}

void ac3_flush_pending(MkvWriter *w, MkvTrackState &t)
{
    if (!t.pending)
        return;
    PacketPtr p = t.pending;
    t.pending.reset();
    if (!t.header_ready) {                  // the first packet: its channels with its dependent frames'
        t.spec.channels = ac3_channels(t.ac3_layout, t.ac3_lfe);
        t.default_duration = (int64_t)t.pending_samples * 1000000000LL / t.pending_rate;
        t.header_ready = true;
    }
    counted_packet(w, t, p, t.pending_time, t.pending_samples, t.pending_rate,
                   (int64_t)t.pending_samples * 1000000000LL / t.pending_rate);
}

// A packet per independent frame with the dependent frames after it (as mkvmerge's AC-3 parser joins them);
// the first packet gives the track's codec, rate and channels.
// ---- dialogue normalization removed (as mkvmerge's --remove-dialog-normalization-gain) ----

void put_bits(uint8_t *p, int pos, int n, unsigned v)
{
    for (int i = n - 1; i >= 0; i--, pos++) {
        uint8_t bit = 0x80 >> (pos & 7);
        p[pos >> 3] = (v >> i) & 1 ? p[pos >> 3] | bit : p[pos >> 3] & ~bit;
    }
}

void put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

unsigned bswap16(unsigned v)
{
    return (v >> 8 & 0xff) | (v & 0xff) << 8;
}

// A/52 crc1 for a changed first 5/8 of a frame (as the A/52 encoder computes it: the CRC of the span after
// crc1, then multiplied by x^-(span) so the whole span checks to 0)
const unsigned CRC16_POLY = (1 << 0) | (1 << 2) | (1 << 15) | (1 << 16);

unsigned mul_poly(unsigned a, unsigned b, unsigned poly)
{
    unsigned c = 0;
    while (a) {
        if (a & 1)
            c ^= b;
        a >>= 1;
        b <<= 1;
        if (b & (1 << 16))
            b ^= poly;
    }
    return c;
}

unsigned pow_poly(unsigned a, unsigned n, unsigned poly)
{
    unsigned r = 1;
    while (n) {
        if (n & 1)
            r = mul_poly(r, a, poly);
        a = mul_poly(a, a, poly);
        n >>= 1;
    }
    return r;
}

// one AC-3 / E-AC-3 frame: dialnorm (and dialnorm2) set to 31 (-31 dB: no reduction), then its CRCs
void ac3_remove_dialnorm(uint8_t *f, const Ac3Header &h)
{
    if (h.dialnorm_pos < 0 || (h.dialnorm == 31 && (h.dialnorm2_pos < 0 || h.dialnorm2 == 31)))
        return;
    put_bits(f, h.dialnorm_pos, 5, 31);
    if (h.dialnorm2_pos >= 0)
        put_bits(f, h.dialnorm2_pos, 5, 31);
    if (!h.eac3) {                      // E-AC-3 has no crc1
        int words = (int)(h.bytes >> 1), words58 = (words >> 1) + (words >> 3);
        unsigned crc1 = bswap16(dr_crc16_ansi(0, f + 4, 2 * words58 - 4));
        unsigned inv = pow_poly(CRC16_POLY >> 1, 16 * words58 - 16, CRC16_POLY);
        put_be16(f + 2, mul_poly(inv, crc1, CRC16_POLY));
    }
    // crc2; crcrsv set when the CRC would read as a sync word
    uint8_t &crcrsv = f[h.bytes - 3];
    crcrsv &= 0xfe;
    unsigned crc2 = bswap16(dr_crc16_ansi(0, f + 2, h.bytes - 4));
    if (crc2 == 0x0B77) {
        crcrsv |= 0x01;
        crc2 = bswap16(dr_crc16_ansi(0, f + 2, h.bytes - 4));
    }
    put_be16(f + h.bytes - 2, crc2);
}

int ac3_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time)
{
    size_t off = 0;
    while (off + 18 <= (size_t)size) {
        Ac3Header h;
        if (!ac3_header(data + off, size - off, h) || off + h.bytes > (size_t)size || h.rate <= 0)
            break;
        if (!h.dependent || !t.pending) {
            ac3_flush_pending(w, t);
            t.pending = std::make_shared<Packet>();
            t.pending->track = (int)(&t - w->tracks.data());
            t.pending_time = off ? INT64_MIN : time;
            t.pending_samples = h.samples;
            t.pending_rate = h.rate;
            if (t.codec_id.empty()) {
                t.codec_id = h.eac3 ? "A_EAC3" : "A_AC3";
                t.spec.sample_rate = h.rate;
                t.ac3_layout = h.layout;
                t.ac3_lfe = h.lfeon;
            }
        } else if (!t.header_ready) {
            t.ac3_layout |= h.layout;
            t.ac3_lfe += h.lfeon;
        }
        t.pending->data.insert(t.pending->data.end(), data + off, data + off + h.bytes);
        if (t.spec.remove_dialnorm)
            ac3_remove_dialnorm(t.pending->data.data() + t.pending->data.size() - h.bytes, h);
        off += h.bytes;
    }
    return 0;
}

// ---- DTS (ETSI TS 102 114; as mkvmerge reads the header) ----

struct DtsAsset {
    int channels = 0, max_rate = 0;
    unsigned mask = 0;
    size_t offset = 0, size = 0;
    size_t core_offset = 0, core_size = 0, xbr_offset = 0, xbr_size = 0, xxch_offset = 0, xxch_size = 0;
    size_t x96_offset = 0, x96_size = 0, lbr_offset = 0, lbr_size = 0, xll_offset = 0, xll_size = 0;
    bool lbr_sync = false, xll_sync = false, embedded_stereo = false, embedded_6ch = false;
    size_t xll_sync_offset = 0;
};

struct DtsHeader {
    bool has_core = false, has_exss = false, has_xch = false, termination = false;
    int core_rate = 0, channels = 0, lfe = 0, pcm_res = 0, blocks = 0, deficit = 0;
    int ext_rate = 0, ref_clock = 3, subframe_duration = 0;
    size_t frame_size = 0;
    std::vector<DtsAsset> assets;
    // dialogue normalization: the core's (4 bits; its level by the encoder version) and the extension
    // substream's (5 bits, the last asset's; bit position from the substream's start, byte position)
    int core_dialnorm_pos = -1, core_dialnorm_db = 0;
    int ext_dialnorm_pos = -1;
    unsigned ext_dialnorm = 0;
    size_t exss_offset = 0, exss_header_size = 0;
    // exss header
    int substream_size_bits = 16;
    bool static_fields = false, mix_metadata = false;
    unsigned nb_mix = 0, mix_channels[4] = { 0 };
};

int dts_count_channels(unsigned mask)
{
    return __builtin_popcount(mask) + __builtin_popcount(mask & 0xae66);
}

bool dts_set_offset(const DtsAsset &, unsigned has, unsigned want, size_t &off, size_t &size, size_t &out,
                    size_t in_size)
{
    if (!(has & want))
        return true;
    if ((off & 3) || in_size > size)
        return false;
    out = off;
    off += in_size;
    size -= in_size;
    return true;
}

bool dts_asset(Bits &r, DtsHeader &h, DtsAsset &a)
{
    static const int sub_rates[16] = { 8000, 16000, 32000, 64000, 128000, 22050, 44100, 88200,
                                       176400, 352800, 12000, 24000, 48000, 96000, 192000, 384000 };
    size_t start = r.pos;
    unsigned size = r.u(9) + 1;
    r.u(3);
    if (h.static_fields) {
        if (r.u(1))
            r.u(4);
        if (r.u(1))
            r.u(24);
        if (r.u(1))
            r.pos += (size_t)(r.u(10) + 1) * 8;
        r.u(5);
        a.max_rate = sub_rates[r.u(4)];
        a.channels = (int)r.u(8) + 1;
        if (r.u(1)) {
            if (a.channels > 2)
                a.embedded_stereo = r.u(1);
            if (a.channels > 6)
                a.embedded_6ch = r.u(1);
            int mask_bits = 16;
            if (r.u(1)) {
                mask_bits = (int)(r.u(2) + 1) << 2;
                r.pos += mask_bits;
            }
            unsigned nsets = r.u(3), nspk[8];
            for (unsigned i = 0; i < nsets; i++)
                nspk[i] = dts_count_channels(r.u(mask_bits));
            for (unsigned i = 0; i < nsets; i++) {
                unsigned nch = r.u(5) + 1;
                for (unsigned k = 0; k < nspk[i]; k++)
                    r.pos += (size_t)__builtin_popcount(r.u(nch)) * 5;
            }
        } else
            r.u(3);
    }
    bool drc = r.u(1);
    if (drc)
        r.u(8);
    if (r.u(1)) {
        h.ext_dialnorm_pos = (int)r.pos;
        h.ext_dialnorm = r.u(5);
    }
    if (drc && a.embedded_stereo)
        r.u(8);
    if (h.mix_metadata && r.u(1)) {
        r.u(1);
        r.u(6);
        if (r.u(2) == 3)
            r.u(8);
        else
            r.u(3);
        if (r.u(1))
            for (unsigned m = 0; m < h.nb_mix; m++)
                r.pos += 6 * (size_t)h.mix_channels[m];
        else
            r.pos += 6 * (size_t)h.nb_mix;
        unsigned nch = a.channels + (a.embedded_6ch ? 6 : 0) + (a.embedded_stereo ? 2 : 0);
        for (unsigned m = 0; m < h.nb_mix; m++)
            for (unsigned c = 0; c < nch; c++)
                r.pos += 6 * (size_t)__builtin_popcount(r.u(h.mix_channels[m]));
    }
    auto lbr = [&]() {
        a.lbr_size = r.u(14) + 1;
        a.lbr_sync = r.u(1);
        if (a.lbr_sync)
            r.u(2);
    };
    auto xll = [&]() {
        a.xll_size = r.u(h.substream_size_bits) + 1;
        a.xll_sync = r.u(1);
        if (a.xll_sync) {
            r.u(4);
            unsigned bits = r.u(5) + 1;
            r.u(bits);
            a.xll_sync_offset = r.u(h.substream_size_bits);
        }
    };
    switch (r.u(2)) {
    case 0:
        a.mask = r.u(12);
        if (a.mask & 0x010) {
            a.core_size = r.u(14) + 1;
            if (r.u(1))
                r.u(2);
        }
        if (a.mask & 0x020)
            a.xbr_size = r.u(14) + 1;
        if (a.mask & 0x040)
            a.xxch_size = r.u(14) + 1;
        if (a.mask & 0x080)
            a.x96_size = r.u(12) + 1;
        if (a.mask & 0x100)
            lbr();
        if (a.mask & 0x200)
            xll();
        if (a.mask & 0x400)
            r.u(16);
        if (a.mask & 0x800)
            r.u(16);
        break;
    case 1:
        a.mask = 0x200;
        xll();
        break;
    case 2:
        a.mask = 0x100;
        lbr();
        break;
    case 3:
        a.mask = 0;
        r.u(14);
        r.u(8);
        if (r.u(1))
            r.u(3);
        break;
    }
    if (a.mask & 0x200)
        r.u(3);
    size_t off = a.offset, left = a.size;
    if (!dts_set_offset(a, a.mask, 0x010, off, left, a.core_offset, a.core_size) ||
        !dts_set_offset(a, a.mask, 0x020, off, left, a.xbr_offset, a.xbr_size) ||
        !dts_set_offset(a, a.mask, 0x040, off, left, a.xxch_offset, a.xxch_size) ||
        !dts_set_offset(a, a.mask, 0x080, off, left, a.x96_offset, a.x96_size) ||
        !dts_set_offset(a, a.mask, 0x100, off, left, a.lbr_offset, a.lbr_size) ||
        !dts_set_offset(a, a.mask, 0x200, off, left, a.xll_offset, a.xll_size))
        return false;
    if (a.mask & 0x100) {
        static const int lbr_rates[16] = { 8000, 16000, 32000, 0, 0, 22050, 44100, 0,
                                           0, 0, 12000, 24000, 48000, 0, 0, 0 };
        r.pos = a.lbr_offset * 8;
        if (a.lbr_sync && r.u(32) != 0x0a801921)
            return false;
        unsigned code = r.u(8);
        if (code == 2)
            h.core_rate = lbr_rates[r.u(8) & 0x0f];
        else if (code != 1)
            return false;
    }
    if (a.mask & 0x200) {
        r.pos = (a.xll_offset + a.xll_sync_offset) * 8;
        if (a.xll_sync && r.u(32) != 0x41a29547)
            return false;
        if (!(a.mask & 0x100)) {
            if (!h.has_core)
                h.core_rate = h.assets[0].max_rate;
            else
                h.ext_rate = h.assets[0].max_rate;
        }
    } else if (a.mask & 0x080)
        h.ext_rate = 96000;
    r.pos = start + (size_t)size * 8;
    return true;
}

bool dts_exss(const uint8_t *p, size_t n, DtsHeader &h)
{
    Bits r(p, n);
    r.b.assign(p, p + n);   // the extension substream has no emulation prevention
    r.u(32);
    r.u(8);
    unsigned index = r.u(2);
    int header_bits = 8;
    h.substream_size_bits = 16;
    if (r.u(1)) {
        header_bits = 12;
        h.substream_size_bits = 20;
    }
    size_t header_size = r.u(header_bits) + 1, part = r.u(h.substream_size_bits) + 1;
    h.exss_header_size = header_size;
    h.frame_size += part;
    h.has_exss = true;
    unsigned npres = 1, nassets = 1;
    h.static_fields = r.u(1);
    if (h.static_fields) {
        h.ref_clock = (int)r.u(2);
        h.subframe_duration = (int)(r.u(3) + 1) * 512;
        if (r.u(1))
            r.pos += 36;
        npres = r.u(3) + 1;
        nassets = r.u(3) + 1;
        unsigned active[8];
        for (unsigned i = 0; i < npres; i++)
            active[i] = r.u(index + 1);
        for (unsigned i = 0; i < npres; i++)
            for (unsigned k = 0; k <= index; k++)
                if (active[i] & (1u << k))
                    r.u(8);
        h.mix_metadata = r.u(1);
        if (h.mix_metadata) {
            r.u(2);
            int bits = (int)(r.u(2) + 1) << 2;
            h.nb_mix = r.u(2) + 1;
            for (unsigned m = 0; m < h.nb_mix; m++)
                h.mix_channels[m] = dts_count_channels(r.u(bits));
        }
    }
    h.assets.resize(nassets);
    size_t off = header_size;
    for (auto &a : h.assets) {
        a.offset = off;
        a.size = r.u(h.substream_size_bits) + 1;
        off += a.size;
    }
    for (auto &a : h.assets)
        if (!dts_asset(r, h, a))
            return false;
    return true;
}

bool dts_header(const uint8_t *p, size_t n, DtsHeader &h)
{
    static const int core_rates[16] = { -1, 8000, 16000, 32000, -1, -1, 11025, 22050,
                                        44100, -1, -1, 12000, 24000, 48000, -1, -1 };
    static const int amode_channels[16] = { 1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8 };
    h = DtsHeader();
    if (n < 15)
        return false;
    uint32_t sync = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    if (sync == 0x64582025)
        return dts_exss(p, n, h);
    if (sync != 0x7ffe8001)
        return false;
    Bits r(p, n);
    r.b.assign(p, p + std::min<size_t>(n, 32));
    r.u(32);
    h.termination = !r.u(1);
    h.deficit = (int)((r.u(5) + 1) % 32);
    bool crc = r.u(1);
    h.blocks = (int)r.u(7) + 1;
    h.frame_size = r.u(14) + 1;
    if (h.frame_size < 96)
        return false;
    unsigned amode = r.u(6);
    h.channels = amode < 16 ? amode_channels[amode] : -1;
    h.core_rate = core_rates[r.u(4)];
    r.u(5);
    r.u(5);
    unsigned ext_audio = r.u(3);
    bool ext_coding = r.u(1);
    r.u(1);
    h.lfe = (int)r.u(2);
    r.u(1);
    if (crc)
        r.u(16);
    r.u(1);
    unsigned version = r.u(4);
    r.u(2);
    static const int pcm[8] = { 16, 16, 20, 20, 0, 24, 24, 0 };
    h.pcm_res = pcm[r.u(3)];
    if (!h.pcm_res)
        return false;
    r.u(1);
    r.u(1);
    h.core_dialnorm_pos = (int)r.pos;
    unsigned dn = r.u(4);
    h.core_dialnorm_db = version == 7 ? -(int)dn : version == 6 ? -16 - (int)dn : 0;
    h.has_core = true;
    // the 6.1 channel extension in the core frame (its sync searched from the frame's end)
    if (ext_coding && ext_audio == 0 && h.frame_size <= n) {
        for (size_t pos = h.frame_size - 96; pos > 0; pos--) {
            if (p[pos] == 0x5a && p[pos + 1] == 0x5a && p[pos + 2] == 0x5a && p[pos + 3] == 0x5a &&
                pos + ((((size_t)p[pos + 4] << 2) | (p[pos + 5] >> 6)) + 1) == h.frame_size) {
                h.has_xch = true;
                break;
            }
        }
    }
    size_t core = h.frame_size;
    if (core + 9 <= n && p[core] == 0x64 && p[core + 1] == 0x58 && p[core + 2] == 0x20 && p[core + 3] == 0x25) {
        h.exss_offset = core;
        return dts_exss(p + core, n - core, h);
    }
    return true;
}

int64_t dts_samples(const DtsHeader &h);
int64_t dts_ns(const DtsHeader &h)
{
    static const int clocks[3] = { 32000, 44100, 48000 };
    if (h.has_core)
        return (dts_samples(h) * 10000000000LL / h.core_rate + 5) / 10;
    if (h.ref_clock < 3)
        return ((int64_t)h.subframe_duration * 10000000000LL / clocks[h.ref_clock] + 5) / 10;
    return 0;
}
int64_t dts_samples(const DtsHeader &h)
{
    if (h.has_core) {
        int64_t s = (int64_t)h.blocks * 32;
        if (h.termination)
            s -= std::min<int64_t>(s, h.deficit);
        return s;
    }
    int64_t ns = dts_ns(h);
    return h.core_rate > 0 ? (ns * h.core_rate / 100000000 + 5) / 10 : 0;
}
int dts_total_channels(const DtsHeader &h)
{
    if (h.has_exss && !h.assets.empty() && h.assets[0].channels)
        return h.assets[0].channels;
    return h.channels + (h.lfe == 1 || h.lfe == 2 ? 1 : 0) + (h.has_xch ? 1 : 0);
}

// one DTS frame: the core's dialogue normalization set to 0 (its header CRC left as it is) and the extension
// substream's set to 0 (its header CRC made again), as mkvmerge does
void dts_remove_dialnorm(uint8_t *f, const DtsHeader &h)
{
    if (h.has_core && h.core_dialnorm_pos >= 0 && h.core_dialnorm_db != 0)
        put_bits(f, h.core_dialnorm_pos, 4, 0);
    if (h.has_exss && h.ext_dialnorm_pos >= 0 && h.ext_dialnorm != 0 && h.exss_header_size >= 8) {
        uint8_t *x = f + h.exss_offset;
        put_bits(x, h.ext_dialnorm_pos, 5, 0);
        put_be16(x + h.exss_header_size - 2, bswap16(dr_crc16_ccitt(0xffff, x + 5, h.exss_header_size - 5 - 2)));
    }
}

int dts_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time)
{
    size_t off = 0;
    while (off + 15 <= (size_t)size) {
        DtsHeader h;
        if (!dts_header(data + off, size - off, h) || !h.frame_size || off + h.frame_size > (size_t)size)
            break;
        if (!t.header_ready) {
            t.codec_id = "A_DTS";
            t.spec.sample_rate = h.ext_rate ? h.ext_rate : h.core_rate;
            t.spec.channels = dts_total_channels(h);
            t.spec.bits = h.pcm_res;
            t.default_duration = dts_ns(h);
            t.header_ready = true;
        }
        auto p = std::make_shared<Packet>();
        p->track = (int)(&t - w->tracks.data());
        p->data.assign(data + off, data + off + h.frame_size);
        if (t.spec.remove_dialnorm)
            dts_remove_dialnorm(p->data.data(), h);
        if (h.core_rate <= 0)
            return fail(w, EINVAL);
        counted_packet(w, t, p, off ? INT64_MIN : time, (int)dts_samples(h), h.core_rate, dts_ns(h));
        off += h.frame_size;
    }
    return 0;
}

void pcm_flush(MkvWriter *w, MkvTrackState &t, bool all)
{
    int track = (int)(&t - w->tracks.data());
    auto make = [&](size_t bytes) {
        auto p = std::make_shared<Packet>();
        p->track = track;
        int64_t samples = (int64_t)(bytes / t.frame_bytes);
        p->timestamp = p->assigned = t.samples_out * 1000000000LL / t.spec.sample_rate;
        p->duration = samples * 1000000000LL / t.spec.sample_rate;
        p->data.assign(t.pcm.begin(), t.pcm.begin() + bytes);
        t.pcm.erase(t.pcm.begin(), t.pcm.begin() + bytes);
        t.samples_out += samples;
        t.queue.push_back(p);
    };
    while ((int)t.pcm.size() >= t.packet_size)
        make(t.packet_size);
    if (all && !t.pcm.empty())
        make(t.pcm.size());
}

int pcm_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time)
{
    if (!t.pcm_started) {
        t.pcm_started = true;
        t.samples_out = time >= 0 ? time * t.spec.sample_rate / 1000000000LL : 0;
    } else if (time >= 0) {
        int64_t buffered = (int64_t)t.pcm.size() / t.frame_bytes;
        int64_t expected = (t.samples_out + buffered) * 1000000000LL / t.spec.sample_rate;
        if (std::llabs(time - expected) > 1000000) {
            // a gap (or an offset): what is buffered goes out, the count starts again at this frame's time
            pcm_flush(w, t, true);
            t.samples_out = time * t.spec.sample_rate / 1000000000LL;
        }
    }
    t.pcm.insert(t.pcm.end(), data, data + size);
    pcm_flush(w, t, false);
    return pump(w, false);
}

// ISO 639-2: a terminology code's bibliographic form (the twenty that differ), which the legacy Language
// element takes (as mkvmerge writes it); every other code is both
std::string bibliographic(const std::string &code)
{
    static const char *const pairs[][2] = {
        { "sqi", "alb" }, { "hye", "arm" }, { "eus", "baq" }, { "mya", "bur" }, { "zho", "chi" }, { "ces", "cze" },
        { "nld", "dut" }, { "fra", "fre" }, { "kat", "geo" }, { "deu", "ger" }, { "ell", "gre" }, { "isl", "ice" },
        { "mkd", "mac" }, { "mri", "mao" }, { "msa", "may" }, { "fas", "per" }, { "ron", "rum" }, { "slk", "slo" },
        { "bod", "tib" }, { "cym", "wel" },
    };
    for (auto &p : pairs)
        if (code == p[0])
            return p[1];
    return code;
}

std::string bcp47_of(const char *code)
{
    // ISO 639-2 bibliographic code -> its ISO 639-1 code where one exists (BCP 47 takes the shortest)
    static const char *const pairs[][2] = {
        { "aar", "aa" }, { "abk", "ab" }, { "afr", "af" }, { "aka", "ak" }, { "alb", "sq" }, { "amh", "am" },
        { "ara", "ar" }, { "arg", "an" }, { "arm", "hy" }, { "asm", "as" }, { "ava", "av" }, { "ave", "ae" },
        { "aym", "ay" }, { "aze", "az" }, { "bak", "ba" }, { "bam", "bm" }, { "baq", "eu" }, { "bel", "be" },
        { "ben", "bn" }, { "bis", "bi" }, { "bos", "bs" }, { "bre", "br" }, { "bul", "bg" }, { "bur", "my" },
        { "cat", "ca" }, { "cha", "ch" }, { "che", "ce" }, { "chi", "zh" }, { "chu", "cu" }, { "chv", "cv" },
        { "cor", "kw" }, { "cos", "co" }, { "cre", "cr" }, { "cze", "cs" }, { "dan", "da" }, { "div", "dv" },
        { "dut", "nl" }, { "dzo", "dz" }, { "eng", "en" }, { "epo", "eo" }, { "est", "et" }, { "ewe", "ee" },
        { "fao", "fo" }, { "fij", "fj" }, { "fin", "fi" }, { "fre", "fr" }, { "fry", "fy" }, { "ful", "ff" },
        { "geo", "ka" }, { "ger", "de" }, { "gla", "gd" }, { "gle", "ga" }, { "glg", "gl" }, { "glv", "gv" },
        { "gre", "el" }, { "grn", "gn" }, { "guj", "gu" }, { "hat", "ht" }, { "hau", "ha" }, { "heb", "he" },
        { "her", "hz" }, { "hin", "hi" }, { "hmo", "ho" }, { "hrv", "hr" }, { "hun", "hu" }, { "ibo", "ig" },
        { "ice", "is" }, { "ido", "io" }, { "iii", "ii" }, { "iku", "iu" }, { "ile", "ie" }, { "ina", "ia" },
        { "ind", "id" }, { "ipk", "ik" }, { "ita", "it" }, { "jav", "jv" }, { "jpn", "ja" }, { "kal", "kl" },
        { "kan", "kn" }, { "kas", "ks" }, { "kau", "kr" }, { "kaz", "kk" }, { "khm", "km" }, { "kik", "ki" },
        { "kin", "rw" }, { "kir", "ky" }, { "kom", "kv" }, { "kon", "kg" }, { "kor", "ko" }, { "kua", "kj" },
        { "kur", "ku" }, { "lao", "lo" }, { "lat", "la" }, { "lav", "lv" }, { "lim", "li" }, { "lin", "ln" },
        { "lit", "lt" }, { "ltz", "lb" }, { "lub", "lu" }, { "lug", "lg" }, { "mac", "mk" }, { "mah", "mh" },
        { "mal", "ml" }, { "mao", "mi" }, { "mar", "mr" }, { "may", "ms" }, { "mlg", "mg" }, { "mlt", "mt" },
        { "mon", "mn" }, { "nau", "na" }, { "nav", "nv" }, { "nbl", "nr" }, { "nde", "nd" }, { "ndo", "ng" },
        { "nep", "ne" }, { "nno", "nn" }, { "nob", "nb" }, { "nor", "no" }, { "nya", "ny" }, { "oci", "oc" },
        { "oji", "oj" }, { "ori", "or" }, { "orm", "om" }, { "oss", "os" }, { "pan", "pa" }, { "per", "fa" },
        { "pli", "pi" }, { "pol", "pl" }, { "por", "pt" }, { "pus", "ps" }, { "que", "qu" }, { "roh", "rm" },
        { "rum", "ro" }, { "run", "rn" }, { "rus", "ru" }, { "sag", "sg" }, { "san", "sa" }, { "sin", "si" },
        { "slo", "sk" }, { "slv", "sl" }, { "sme", "se" }, { "smo", "sm" }, { "sna", "sn" }, { "snd", "sd" },
        { "som", "so" }, { "sot", "st" }, { "spa", "es" }, { "srd", "sc" }, { "srp", "sr" }, { "ssw", "ss" },
        { "sun", "su" }, { "swa", "sw" }, { "swe", "sv" }, { "tah", "ty" }, { "tam", "ta" }, { "tat", "tt" },
        { "tel", "te" }, { "tgk", "tg" }, { "tgl", "tl" }, { "tha", "th" }, { "tib", "bo" }, { "tir", "ti" },
        { "ton", "to" }, { "tsn", "tn" }, { "tso", "ts" }, { "tuk", "tk" }, { "tur", "tr" }, { "twi", "tw" },
        { "uig", "ug" }, { "ukr", "uk" }, { "urd", "ur" }, { "uzb", "uz" }, { "ven", "ve" }, { "vie", "vi" },
        { "vol", "vo" }, { "wel", "cy" }, { "wln", "wa" }, { "wol", "wo" }, { "xho", "xh" }, { "yid", "yi" },
        { "yor", "yo" }, { "zha", "za" }, { "zul", "zu" },
    };
    for (auto &p : pairs)
        if (!strcmp(p[0], code))
            return p[1];
    return code;
}

void write_cues(MkvWriter *w)
{
    // mkvmerge's cues: sorted by time, then track; the Cues head written with the size the points take
    auto &pts = w->cue_points;
    if (pts.empty())
        return;
    std::stable_sort(pts.begin(), pts.end(), [](const CuePoint &a, const CuePoint &b) {
        return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.track < b.track;
    });
    // its place for the SeekHead
    uint64 here = w->out->getFilePointer();
    {
        KaxSeek &e = AddNewChild<KaxSeek>(w->seek);
        const uint8_t id_cues[4] = { 0x1C, 0x53, 0xBB, 0x6B };
        GetChild<KaxSeekID>(e).CopyBuffer(id_cues, 4);
        GetChild<KaxSeekPosition>(e).SetValue(here - w->segment.GetDataStart());
    }
    auto bytes = [](uint64_t v) {
        for (int i = 1; i <= 7; i++)
            if (v < (1ULL << (i * 8)))
                return (uint64_t)i;
        return (uint64_t)8;
    };
    uint64_t total = 0;
    for (auto &p : pts) {
        uint64_t s = 1 + 1 + 1 + 1 + bytes(p.timestamp / SCALE) + 1 + 1 + 1 + 1 + bytes(p.track) + 1 + 1 +
                     bytes(p.cluster_position);
        if (p.relative_position)
            s += 1 + 1 + bytes(p.relative_position);
        total += s;
    }
    uint8_t head[12];
    int id = 4, n = CodedSizeLength(total, 0);
    head[0] = 0x1C; head[1] = 0x53; head[2] = 0xBB; head[3] = 0x6B;
    CodedValueLength(total, n, head + id);
    w->out->writeFully(head, id + n);
    for (auto &p : pts) {
        KaxCuePoint cp;
        GetChild<KaxCueTime>(cp).SetValue(p.timestamp / SCALE);
        KaxCueTrackPositions &tp = GetChild<KaxCueTrackPositions>(cp);
        GetChild<KaxCueTrack>(tp).SetValue(p.track);
        GetChild<KaxCueClusterPosition>(tp).SetValue(p.cluster_position);
        if (p.relative_position)
            GetChild<KaxCueRelativePosition>(tp).SetValue(p.relative_position);
        cp.Render(*w->out);
    }
    w->st.cues = (int64_t)pts.size();
}

// The tracks that got no frame (a forced-only subtitle track without forced subtitles) leave the track
// list: it is written again in its place without them (the room after it grows; mkvmerge writes its
// track list again the same way when a header changes), the other tracks keeping their numbers (their
// blocks carry them). No block, cue point or tag names such a track.
void remove_empty_tracks(MkvWriter *w)
{
    bool any = false;
    for (auto &t : w->tracks)
        any |= !t.stats.frames;
    if (!any || !w->ktracks)
        return;
    uint64 start = w->ktracks->GetElementPosition();
    uint64 end = w->tracks_void.GetElementPosition() + w->tracks_void.ElementSize();
    for (size_t k = 0; k < w->tracks.size(); k++) {
        MkvTrackState &t = w->tracks[k];
        if (t.stats.frames || !t.entry)
            continue;
        for (unsigned i = 0; i < w->ktracks->ListSize(); i++)
            if ((*w->ktracks)[i] == t.entry) {
                delete t.entry;
                w->ktracks->Remove(i);
                break;
            }
        t.entry = nullptr;
        if (w->st.nb_removed < (int)(sizeof(w->st.removed) / sizeof(*w->st.removed)))
            w->st.removed[w->st.nb_removed] = (int)k;
        w->st.nb_removed++;
    }
    uint64 here = w->out->getFilePointer();
    w->ktracks->UpdateSize(false);
    w->out->setFilePointer(start);
    w->ktracks->Render(*w->out, false);
    EbmlVoid room;
    render_void(w, room, (int64_t)(end - w->out->getFilePointer()));
    w->out->setFilePointer(here);
}

void write_tags(MkvWriter *w)
{
    KaxTags tags;
    tags.RemoveAll();
    char date[32];
    time_t d = (time_t)w->spec.date;
    struct tm tm;
    gmtime_r(&d, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);
    for (auto &t : w->tracks) {
        if (!t.entry)
            continue;               // taken out of the track list (no frames)
        KaxTag &tag = AddNewChild<KaxTag>(tags);
        while (tag.ListSize()) {
            delete tag[0];
            tag.Remove(0);
        }
        KaxTagTargets &tg = GetChild<KaxTagTargets>(tag);
        while (tg.ListSize()) {
            delete tg[0];
            tg.Remove(0);
        }
        GetChild<KaxTagTrackUID>(tg).SetValue(t.uid);
        int64_t dur = t.stats.frames ? t.stats.max_end - t.stats.min_ts : 0;
        int64_t bps = t.stats.frames && dur > 1000000 ? (int64_t)(t.stats.bytes * 8000) / (dur / 1000000) : 0;
        auto simple = [&](const char *name, const std::string &value) {
            KaxTagSimple &s = AddNewChild<KaxTagSimple>(tag);
            while (s.ListSize()) {
                delete s[0];
                s.Remove(0);
            }
            GetChild<KaxTagName>(s).SetValueUTF8(name);
            GetChild<KaxTagString>(s).SetValueUTF8(value);
        };
        simple("BPS", std::to_string(bps));
        simple("DURATION", format_ts(dur));
        simple("NUMBER_OF_FRAMES", std::to_string(t.stats.frames));
        simple("NUMBER_OF_BYTES", std::to_string(t.stats.bytes));
        simple("_STATISTICS_WRITING_APP", w->writing_app);
        simple("_STATISTICS_WRITING_DATE_UTC", date);
        simple("_STATISTICS_TAGS", "BPS DURATION NUMBER_OF_FRAMES NUMBER_OF_BYTES");
    }
    tags.Render(*w->out, true);
    w->seek.IndexThis(tags, w->segment);
}

// ---- PGS: a display set per block (as mkvmerge joins a .sup file's segments up to each end segment) ----

int pgs_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time)
{
    auto p = std::make_shared<Packet>();
    p->track = (int)(&t - w->tracks.data());
    p->timestamp = p->assigned = time;
    p->key = 1;
    p->raw_size = (uint64_t)size;
    if (t.spec.zlib) {
        // as mkvmerge compresses a block: zlib, level 9, the whole block in one stream
        z_stream z{};
        if (deflateInit(&z, 9) != Z_OK)
            return fail(w, ENOMEM);
        p->data.resize(deflateBound(&z, (uLong)size));
        z.next_in = const_cast<Bytef *>(data);
        z.avail_in = (uInt)size;
        z.next_out = p->data.data();
        z.avail_out = (uInt)p->data.size();
        int r = deflate(&z, Z_FINISH);
        p->data.resize(z.total_out);
        deflateEnd(&z);
        if (r != Z_STREAM_END)
            return fail(w, EIO);
    } else
        p->data.assign(data, data + size);
    if (time < 0) {
        t.dropped_negative++;
        return 0;
    }
    if (time < w->last_out)
        w->st.late++;
    t.queue.push_back(p);
    return 0;
}

}  // namespace

// ---- the C interface ----

extern "C" int mkv_open(MkvWriter **pw, const char *path, const MkvFileSpec *file, const MkvTrackSpec *tracks,
                        int nb_tracks)
{
    MkvWriter *w = new (std::nothrow) MkvWriter;
    if (!w)
        return -ENOMEM;
    w->path = path;
    w->spec = *file;
    w->muxing_app = file->muxing_app ? file->muxing_app : "";
    w->writing_app = file->writing_app ? file->writing_app : "";
    w->uids.fixed = file->fixed_ids;
    for (int i = 0; i < file->nb_chapters; i++) {
        w->est_names.push_back(file->chapters[i].name ? file->chapters[i].name : "");
        w->est_chapters.push_back(file->chapters[i]);
    }
    for (size_t i = 0; i < w->est_chapters.size(); i++)
        w->est_chapters[i].name = w->est_names[i].c_str();
    w->tracks.resize(nb_tracks);
    for (int i = 0; i < nb_tracks; i++) {
        MkvTrackState &t = w->tracks[i];
        t.spec = tracks[i];
        t.number = i + 1;
        t.language = bibliographic(tracks[i].language && *tracks[i].language ? tracks[i].language : "und");
        t.language_ietf = tracks[i].language_ietf && *tracks[i].language_ietf ? tracks[i].language_ietf
                                                                              : bcp47_of(t.language.c_str());
        t.name = tracks[i].name ? tracks[i].name : "";
        t.uid = w->uids.make(UID_TRACK);
        if (t.spec.codec == MKV_CODEC_H264) {
            t.codec_id = "V_MPEG4/ISO/AVC";
            if (w->video < 0)
                w->video = i;
        } else if (t.spec.codec == MKV_CODEC_AC3 || t.spec.codec == MKV_CODEC_DTS) {
            // codec, rate, channels and default duration: from the first packet
        } else if (t.spec.codec == MKV_CODEC_PGS) {
            t.codec_id = "S_HDMV/PGS";
            t.header_ready = true;
            t.sparse = true;
        } else if (t.spec.codec == MKV_CODEC_PCM) {
            t.codec_id = "A_PCM/INT/LIT";
            if (t.spec.sample_rate <= 0 || t.spec.channels <= 0 || t.spec.bits <= 0) {
                delete w;
                return -EINVAL;
            }
            t.header_ready = true;
            t.frame_bytes = t.spec.channels * t.spec.bits / 8;
            t.samples_per_packet = t.spec.sample_rate / 25;
            t.packet_size = t.samples_per_packet * t.frame_bytes;
            t.default_duration = (int64_t)(1000000000.0 * t.samples_per_packet / t.spec.sample_rate);
        } else {
            delete w;
            return -EINVAL;
        }
    }
    *pw = w;
    return 0;
}

extern "C" int mkv_h264_pictures(const uint8_t *data, int size)
{
    // the start rule of h264_frame: an access unit delimiter or a slice with first_mb_in_slice 0 after a slice
    std::vector<std::pair<const uint8_t *, size_t>> nals;
    nal_units(data, size, nals);
    int pics = 0;
    bool slice_in_pic = false;
    for (auto &n : nals) {
        int type = n.first[0] & 31;
        bool slice = type == 1 || type == 5;
        if (slice_in_pic && (type == 9 || (slice && Bits(n.first + 1, n.second - 1).ue() == 0)))
            slice_in_pic = false;
        if (slice && !slice_in_pic) {
            pics++;
            slice_in_pic = true;
        }
    }
    return pics;
}

extern "C" int mkv_frame(MkvWriter *w, int track, const uint8_t *data, int size, int64_t time, int64_t duration,
                         int key)
{
    if (w->failed)
        return -w->error;
    if (track < 0 || track >= (int)w->tracks.size())
        return -EINVAL;
    MkvTrackState &t = w->tracks[track];
    try {
        int ret;
        switch (t.spec.codec) {
        case MKV_CODEC_H264: return h264_frame(w, t, data, size, time, duration, key);
        case MKV_CODEC_AC3:  ret = ac3_frame(w, t, data, size, time); break;
        case MKV_CODEC_DTS:  ret = dts_frame(w, t, data, size, time); break;
        case MKV_CODEC_PGS:  ret = pgs_frame(w, t, data, size, time); break;
        default:             return pcm_frame(w, t, data, size, time);
        }
        return ret < 0 ? ret : pump(w, false);
    } catch (...) {
        return fail(w, errno ? errno : EIO);
    }
}

extern "C" int mkv_close(MkvWriter **pw, const MkvChapter *chapters, int nb_chapters, int abort, MkvStats *st)
{
    MkvWriter *w = *pw;
    int ret = 0;
    if (!w)
        return 0;
    *pw = nullptr;
    try {
        if (!abort && !w->failed) {
            for (auto &t : w->tracks) {
                if (t.spec.codec == MKV_CODEC_H264 && !t.header_ready && !t.held.empty()) {
                    if (t.sps.empty() || t.pps.empty()) {
                        fail(w, EINVAL);
                        break;
                    }
                    h264_make_header(t);
                    for (auto &h : t.held)
                        t.queue.push_back(h);
                    t.held.clear();
                }
                if (t.spec.codec == MKV_CODEC_PCM)
                    pcm_flush(w, t, true);
                if (t.spec.codec == MKV_CODEC_AC3)
                    ac3_flush_pending(w, t);
                t.done = true;
            }
            if (!w->failed && pump(w, true) == 0 && !w->started)
                write_headers(w);
            if (!w->failed && w->cluster && !w->packets.empty())
                render_cluster(w);
            if (!w->failed) {
                write_cues(w);
                remove_empty_tracks(w);
                // the Duration: the file's last block end
                int64_t dur = w->max_ts_and_duration - (w->min_ts_in_file == INT64_MAX ? 0 : w->min_ts_in_file);
                uint64 end = w->out->getFilePointer();
                w->duration->SetValue((double)std::llround((double)dur / SCALE));
                w->out->setFilePointer(w->duration->GetElementPosition());
                w->duration->Render(*w->out, true);
                w->out->setFilePointer(end);
                // the chapters into their room (or at the end)
                if (nb_chapters) {
                    KaxChapters ch;
                    build_chapters(ch, w->uids, chapters, nb_chapters);
                    ch.UpdateSize(true);
                    bool placed = !w->est_chapters.empty() &&
                                  w->chapters_void.ReplaceWith(ch, *w->out, true, true) != INVALID_FILEPOS_T;
                    if (!placed) {
                        w->out->setFilePointer(0, seek_end);
                        ch.Render(*w->out, true);
                    }
                    write_tags(w);
                    w->seek.IndexThis(ch, w->segment);
                } else
                    write_tags(w);
                w->seek.UpdateSize();
                if (w->seek_void.ReplaceWith(w->seek, *w->out, true) == INVALID_FILEPOS_T)
                    fail(w, EIO);
                w->out->setFilePointer(0, seek_end);
                uint64 size = w->out->getFilePointer();
                if (w->segment.ForceSize(size - w->segment.GetDataStart()))
                    w->segment.OverwriteHead(*w->out);
                // the DocType versions the elements need: SimpleBlock (2, read 2), the IETF languages (4)
                GetChild<EDocTypeVersion>(w->head).SetValue(w->used_ietf ? 4 : 2);
                GetChild<EDocTypeReadVersion>(w->head).SetValue(2);
                w->out->setFilePointer(0);
                w->head.Render(*w->out, true);
                w->st.bytes = (int64_t)size;
            }
        }
    } catch (...) {
        fail(w, errno ? errno : EIO);
    }
    if (w->failed)
        ret = -w->error;
    if (st)
        *st = w->st;
    if (w->out) {
        try {
            w->out->close();
        } catch (...) {
            if (!ret)
                ret = -EIO;
        }
        delete w->out;
    }
    delete w;
    return ret;
}
