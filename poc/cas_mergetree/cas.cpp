#include "cas.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace cas
{

// ---------------------------------------------------------------------------
// Hash128 — deterministic 128-bit hash (two FNV-1a-64 streams with distinct
// seeds + a final mix). Production would use cityHash128 over the file bytes.
// ---------------------------------------------------------------------------
static uint64_t fnv1a64(const std::string & s, uint64_t basis)
{
    uint64_t h = basis;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    // final avalanche (splitmix64)
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

Hash128 hash128(const std::string & bytes)
{
    Hash128 h;
    h.lo = fnv1a64(bytes, 0xcbf29ce484222325ULL);
    // second stream over a length-salted view to decorrelate hi from lo
    std::string salted = std::string("\x01" "CAS") + std::to_string(bytes.size()) + bytes;
    h.hi = fnv1a64(salted, 0x9e3779b97f4a7c15ULL);
    return h;
}

std::string Hash128::hex() const
{
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16) << lo;
    return os.str();
}

// ---------------------------------------------------------------------------
// LocalObjectStore
// ---------------------------------------------------------------------------
namespace
{
class LocalObjectStore final : public ObjectStore
{
public:
    explicit LocalObjectStore(std::string root_) : root(std::move(root_))
    {
        fs::create_directories(root);
    }

    bool putIfAbsent(const std::string & key, const std::string & data) override
    {
        if (existsImpl(key))
        {
            st.put_skipped++;
            return false;
        }
        writeFile(key, data);
        st.put_new++;
        return true;
    }

    void put(const std::string & key, const std::string & data) override
    {
        bool existed = existsImpl(key);
        writeFile(key, data);
        if (!existed)
            st.put_new++;
    }

    bool exists(const std::string & key) const override { return existsImpl(key); }

    std::string get(const std::string & key) const override
    {
        fs::path p = pathOf(key);
        std::ifstream in(p, std::ios::binary);
        if (!in)
            throw std::runtime_error("object not found: " + key);
        st.get++;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool remove(const std::string & key) override
    {
        fs::path p = pathOf(key);
        std::error_code ec;
        bool removed = fs::remove(p, ec);
        if (removed)
            st.removed++;
        return removed;
    }

    std::vector<std::string> list(const std::string & prefix) const override
    {
        std::vector<std::string> out;
        if (!fs::exists(root))
            return out;
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it)
        {
            if (!it->is_regular_file())
                continue;
            std::string key = fs::relative(it->path(), root).generic_string();
            if (key.rfind(prefix, 0) == 0)
                out.push_back(key);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    const Stats & stats() const override { return st; }
    size_t countUnder(const std::string & prefix) const override { return list(prefix).size(); }

private:
    std::string root;
    mutable Stats st;

    fs::path pathOf(const std::string & key) const { return fs::path(root) / key; }
    bool existsImpl(const std::string & key) const { return fs::exists(pathOf(key)); }

    void writeFile(const std::string & key, const std::string & data)
    {
        fs::path p = pathOf(key);
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
};
} // namespace

std::unique_ptr<ObjectStore> makeLocalObjectStore(const std::string & root_dir)
{
    return std::make_unique<LocalObjectStore>(root_dir);
}

// ---------------------------------------------------------------------------
// PartInfo
// ---------------------------------------------------------------------------
static void requireCleanPartition(const std::string & p)
{
    // Part names are '_'-delimited and refs are newline-delimited text, so a
    // partition id must not contain '_' or '\n' (CH partition ids are sanitized).
    if (p.empty() || p.find('_') != std::string::npos || p.find('\n') != std::string::npos)
        throw std::runtime_error("invalid partition id (non-empty, no '_' or newline): '" + p + "'");
}

static std::vector<std::string> splitUnderscore(const std::string & s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == '_')
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

PartInfo PartInfo::parse(const std::string & name)
{
    auto t = splitUnderscore(name);
    if (t.size() < 4)
        throw std::runtime_error("bad part name: " + name);
    PartInfo p;
    p.partition = t[0];
    p.min_block = std::stoll(t[1]);
    p.max_block = std::stoll(t[2]);
    p.level = static_cast<uint32_t>(std::stoul(t[3]));
    p.mutation = t.size() > 4 ? std::stoll(t[4]) : 0;
    p.tombstone = (p.level == TOMBSTONE_LEVEL);
    return p;
}

std::string PartInfo::name() const
{
    std::ostringstream os;
    os << partition << '_' << min_block << '_' << max_block << '_' << level;
    if (mutation > 0)
        os << '_' << mutation;
    return os.str();
}

bool PartInfo::operator==(const PartInfo & o) const
{
    return partition == o.partition && min_block == o.min_block && max_block == o.max_block
        && level == o.level && mutation == o.mutation && tombstone == o.tombstone;
}

bool PartInfo::sameRangeIdentity(const PartInfo & o) const
{
    return partition == o.partition && min_block == o.min_block && max_block == o.max_block;
}

bool PartInfo::contains(const PartInfo & o) const
{
    // Mirrors MergeTreePartInfo::contains. A part supersedes another ONLY if it
    // spans the range AND has level >= AND mutation >= (mutation is unconditional:
    // a higher level never excuses a lower mutation — otherwise a stale merge would
    // mask a fresher mutated part = data loss), and the range is "strictly
    // contained" (equal range, or strictly higher level, or a tombstone marker).
    if (partition != o.partition)
        return false;
    if (*this == o)
        return false;
    const bool strict_range = (min_block == o.min_block && max_block == o.max_block)
        || level > o.level || level == TOMBSTONE_LEVEL;
    return min_block <= o.min_block && max_block >= o.max_block
        && level >= o.level && mutation >= o.mutation && strict_range;
}

PartInfo makeTombstone(const std::string & partition, int64_t min_block, int64_t max_block)
{
    PartInfo p;
    p.partition = partition;
    p.min_block = min_block;
    p.max_block = max_block;
    p.level = PartInfo::TOMBSTONE_LEVEL;
    // Maximal mutation so the tombstone covers every data part in range, including
    // mutated parts (mutation >= o.mutation must hold for any o).
    p.mutation = std::numeric_limits<int64_t>::max();
    p.tombstone = true;
    return p;
}

// ---------------------------------------------------------------------------
// Ref
// ---------------------------------------------------------------------------
std::string Ref::serialize() const
{
    return info.name() + "\n" + manifest_hash + "\n" + columns_hash + "\n";
}

Ref Ref::deserialize(const std::string & s)
{
    std::istringstream is(s);
    std::string name, mh, ch;
    std::getline(is, name);
    std::getline(is, mh);
    std::getline(is, ch);
    Ref r;
    r.info = PartInfo::parse(name);
    r.manifest_hash = mh;
    r.columns_hash = ch;
    return r;
}

// ---------------------------------------------------------------------------
// Manifest (length-prefixed canonical serialization; maps are ordered → stable)
// ---------------------------------------------------------------------------
static void putU64(std::string & buf, uint64_t v)
{
    char b[8];
    std::memcpy(b, &v, 8);
    buf.append(b, 8);
}
static void putStr(std::string & buf, const std::string & s)
{
    putU64(buf, s.size());
    buf.append(s);
}
static uint64_t getU64(const std::string & buf, size_t & pos)
{
    if (pos + 8 > buf.size())
        throw std::runtime_error("manifest truncated (u64)");
    uint64_t v;
    std::memcpy(&v, buf.data() + pos, 8);
    pos += 8;
    return v;
}
static std::string getStr(const std::string & buf, size_t & pos)
{
    uint64_t n = getU64(buf, pos);
    if (pos + n > buf.size())
        throw std::runtime_error("manifest truncated (str)");
    std::string s = buf.substr(pos, n);
    pos += n;
    return s;
}

std::string Manifest::serialize() const
{
    std::string buf = "CASM1";
    putU64(buf, blobs.size());
    for (const auto & [k, v] : blobs)
    {
        putStr(buf, k);
        putStr(buf, v.hash);
        putU64(buf, v.size);
    }
    putU64(buf, inlined.size());
    for (const auto & [k, v] : inlined)
    {
        putStr(buf, k);
        putStr(buf, v);
    }
    return buf;
}

Manifest Manifest::deserialize(const std::string & s)
{
    if (s.size() < 5 || s.compare(0, 5, "CASM1") != 0)
        throw std::runtime_error("bad manifest: missing magic");
    Manifest m;
    size_t pos = 5; // skip "CASM1"
    uint64_t nb = getU64(s, pos);
    for (uint64_t i = 0; i < nb; ++i)
    {
        std::string k = getStr(s, pos);
        BlobRef br;
        br.hash = getStr(s, pos);
        br.size = getU64(s, pos);
        m.blobs[k] = br;
    }
    uint64_t ni = getU64(s, pos);
    for (uint64_t i = 0; i < ni; ++i)
    {
        std::string k = getStr(s, pos);
        m.inlined[k] = getStr(s, pos);
    }
    return m;
}

std::string Manifest::contentHash() const
{
    return hash128(serialize()).hex();
}

// ---------------------------------------------------------------------------
// ActivePartSet
// ---------------------------------------------------------------------------
void ActivePartSet::add(const PartInfo & info) { parts.push_back(info); }

bool ActivePartSet::isActive(const PartInfo & info) const
{
    if (info.tombstone)
        return false;
    for (const auto & q : parts)
        if (!(q == info) && q.contains(info))
            return false;
    return true;
}

std::vector<PartInfo> ActivePartSet::activeDataParts() const
{
    std::vector<PartInfo> out;
    for (const auto & p : parts)
        if (!p.tombstone && isActive(p))
            out.push_back(p);
    return out;
}

std::vector<PartInfo> ActivePartSet::outdatedDataParts() const
{
    std::vector<PartInfo> out;
    for (const auto & p : parts)
    {
        if (p.tombstone)
            continue;
        bool covered = false;
        for (const auto & q : parts)
            if (!(q == p) && q.contains(p))
            {
                covered = true;
                break;
            }
        if (covered)
            out.push_back(p);
    }
    return out;
}

std::vector<PartInfo> ActivePartSet::staleTombstones() const
{
    std::vector<PartInfo> out;
    for (const auto & t : parts)
    {
        if (!t.tombstone)
            continue;
        bool covers_something = false;
        for (const auto & p : parts)
            if (!p.tombstone && t.contains(p))
            {
                covers_something = true;
                break;
            }
        if (!covers_something)
            out.push_back(t);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------
Catalog::Catalog(ObjectStore & store_) : store(store_) { }

void Catalog::putRef(const std::string & ns, const Ref & ref)
{
    store.put(ns + ref.info.name(), ref.serialize());
}

void Catalog::removeRef(const std::string & ns, const std::string & name)
{
    store.remove(ns + name);
}

std::optional<Ref> Catalog::getRef(const std::string & ns, const std::string & name) const
{
    std::string key = ns + name;
    if (!store.exists(key))
        return std::nullopt;
    return Ref::deserialize(store.get(key));
}

std::vector<Ref> Catalog::listRefs(const std::string & ns) const
{
    std::vector<Ref> out;
    for (const auto & key : store.list(ns))
        out.push_back(Ref::deserialize(store.get(key)));
    return out;
}

std::vector<Ref> Catalog::activeDataRefs() const
{
    auto refs = listRefs(NS_REFS);
    ActivePartSet aps;
    for (const auto & r : refs)
        aps.add(r.info);
    std::vector<Ref> out;
    for (const auto & r : refs)
        if (!r.info.tombstone && aps.isActive(r.info))
            out.push_back(r);
    return out;
}

std::vector<Ref> Catalog::outdatedDataRefs() const
{
    auto refs = listRefs(NS_REFS);
    ActivePartSet aps;
    for (const auto & r : refs)
        aps.add(r.info);
    auto outdated = aps.outdatedDataParts();
    std::set<std::string> names;
    for (const auto & p : outdated)
        names.insert(p.name());
    std::vector<Ref> out;
    for (const auto & r : refs)
        if (names.count(r.info.name()))
            out.push_back(r);
    return out;
}

std::vector<Ref> Catalog::staleTombstoneRefs() const
{
    auto refs = listRefs(NS_REFS);
    ActivePartSet aps;
    for (const auto & r : refs)
        aps.add(r.info);
    auto stale = aps.staleTombstones();
    std::set<std::string> names;
    for (const auto & p : stale)
        names.insert(p.name());
    std::vector<Ref> out;
    for (const auto & r : refs)
        if (r.info.tombstone && names.count(r.info.name()))
            out.push_back(r);
    return out;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
Engine::Engine(ObjectStore & store_) : store(store_), cat(store_) { }

static bool endsWith(const std::string & s, const std::string & suf)
{
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool Engine::isBlobFile(const std::string & logical)
{
    return endsWith(logical, ".bin") || endsWith(logical, ".mrk") || endsWith(logical, ".mrk2")
        || endsWith(logical, ".cmrk") || endsWith(logical, ".cmrk3");
}

static std::string columnsHashOf(const std::map<std::string, std::string> & files)
{
    std::string acc;
    for (const auto & [k, v] : files)
    {
        acc += k;
        acc += ':';
        acc += std::to_string(v.size());
        acc += ';';
    }
    return hash128(acc).hex();
}

Ref Engine::insertPart(const PartInfo & info, const std::map<std::string, std::string> & files)
{
    requireCleanPartition(info.partition);
    Manifest m;
    for (const auto & [logical, bytes] : files)
    {
        if (isBlobFile(logical))
        {
            std::string h = hash128(bytes).hex();
            store.putIfAbsent("blobs/" + h, bytes);
            m.blobs[logical] = BlobRef{h, bytes.size()};
        }
        else
        {
            m.inlined[logical] = bytes;
        }
    }
    std::string mh = m.contentHash();
    store.putIfAbsent("manifests/" + mh, m.serialize());

    Ref ref;
    ref.info = info;
    ref.manifest_hash = mh;
    ref.columns_hash = columnsHashOf(files);
    cat.putRef(Catalog::NS_REFS, ref);
    return ref;
}

Ref Engine::mergeParts(const PartInfo & new_info,
                       const std::vector<std::string> & /*source_names*/,
                       const std::map<std::string, std::string> & merged_files)
{
    // The new part covers the sources by name (covering rule); sources stay as
    // outdated refs until the lifecycle removes them.
    return insertPart(new_info, merged_files);
}

std::optional<std::string> Engine::resolveManifestHash(const std::string & name) const
{
    if (auto r = cat.getRef(Catalog::NS_REFS, name))
        return r->manifest_hash;
    if (auto r = cat.getRef(Catalog::NS_DETACHED, name))
        return r->manifest_hash;
    return std::nullopt;
}

Ref Engine::mutatePart(const PartInfo & new_info,
                       const std::string & source_name,
                       const std::map<std::string, std::string> & changed_files,
                       const std::set<std::string> & dropped_files)
{
    requireCleanPartition(new_info.partition);
    auto src_mh = resolveManifestHash(source_name);
    if (!src_mh)
        throw std::runtime_error("mutate: source part not found: " + source_name);
    Manifest m = Manifest::deserialize(store.get("manifests/" + *src_mh));

    for (const auto & d : dropped_files)
    {
        m.blobs.erase(d);
        m.inlined.erase(d);
    }
    for (const auto & [logical, bytes] : changed_files)
    {
        if (isBlobFile(logical))
        {
            std::string h = hash128(bytes).hex();
            store.putIfAbsent("blobs/" + h, bytes); // only changed columns are (re)uploaded
            m.inlined.erase(logical);
            m.blobs[logical] = BlobRef{h, bytes.size()};
        }
        else
        {
            m.blobs.erase(logical);
            m.inlined[logical] = bytes;
        }
    }
    // Unchanged blob files keep their source hash → carry-forward by reference.

    std::string mh = m.contentHash();
    store.putIfAbsent("manifests/" + mh, m.serialize());

    Ref ref;
    ref.info = new_info;
    ref.manifest_hash = mh;
    ref.columns_hash = hash128(mh).hex();
    cat.putRef(Catalog::NS_REFS, ref);
    return ref;
}

Ref Engine::dropRange(const std::string & partition, int64_t min_block, int64_t max_block)
{
    requireCleanPartition(partition);
    Ref ref;
    ref.info = makeTombstone(partition, min_block, max_block);
    ref.manifest_hash = "";
    ref.columns_hash = "";
    cat.putRef(Catalog::NS_REFS, ref);
    return ref;
}

void Engine::detachPart(const std::string & name)
{
    auto r = cat.getRef(Catalog::NS_REFS, name);
    if (!r)
        throw std::runtime_error("detach: not found: " + name);
    cat.putRef(Catalog::NS_DETACHED, *r);
    cat.removeRef(Catalog::NS_REFS, name);
}

void Engine::attachPart(const std::string & name)
{
    auto r = cat.getRef(Catalog::NS_DETACHED, name);
    if (!r)
        throw std::runtime_error("attach: not found in detached: " + name);
    if (cat.getRef(Catalog::NS_REFS, name))
        throw std::runtime_error("attach: a live ref already exists for " + name + " (would clobber)");
    cat.putRef(Catalog::NS_REFS, *r);
    cat.removeRef(Catalog::NS_DETACHED, name);
}

void Engine::freezePart(const std::string & snapshot, const std::string & name)
{
    // v3 fix: FREEZE materializes REAL bytes (not a reference), so a filesystem
    // backup of the snapshot is self-contained and survives GC of the originals.
    auto files = readPart(name);
    for (const auto & [logical, bytes] : files)
        store.put("frozen/" + snapshot + "/" + name + "/" + logical, bytes);
}

std::map<std::string, std::string> Engine::readPart(const std::string & name) const
{
    auto mh = resolveManifestHash(name);
    if (!mh)
        throw std::runtime_error("read: part not found: " + name);
    Manifest m = Manifest::deserialize(store.get("manifests/" + *mh));
    std::map<std::string, std::string> out;
    for (const auto & [logical, br] : m.blobs)
        out[logical] = store.get("blobs/" + br.hash);
    for (const auto & [logical, bytes] : m.inlined)
        out[logical] = bytes;
    return out;
}

void Engine::markOutdatedObserved(const std::string & name, int64_t now)
{
    outdated_since.emplace(name, now);
}

Engine::LifecycleResult Engine::removeOutdatedRefs(int64_t now, int64_t parts_lifetime)
{
    LifecycleResult res;

    // 1) outdated (covered) data refs: remove once observed-outdated for >= lifetime.
    for (const auto & r : cat.outdatedDataRefs())
    {
        const std::string n = r.info.name();
        markOutdatedObserved(n, now);
        if (now - outdated_since[n] >= parts_lifetime)
        {
            cat.removeRef(Catalog::NS_REFS, n);
            outdated_since.erase(n);
            res.refs_removed++;
        }
    }

    // 2) tombstones that no longer cover any present data ref → remove.
    for (const auto & t : cat.staleTombstoneRefs())
    {
        cat.removeRef(Catalog::NS_REFS, t.info.name());
        res.tombstones_removed++;
    }
    return res;
}

void Engine::pinSnapshot(const std::string & session, const std::vector<std::string> & part_names)
{
    for (const auto & n : part_names)
        if (auto mh = resolveManifestHash(n))
            reader_pins[session].insert(*mh);
}

void Engine::unpin(const std::string & session) { reader_pins.erase(session); }

std::set<std::string> Engine::pinnedManifestHashes() const
{
    std::set<std::string> out;
    for (const auto & [_, hashes] : reader_pins)
        out.insert(hashes.begin(), hashes.end());
    return out;
}

// ---------------------------------------------------------------------------
// GC
// ---------------------------------------------------------------------------
GC::GC(ObjectStore & store_, Engine & engine_) : store(store_), engine(engine_) { }

std::set<std::string> GC::markReachable() const
{
    std::set<std::string> manifest_hashes;

    auto collect = [&](const std::vector<Ref> & refs)
    {
        for (const auto & r : refs)
            if (!r.manifest_hash.empty())
                manifest_hashes.insert(r.manifest_hash);
    };
    collect(engine.catalog().listRefs(Catalog::NS_REFS));     // active + outdated-still-present
    collect(engine.catalog().listRefs(Catalog::NS_DETACHED)); // detached roots
    for (const auto & mh : engine.pinnedManifestHashes())     // ephemeral reader pins
        manifest_hashes.insert(mh);

    std::set<std::string> reachable;
    for (const auto & mh : manifest_hashes)
    {
        std::string mkey = "manifests/" + mh;
        reachable.insert(mkey);
        if (!store.exists(mkey))
            // Fail-close: a live ref pointing at a missing manifest is a hard
            // store/catalog inconsistency. Refuse to sweep rather than silently
            // widen the deletable set (which would delete the ref's live blobs).
            throw std::runtime_error("GC: referenced manifest is missing (store inconsistency): " + mkey);
        Manifest m = Manifest::deserialize(store.get(mkey));
        for (const auto & [_, br] : m.blobs)
            reachable.insert("blobs/" + br.hash);
    }
    return reachable;
}

GC::Result GC::run(int64_t now, int64_t grace)
{
    Result res;
    std::set<std::string> reachable = markReachable();

    std::vector<std::string> objs = store.list("blobs/");
    for (auto & m : store.list("manifests/"))
        objs.push_back(m);

    for (const auto & key : objs)
    {
        const bool is_blob = key.rfind("blobs/", 0) == 0;
        if (reachable.count(key))
        {
            first_unreachable.erase(key);
            if (is_blob)
                res.blobs_kept++;
            else
                res.manifests_kept++;
            continue;
        }
        // unreachable: grace is measured from FIRST loss of reachability.
        auto [it, inserted] = first_unreachable.emplace(key, now);
        if (now - it->second >= grace)
        {
            store.remove(key);
            first_unreachable.erase(key);
            if (is_blob)
                res.blobs_deleted++;
            else
                res.manifests_deleted++;
        }
        else
        {
            if (is_blob)
                res.blobs_aging++;
            else
                res.manifests_aging++;
        }
    }
    return res;
}

} // namespace cas
