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

extern "C" {
#include "mkv/mkv.h"
}

using namespace libebml;
using namespace libmatroska;

namespace {

const int64_t SCALE          = 1000000;           // the timestamp scale: 1 ms
const int64_t MAX_NS_CLUSTER = 5000000000LL;      // min(32700 x scale, 5 s)
const int     MAX_BLOCKS     = 65535;
const int     MAX_CONTENT    = 1500000;

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
    std::map<int64_t, int> duration_count;
    std::deque<PacketPtr> held;     // H.264 frames until the configuration record is made
    // PCM
    std::vector<uint8_t> pcm;
    int64_t samples_out = 0;
    int samples_per_packet = 0, packet_size = 0, frame_bytes = 0;
    bool pcm_started = false;
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
        GetChild<KaxTrackType>(e).SetValue(t.spec.codec == MKV_CODEC_H264 ? track_video : track_audio);
        GetChild<KaxCodecID>(e).SetValue(t.spec.codec == MKV_CODEC_H264 ? "V_MPEG4/ISO/AVC" : "A_PCM/INT/LIT");
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
        } else {
            KaxTrackAudio &a = GetChild<KaxTrackAudio>(e);
            GetChild<KaxAudioSamplingFreq>(a).SetValue((double)t.spec.sample_rate);
            GetChild<KaxAudioChannels>(a).SetValue(t.spec.channels);
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
    if (t.spec.codec == MKV_CODEC_H264)
        add = p.is_key_frame();
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
        t.stats.bytes += p.data.size();
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

// the tracks' next packets go out in time order once every track has one (or has ended)
int pump(MkvWriter *w, bool flushing)
{
    for (;;) {
        int winner = -1;
        for (size_t i = 0; i < w->tracks.size(); i++) {
            MkvTrackState &t = w->tracks[i];
            if (t.queue.empty()) {
                if (!t.done && !flushing)
                    return 0;
                continue;
            }
            if (winner < 0 || t.queue.front()->assigned < w->tracks[winner].queue.front()->assigned)
                winner = (int)i;
        }
        if (winner < 0)
            return 0;
        if (!w->started && write_headers(w) < 0)
            return -w->error;
        PacketPtr p = w->tracks[winner].queue.front();
        w->tracks[winner].queue.pop_front();
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

int h264_frame(MkvWriter *w, MkvTrackState &t, const uint8_t *data, int size, int64_t time, int64_t dur, int key)
{
    std::vector<std::pair<const uint8_t *, size_t>> nals;
    nal_units(data, size, nals);
    auto p = std::make_shared<Packet>();
    p->track = (int)(&t - w->tracks.data());
    p->timestamp = p->assigned = time;
    p->duration = dur;
    p->key = key;
    bool slice_seen = false;
    for (auto &n : nals) {
        int type = n.first[0] & 31;
        if (type == 9 || type == 10 || type == 11 || type == 12)
            continue;               // access unit delimiter, end of sequence / stream, filler data
        if (!t.header_ready && !slice_seen && (type == 7 || type == 8)) {
            std::vector<uint8_t> v(n.first, n.first + n.second);
            if (type == 7) {
                H264Sps s;
                parse_sps(n.first, n.second, s);
                auto it = std::find(t.sps_ids.begin(), t.sps_ids.end(), s.id);
                if (it == t.sps_ids.end()) {
                    t.sps_ids.push_back(s.id);
                    t.sps.push_back(v);
                } else
                    t.sps[it - t.sps_ids.begin()] = v;
                if (!t.have_sps) {
                    t.first_sps = s;
                    t.have_sps = true;
                }
            } else {
                Bits r(n.first + 1, n.second - 1);
                unsigned id = r.ue();
                auto it = std::find(t.pps_ids.begin(), t.pps_ids.end(), id);
                if (it == t.pps_ids.end()) {
                    t.pps_ids.push_back(id);
                    t.pps.push_back(v);
                } else
                    t.pps[it - t.pps_ids.begin()] = v;
            }
        }
        if ((type == 1 || type == 5) && !slice_seen) {
            slice_seen = true;
            p->discardable = !(n.first[0] >> 5);   // nal_ref_idc 0
        }
        uint32_t l = (uint32_t)n.second;
        uint8_t h[4] = { (uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l };
        p->data.insert(p->data.end(), h, h + 4);
        p->data.insert(p->data.end(), n.first, n.first + n.second);
    }
    if (p->discardable < 0)
        p->discardable = 0;
    if (!t.header_ready) {
        if (dur > 0)
            t.duration_count[dur]++;
        t.held.push_back(p);
        // the header is made when the first frames are in (mkvmerge: its first batch of frames)
        if (t.held.size() < 16 && !(key && t.held.size() > 1))
            return 0;
        if (t.sps.empty() || t.pps.empty())
            return fail(w, EINVAL);
        h264_make_header(t);
        for (auto &h : t.held)
            t.queue.push_back(h);
        t.held.clear();
        return pump(w, false);
    }
    t.queue.push_back(p);
    return pump(w, false);
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
        t.language = tracks[i].language && *tracks[i].language ? tracks[i].language : "und";
        t.language_ietf = tracks[i].language_ietf && *tracks[i].language_ietf ? tracks[i].language_ietf
                                                                              : bcp47_of(t.language.c_str());
        t.name = tracks[i].name ? tracks[i].name : "";
        t.uid = w->uids.make(UID_TRACK);
        if (t.spec.codec == MKV_CODEC_H264) {
            if (w->video < 0)
                w->video = i;
        } else if (t.spec.codec == MKV_CODEC_PCM) {
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

extern "C" int mkv_frame(MkvWriter *w, int track, const uint8_t *data, int size, int64_t time, int64_t duration,
                         int key)
{
    if (w->failed)
        return -w->error;
    if (track < 0 || track >= (int)w->tracks.size())
        return -EINVAL;
    MkvTrackState &t = w->tracks[track];
    try {
        if (t.spec.codec == MKV_CODEC_H264)
            return h264_frame(w, t, data, size, time, duration, key);
        return pcm_frame(w, t, data, size, time);
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
                t.done = true;
            }
            if (!w->failed && pump(w, true) == 0 && !w->started)
                write_headers(w);
            if (!w->failed && w->cluster && !w->packets.empty())
                render_cluster(w);
            if (!w->failed) {
                write_cues(w);
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
