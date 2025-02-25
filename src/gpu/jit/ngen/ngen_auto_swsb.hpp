/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

/*
 * Do not #include this file directly; ngen uses it internally.
 */

#ifndef NGEN_AUTO_SWSB_HPP
#define NGEN_AUTO_SWSB_HPP

#if defined(NGEN_DEBUG) || defined(NGEN_DEBUG_PROPAGATE) || defined(NGEN_DEBUG_BB)
#include <iomanip>
#include <iostream>
#endif

#include <limits>
#include <list>
#include <limits>
#include <map>

namespace ngen {
namespace autoswsb {

/*******************/
/* Data structures */
/*******************/

typedef uint8_t PipeMask;
enum {
    PipeMaskNone = 0,
    PipeMaskA = 1,      // All in-order pipes
    PipeMaskF = 2,
    PipeMaskI = 4,
    PipeMaskL = 8,
    PipeMaskO = 0x10,   // All out-of-order pipes. Not a valid GeneralizedPipe.
    PipeBitA = 0,
    PipeBitF = 1,
    PipeBitI = 2,
    PipeBitL = 3,
    PipeBitO = 4,
};
static constexpr int NPipes = 4;

static inline PipeMask toMask(Pipe pipe)   { return (1 << (static_cast<unsigned>(pipe) - 1)); }
static inline Pipe fromMask(PipeMask mask) { return mask ? static_cast<Pipe>(1 + utils::log2(mask)) : Pipe::Default; }

typedef uint8_t DestinationMask;
enum {
    DestNone = 0,
    DestNextIP = 1,
    DestJIP = 2,
    DestUIP = 4,
    DestUnknown = 8
};

class GeneralizedPipe {
    uint8_t v;

    static constexpr uint8_t vInOrder = 0x00;
    static constexpr uint8_t vSend = 0x40;        // OR'ed with SFID
    static constexpr uint8_t vSystolic = 0x80;
    static constexpr uint8_t vMath = 0xC0;
    static constexpr uint8_t vTypeMask = 0xC0;

    GeneralizedPipe(uint8_t v_, int dummy) : v{v_} {}

public:
    GeneralizedPipe()                    : v{uint8_t(0)} {}
    GeneralizedPipe(PipeMask pipe)       : v{uint8_t(vInOrder | pipe)} {}
    GeneralizedPipe(SharedFunction sfid) : v{uint8_t(vSend | static_cast<uint8_t>(sfid))} {}

    static GeneralizedPipe Systolic() { return GeneralizedPipe(vSystolic, 0); }
    static GeneralizedPipe Math()     { return GeneralizedPipe(vMath, 0); }

    bool operator==(GeneralizedPipe other) const { return v == other.v; }
    bool operator!=(GeneralizedPipe other) const { return v != other.v; }

    bool inOrder() const { return ((v & vTypeMask) == vInOrder) && (v != PipeMaskNone); }
    PipeMask inOrderPipe() const { return inOrder() ? (v & ~vTypeMask) : PipeMaskNone; }
    Pipe toPipe() const { return fromMask(inOrderPipe()); }
    inline PipeMask syncPipes(HW hw) const;

#ifdef NGEN_DEBUG
    inline void dump() const;
#endif
};

struct DependencyRegion {
    uint8_t base, size;
    uint8_t unspecified : 1;
    uint8_t checkWAW : 1;
    HW hw;
    std::array<uint32_t, 16> masks;

    DependencyRegion() : DependencyRegion(HW::Unknown) {}
    explicit DependencyRegion(HW hw_) : base(0), size(0), unspecified{true}, checkWAW{false}, hw{hw_} {
        for (auto &m: masks) m = 0;
    }
    inline DependencyRegion(HW hw, GRFRange r);
    inline DependencyRegion(HW hw, int esize, RegData rr);

    inline void intersect(const DependencyRegion &other);
    inline void subtract(const DependencyRegion &other);

    bool empty() const {
        if (unspecified) return false;
        for (auto m : masks)
            if (m != 0)
                return false;
        return true;
    }
    void clear()        { *this = DependencyRegion(hw); unspecified = false; checkWAW = false; }

#ifdef NGEN_DEBUG
    inline void dump() const;
#endif
};

template <bool consumer>
struct Dependency {
    int32_t label;                                      // Multipurpose label for use in algorithms

    // Source instruction information.
    GeneralizedPipe pipe;                               // Execution pipe for instruction
    uint16_t tokenTime;                                 // Estimated upper bound for token lifetime, in cycles.
    std::array<int32_t, NPipes> counters;               // Pipe counters, relative to start of BB.

    // (Mostly) dependency information.
    uint8_t token;                                      // Out of order token
    uint8_t tokenSrc : 1;                               // Src dependency on token?
    uint8_t tokenDst : 1;                               // Dst dependency on token?
    uint8_t rw : 1;                                     // Flag: read or write?
    uint8_t swsb : 1;                                   // True for SWSB dependency consumers
    PipeMask depPipe;                                   // (swsb consumer only) Pipe to wait on
    uint8_t dist;                                       // (swsb consumer only) Pipe distance
    DependencyRegion region;                            // GRF region covered

    Dependency() : label{0}, pipe{}, tokenTime{0},
        token{0}, tokenSrc{false}, tokenDst{false},
        rw{false}, swsb{false}, depPipe{PipeMaskNone}, dist{0}, region{} { counters.fill(0); }

    bool operator==(const Dependency &other) {
        return !std::memcmp(this, &other, sizeof(Dependency));
    }
    bool operator!=(const Dependency *other) { return !(operator==(other)); }

    int32_t &inum()                 { return counters[1]; }     // For OOO dependencies in phase 0
    const int32_t &inum() const     { return counters[1]; }

    constexpr bool read() const     { return !rw; }
    constexpr bool write() const    { return rw; }
    constexpr bool hasToken() const { return tokenSrc || tokenDst; }
    constexpr bool hasDist() const  { return (dist > 0); }

    Dependency<!consumer>& cast()   { return *reinterpret_cast<Dependency<!consumer>*>(this); }

    static constexpr uint8_t tokenTBD = 0xFF;

#ifdef NGEN_DEBUG
    inline void dump() const;
#endif
};

template <bool consumer>
class DependencyTable {
    std::list<Dependency<consumer>> deps;

public:
    void clear()                                                        { deps.clear(); }
    inline bool insert(Dependency<consumer> &dep, bool checkWeaker = true, bool checkStronger = true);
    inline bool insertWeak(Dependency<consumer> &dep)                   { return insert(dep, true, false); }
    inline void insertStrong(const Dependency<consumer> &dep)           { (void) insert(const_cast<Dependency<consumer> &>(dep), false, true); }
    inline void remove(const Dependency<consumer> &dep);
    template <bool iconsumer> inline void findIntersections(const Dependency<iconsumer> &dep, std::vector<Dependency<consumer>> &out);
    template <bool iconsumer> inline void findAndRemoveIntersections(const Dependency<iconsumer> &dep, std::vector<Dependency<consumer>> *out, HW hw);
    template <bool iconsumer> inline void removeIntersections(const Dependency<iconsumer> &dep, HW hw);
    inline void subtractAllRegions(DependencyRegion &region);
    inline uint32_t removeByTokenMask(uint32_t mask, bool dst);
    inline uint32_t removeOOOWritesByRegion(const DependencyRegion &region);
    inline bool implies(const Dependency<consumer> &other);

    template <typename Func> inline void forEach(Func f)                { for (auto &entry : deps) f(entry); }
    template <typename Func> inline void forEach(Func f) const          { for (auto &entry : deps) f(entry); }

#ifdef NGEN_DEBUG
    inline void dump() const;
#endif
};

struct SyncInsertion {
    uint32_t inum;
    SWSBInfo swsb;
    SyncFunction fc;
    uint32_t mask;                                  // (allrd/allwr) 0 indicates no mask to be applied.
};

struct BasicBlock;

struct BasicBlock {
    uint32_t id;                                    // index
    int32_t label;                                  // multipurpose flag for use in algorithms
    uint32_t istart, iend;                          // instruction range: [istart, iend)
    uint32_t wrdeps;                                // # of wrdep pseudo-instructions in this BB
    std::array<uint32_t, NPipes> lengths;           // # of instructions in each pipe in this BB
    std::vector<BasicBlock *> pred, succ;           // list of predecessor/successor BBs
    DependencyTable<false> producers;               // table of dependencies produced and consumed by this BB.
    DependencyTable<true> consumers;                //   production table re-used for live incoming dependencies.
    DependencyTable<false> incoming;                // table of dependencies produced by prior BBs (temporary).
    std::vector<SyncInsertion> syncs;               // list of sync instructions to generate.
};

using BasicBlockList = std::vector<BasicBlock>;

/*****************/
/* Pipe Handling */
/*****************/

// Get all pipes to track in-order dependencies on.
inline PipeMask allPipes(HW hw)
{
    PipeMask mask = PipeMaskA | PipeMaskO;
    if (hw >= HW::XeHP) mask |= PipeMaskF | PipeMaskI | PipeMaskL;

    return mask;
}

// Get the execution pipe for an instruction.
template <typename Instruction>
inline GeneralizedPipe getPipe(HW hw, const Instruction &insn, bool checkOOO = true)
{
    // Check jumps and no-ops
    auto op = insn.opcode();
    if (isBranch(op) || op == Opcode::nop_gen12 || op == Opcode::sync || op == Opcode::illegal)
        return GeneralizedPipe();

    // Check OOO instructions.
    if (isVariableLatency(hw, op)) {
        if (!checkOOO)
            return GeneralizedPipe();
        switch (op) {
            case Opcode::dpas:
            case Opcode::dpasw:
                return GeneralizedPipe::Systolic();
            case Opcode::math:
                return GeneralizedPipe::Math();
            case Opcode::send:
            case Opcode::sendc:
                return GeneralizedPipe(insn.sfid());
            default:
                break;
        }
    }


    PipeMask mask = PipeMaskNone;

    // For SWSB purposes, Gen12LP has a single in-order pipe.
    // Otherwise, in-order pipe determined by destination type.
    // Exception: if there are any long operands, it's a long pipe instruction.
    if (hw >= HW::XeHP) {
        auto dt = insn.dstTypecode();
        unsigned lmask = 3;
        if ((dt & lmask) == lmask)
            mask |= PipeMaskL;
        else if (dt & 8)
            mask |= PipeMaskF;
        else
            mask |= PipeMaskI;

        if (!(mask & PipeMaskL)) {
            if ((insn.src0Typecode() & lmask) == lmask)
                mask = PipeMaskL;
            else if ((insn.src1Typecode() & lmask) == lmask)
                mask = PipeMaskL;
        }
    } else
        mask = PipeMaskA;
    return mask;
}

template <typename Instruction>
inline PipeMask getPipeMask(HW hw, const Instruction &insn)
{
    PipeMask pipe = getPipe(hw, insn, false).inOrderPipe();
    if (pipe != PipeMaskNone)
        pipe |= PipeMaskA;
    return pipe;
}

PipeMask GeneralizedPipe::syncPipes(HW hw) const
{
    if ((hw >= HW::XeHP) && (v & PipeMaskA))
        return allPipes(hw) & ~PipeMaskA & ~PipeMaskO;
    return (v == PipeMaskNone) ? allPipes(hw) : inOrderPipe();
}

/**********************/
/* Dependency Regions */
/**********************/
DependencyRegion::DependencyRegion(HW hw_, GRFRange r)
{
    auto nmasks = int(masks.size());
#ifdef NGEN_SAFE
    if (r.isInvalid() || (r.getLen() > nmasks))
        throw invalid_region_exception();
#endif

    hw = hw_;
    unspecified = false;
    checkWAW = false;
    base = r.getBase();
    size = r.getLen();
    auto fullMask = ~uint32_t(0);
    for (int i = 0; i < nmasks; i++)
        masks[i] = (i < r.getLen()) ? fullMask : 0u;
}

DependencyRegion::DependencyRegion(HW hw_, int esize, RegData rr)
{
    const auto mbits = GRF::bytes(hw_);
    const auto log2MBits = GRF::log2Bytes(hw_);

    hw = hw_;
    base = rr.getBase();
    unspecified = false;
    checkWAW = false;

    int hs = rr.getHS(), vs = rr.getVS();
    int nh = rr.getWidth();
#ifdef NGEN_SAFE
    if (nh == 0) nh = 1;
#endif
    int nv = esize / nh;
    int bytes = rr.getBytes();
    int off = rr.getByteOffset();

    auto makeMask = [](int sz) -> uint64_t {
        return (uint64_t(1) << sz) - 1;
    };

    auto compress = [&](uint64_t m) -> uint32_t {
        return uint32_t(m);
    };

    if (hs == 0) nh = hs = 1;
    if (vs == 0) nv = 1;
    hs *= bytes;
    vs *= bytes;

    for (auto &m : masks)
        m = 0;

    uint64_t hmask = makeMask(bytes) * (makeMask(nh * hs) / makeMask(hs));
    for (int j = 0; j < nv; j++) {
        masks[off >> log2MBits] |= compress(hmask << (off & (mbits - 1)));
        off += vs;
    }

    size = ((off - vs) >> log2MBits) + 1;
}

void DependencyRegion::intersect(const DependencyRegion &other)
{
    if (unspecified || other.unspecified)
        return;

    int i, iOther;
    for (i = 0, iOther = base - other.base; i < size; i++, iOther++) {
        if (iOther >= 0 && iOther < other.size)
            masks[i] &= other.masks[iOther];
        else
            masks[i] = 0;
    }
}

// Check whether two regions overlap.
inline bool intersects(const DependencyRegion &dep1, const DependencyRegion &dep2)
{
    // Unspecified regions might always overlap.
    if (dep1.unspecified || dep2.unspecified)
        return true;

    // Quick check based on register bounds.
    int diff = dep1.base - dep2.base;
    if ((diff >= dep2.size) || (diff <= -dep1.size))
        return false;

    // Precise check.
    int i1, i2;
    for (i1 = 0, i2 = diff; i1 < dep1.size; i1++, i2++)
        if (i2 >= 0 && i2 < dep2.size)
            if (dep1.masks[i1] & dep2.masks[i2])
                return true;

    return false;
}

void DependencyRegion::subtract(const DependencyRegion &other)
{
    if (unspecified)
        return;
    if (other.unspecified)
        clear();
    else {
        int i, iOther;
        for (i = 0, iOther = base - other.base; i < size; i++, iOther++)
            if (iOther >= 0 && iOther < other.size)
                masks[i] &= ~other.masks[iOther];
    }
}

inline bool contains(const DependencyRegion &dep1, const DependencyRegion &dep2)
{
    using mtype = decltype(DependencyRegion::masks)::value_type;

    if (dep1.unspecified) return true;
    if (dep2.unspecified) return false;

    int i1, i2;
    for (i1 = dep2.base - dep1.base, i2 = 0; i2 < dep2.size; i1++, i2++) {
        mtype mask = (i1 >= 0 && i1 < dep1.size) ? dep1.masks[i1] : 0;
        if (~mask && dep2.masks[i2])
            return false;
    }
    return true;
}

// Distance in an in-order pipe after which a dependency can be ignored.
inline int timeout(GeneralizedPipe pipe)
{
    switch (pipe.inOrderPipe()) {
        case PipeMaskA: return 11; // Gen12LP
        case PipeMaskI: return 11;
        case PipeMaskF: return 11;
        case PipeMaskL: return 15;
        default:        return std::numeric_limits<int>::max();
    }
}

// Approximate upper bound on cycle count for an OOO instruction.
template <typename Instruction>
inline int estimateLatency(HW hw, const Instruction &insn)
{
    switch (insn.opcode()) {
        case Opcode::math: return (hw == HW::Gen12LP) ? 20 : 17;
        case Opcode::dpas:
        case Opcode::dpasw: return 20;   // need correct value
        case Opcode::send:
        case Opcode::sendc: {
            switch (insn.sfid()) {
                case SharedFunction::dc0:
                case SharedFunction::dc1: {
                    MessageDescriptor desc;
                    if (insn.getSendDesc(desc))
                        if (desc.surface.index == 0xFE)
                            return (hw == HW::Gen12LP) ? 33 : 25;
                    return (hw == HW::Gen12LP) ? 106 : 150;
                }
                case SharedFunction::sampler: return (hw == HW::Gen12LP) ? 175 : 210;
                default: return 50;
            }
        }
        default: return 0;
    }
}

// Measure instruction distance between two Dependencies in a given pipe.
template <bool consumer1, bool consumer2>
inline int distance(const Dependency<consumer1> &dep1, const Dependency<consumer2> &dep2, GeneralizedPipe pipe)
{
    auto ioPipe = pipe.inOrderPipe();

    if (ioPipe == PipeMaskNone)
        return 0;

    auto pidx = utils::log2(ioPipe);
    return dep2.counters[pidx] - dep1.counters[pidx];
}

// Check whether two dependencies form a producer-consumer pair.
inline bool intersects(const Dependency<false> &dep1, const Dependency<true> &dep2)
{
    if (!dep2.swsb) {
        // Region-based dependency. First, quick check based on dependency type:
        //   RAR:     ignore
        //   WAR/WAW: ignore if both instructions in same GeneralizedPipe (same in-order pipe, or sends with same SFID, etc.)
        // If not ignorable, check:
        //   * If consumer is in-order, is that pipe still live (unsynchronized) in the producer?
        //   * If producer is in-order, is it close enough to require tracking the dependency?
        //   * Do the producer+consumer regions overlap?
        if (dep1.read() && dep2.read())                                                             return false;
        if (dep2.write() && (dep1.pipe == dep2.pipe) && (dep1.pipe != GeneralizedPipe::Math()))     return false;
        if (dep1.pipe.inOrder() && (distance(dep1, dep2, dep1.pipe) >= timeout(dep1.pipe)))         return false;
        return intersects(dep1.region, dep2.region);
    } else {
        // SWSB dependency.
        if (dep1.hasToken() && dep2.hasToken() && (dep1.token == dep2.token) && (dep1.tokenSrc || dep2.tokenDst) && (dep1.token != dep1.tokenTBD))
            return true;
        if (dep1.pipe.inOrder()) {
            auto commonPipe = (dep1.pipe.inOrderPipe() | PipeMaskA) & dep2.depPipe;
            if (commonPipe)
                return (distance(dep1, dep2, dep1.pipe) >= dep2.dist);      // In theory should check timeout, but this
                                                                            // path is only used for removing dependencies.
        }
        return false;
    }
}

// Check whether two dependencies form a producer-consumer pair.
inline bool intersects(const Dependency<true> &dep1, const Dependency<false> &dep2)
{
    return intersects(dep2, dep1);
}

// Check whether one producer dependency implies another, without checking regions.
inline bool impliesWithoutRegion(const Dependency<false> &dep1, const Dependency<false> &dep2)
{
    // Reads never imply writes.
    if (dep2.write() && dep1.read())
        return false;
    // Check pipes.
    if (dep2.pipe != dep1.pipe)
        return false;
    if (dep2.hasToken()) {
        // Token dependency: tokens must match. If tokens not assigned, instructions must match.
        if (!dep1.hasToken())
            return false;
        if (!dep1.tokenDst && dep2.tokenDst)
            return false;
        if (dep1.token != dep2.token)
            return false;
        if ((dep1.token == dep1.tokenTBD) && (dep1.inum() != dep2.inum()))
            return false;
    }
    if (dep2.pipe.inOrder()) {
        // Pipeline dependency: compare counters.
        if (dep1.counters[PipeBitA] < dep2.counters[PipeBitA])
            return false;
        auto pidx = utils::log2(dep2.pipe.inOrderPipe());
        if (dep1.counters[pidx] < dep2.counters[pidx])
            return false;
    }
    return true;
}

// Check whether one consumer dependency implies another, without checking regions.
inline bool impliesWithoutRegion(const Dependency<true> &dep1, const Dependency<true> &dep2)
{
    // Writes never imply reads. However, if a read is inserted overlapping a prior
    //   write, we may assume the dependency was handled in a prior instruction.
    /* if (dep2.read() && dep1.write()) return false; */

    // Check pipes.
    if (dep2.pipe != dep1.pipe)
        return false;
    if (dep2.depPipe != dep1.depPipe)
        return false;
    if (dep2.hasToken()) {
        // Token dependency.
        if (!dep1.hasToken())
            return false;
        if (!dep1.tokenDst && dep2.tokenDst)
            return false;
        if (dep1.token != dep2.token)
            return false;
    }
    if (dep2.pipe.inOrder()) {
        // Pipeline dependency. Consumer dependencies are only compared
        //  within BBs, so it's enough to check the A counter.
        // Note distance check not always valid for A@ consumers >= XeHP,
        //  but is never used in these cases.
        if (dep2.counters[PipeBitA] < dep1.counters[PipeBitA])
            return false;
        if (dep2.hasDist() != dep1.hasDist())
            return false;
        if (dep2.hasDist())
            if (distance(dep1, dep2, dep2.pipe) - dep2.dist + dep1.dist < 0)
                return false;
        if (dep1.read() && dep2.write())
            return false;
    }
    return true;
}

template <bool consumer>
inline bool implies(const Dependency<consumer> &dep1, const Dependency<consumer> &dep2)
{
    return impliesWithoutRegion(dep1, dep2) && contains(dep1.region, dep2.region);
}

// Insert dependency into table.
// If checkStronger set, remove any weaker existing dependencies.
// If checkWeaker set, the input dependency's region will be adjusted to remove
//   overlapping stronger dependencies. If this dependency is already implied by the
//   table, it will not be added.
// Return value indicates whether dependency added.
template <bool consumer>
bool DependencyTable<consumer>::insert(Dependency<consumer> &dep, bool checkWeaker, bool checkStronger)
{
    for (auto entry = deps.begin(); entry != deps.end();) {
        bool noRegions = (dep.region.unspecified && entry->region.unspecified);

        if (checkWeaker && impliesWithoutRegion(*entry, dep)) {
            if (noRegions)
                return false;
            dep.region.subtract(entry->region);
            if (dep.region.empty())
                return false;
        }
        if (checkStronger && impliesWithoutRegion(dep, *entry)) {
            entry->region.subtract(dep.region);
            if (entry->region.empty() || noRegions) {
                entry = deps.erase(entry);
                continue;
            }
        }
        entry++;
    }

    deps.push_back(dep);
    return true;
}

template <bool consumer>
void DependencyTable<consumer>::remove(const Dependency<consumer> &dep)
{
    deps.remove(dep);
}

// Find dependencies in the table intersecting the given dependency, and append them to the given list.
template <bool consumer>
template <bool iconsumer>
void DependencyTable<consumer>::findIntersections(const Dependency<iconsumer> &dep, std::vector<Dependency<consumer>> &out)
{
    for (auto &entry : deps)
        if (intersects(dep, entry))
            out.push_back(entry);
}

// Find dependencies in the table intersecting the given dependency.
// Append them to the given list, and remove from table.
// Also checks for, and removes, timed-out producer dependencies.
template <bool consumer>
template <bool iconsumer>
void DependencyTable<consumer>::findAndRemoveIntersections(const Dependency<iconsumer> &dep, std::vector<Dependency<consumer>> *out, HW hw)
{
    for (auto entry = deps.begin(); entry != deps.end();) {
        if (!consumer && (distance(*entry, dep, entry->pipe) >= timeout(entry->pipe))) {
            entry = deps.erase(entry);
            continue;
        }
        if (intersects(dep, *entry)) {
            if (out != nullptr)
                out->push_back(*entry);
            entry = deps.erase(entry);
            continue;
        }
        entry++;
    }
}

// Find dependencies in the table intersecting the given dependency, and remove them.
template <bool consumer>
template <bool iconsumer>
void DependencyTable<consumer>::removeIntersections(const Dependency<iconsumer> &dep, HW hw)
{
    findAndRemoveIntersections(dep, nullptr, hw);
}

// Subtract all regions from the table from the given region.
template <bool consumer>
void DependencyTable<consumer>::subtractAllRegions(DependencyRegion &region)
{
    for (auto &entry : deps)
        if (!entry.region.unspecified)
            region.subtract(entry.region);
}

// Remove dependencies from the table matching a token mask.
// Returns mask of unmatched tokens.
template <bool consumer>
uint32_t DependencyTable<consumer>::removeByTokenMask(uint32_t mask, bool dst)
{
    auto unmatched = mask;

    for (auto entry = deps.begin(); entry != deps.end();) {
        if (entry->token != entry->tokenTBD) {
            auto entryMask = (1 << entry->token);
            if ((entry->tokenSrc || (entry->tokenDst && dst)) && (mask & entryMask)) {
                unmatched &= ~entryMask;
                entry = deps.erase(entry);
                continue;
            }
        }
        entry++;
    }

    return unmatched;
}

// Remove OOO writes intersecting the given region. Return mask of token IDs for these writes.
template <bool consumer>
uint32_t DependencyTable<consumer>::removeOOOWritesByRegion(const DependencyRegion &region)
{
    uint32_t removed = 0;

    for (auto entry = deps.begin(); entry != deps.end();) {
        if (!entry->pipe.inOrder() && entry->write() && intersects(entry->region, region)) {
            if (entry->token != entry->tokenTBD)
                removed |= (1 << entry->token);
            entry = deps.erase(entry);
            continue;
        }
        entry++;
    }

    return removed;
}

// Check if the given dependency is already implied by this table.
template <>
inline bool DependencyTable<false>::implies(const Dependency<false> &other)
{
    for (auto &entry : deps)
        if (autoswsb::implies(entry, other))
            return true;
    return false;
}

#ifdef NGEN_DEBUG
inline void dumpPipeMask(PipeMask mask, bool spacers = true)
{
    if (spacers) {
        std::cerr << char((mask & PipeMaskA) ? 'A' : ' ');
        std::cerr << char((mask & PipeMaskF) ? 'F' : ' ');
        std::cerr << char((mask & PipeMaskI) ? 'I' : ' ');
        std::cerr << char((mask & PipeMaskL) ? 'L' : ' ');
        std::cerr << char((mask & PipeMaskO) ? 'O' : ' ');
    } else {
        if (mask & PipeMaskA) std::cerr << 'A';
        if (mask & PipeMaskF) std::cerr << 'F';
        if (mask & PipeMaskI) std::cerr << 'I';
        if (mask & PipeMaskL) std::cerr << 'L';
        if (mask & PipeMaskO) std::cerr << 'O';
        if (mask == PipeMaskNone) std::cerr << '-';
    }
}

void GeneralizedPipe::dump() const
{
    switch (v & vTypeMask) {
        case vInOrder:  dumpPipeMask(inOrderPipe(), false); break;
        case vSystolic: std::cerr << 'D'; break;
        case vMath:     std::cerr << 'M'; break;
        case vSend:     std::cerr << 'S' << int(v & 0xF); break;
        default:        std::cerr << '?'; break;
    }
}

void DependencyRegion::dump() const
{
    if (unspecified)
        std::cerr << "[no region]";
    else if (size == 0)
        std::cerr << "[zero size region]";
    else {
        std::cerr << "r" << int(base);
        if (size > 1)
            std::cerr << "-r" << int(base + size - 1);

    auto fullMask = ~uint32_t(0);
        bool partial = false;
        for (int ii = 0; ii < size; ii++)
            partial |= (masks[ii] != fullMask);

        if (partial) {
            std::cerr << " (" << std::hex;
            for (int ii = 0; ii < size; ii++) {
                if (masks[ii] != fullMask)
                    std::cerr << std::setw(sizeof(masks[ii]) * 2) << masks[ii];
                else
                    std::cerr << "all";
                std::cerr << char((ii == (size - 1)) ? ')' : ' ');
            }
            std::cerr << std::dec;
        }
    }
}

template <bool consumer>
void Dependency<consumer>::dump() const
{
    if (tokenTime > 0) {
        std::cerr << '[' << counters[PipeBitA] << " + " << tokenTime;
        std::cerr << ',' << inum();
    } else {
        std::cerr << '[';
        for (auto &counter : counters)
            std::cerr << counter << ',';
        pipe.dump();
    }
    std::cerr << ']';
    if (hasToken()) {
        std::cerr << " $";
        if (token == tokenTBD)
            std::cerr << '?';
        else
            std::cerr << std::hex << int(token) << std::dec;
        if (tokenSrc && !tokenDst)
            std::cerr << ".src";
        else if (tokenDst && !tokenSrc)
            std::cerr << ".dst";
        else
            std::cerr << "    ";
    } else
        std::cerr << "       ";
    if (dist > 0) {
        dumpPipeMask(depPipe, false);
        std::cerr << '@' << int(dist);
    } else
        std::cerr << "   ";

    std::cerr << (rw ? " write " : "  read ");
    if (!region.unspecified)
        region.dump();
}

template <bool consumer>
void DependencyTable<consumer>::dump() const
{
    std::cerr << (consumer ? "Consumers:\n" : "Producers:\n");
    for(const auto &dep : deps) {
        std::cerr << '\t';
        dep.dump();
        std::cerr << std::endl;
    }
}
#endif

/*****************/
/* Main Routines */
/*****************/

template <typename Program>
inline bool hasAutoSWSB(HW hw, const Program &program)
{
    if (hw < HW::Gen12LP)
        return false;
    for (uint32_t n = 0; n < program.size(); n++)
        if (program[n].autoSWSB())
            return true;
    return false;
}

// Get a list of basic blocks for this program.
template <typename Program>
inline BasicBlockList getBasicBlocks(HW hw, const Program &program)
{
    auto icount = int(program.size());

    // Create map from BB head instructions to instruction #s.
    std::map<int, int> heads;
    heads.insert({0, 0});

    // Scan through program and find all fixed jump targets. These will
    //  be the BB heads (first instruction in block).
    // Also check for instructions which end blocks.
    for (int n = 0; n < icount; n++) {
        const auto &insn = program[n];
        int jip = -1, uip = -1;
        auto dests = insn.destinations(jip, uip);

        if (dests == DestNextIP)
            continue;

#ifdef NGEN_DEBUG_BB
        std::cerr << "Instruction " << n << " ->";
        if (dests & DestNextIP) std::cerr << " " << n + 1;
        if (dests & DestJIP) std::cerr << " " << n + jip;
        if (dests & DestUIP) std::cerr << " " << n + uip;
        std::cerr << std::endl;
#endif

        heads.insert({n + 1, 0});
        if (dests & DestJIP) heads.insert({n + jip, 0});
        if (dests & DestUIP) heads.insert({n + uip, 0});
    }

    // Create basic blocks and remember mapping from instruction #s to BBs.
    auto bbCount = uint32_t(heads.size());
    BasicBlockList list{bbCount};

    int nextBB = 0;
    for (auto &head : heads) {
        auto istart = head.first;
        if (istart >= 0 && istart < icount) {
            head.second = nextBB;
            list[nextBB].id = nextBB;
            list[nextBB++].istart = istart;
        }
    }

    bbCount = nextBB;
    list.resize(bbCount);

    for (uint32_t i = 0; i < bbCount - 1; i++)
        list[i].iend = list[i + 1].istart;
    list[bbCount - 1].iend = icount;

    // Scan through basic blocks again.
    for (auto &bb : list) {
        // Count in-order instructions in each pipe, and wrdep pseudo-instructions.
        for (auto &l : bb.lengths)
            l = 0;
        bb.wrdeps = 0;

        for (uint32_t n = bb.istart; n < bb.iend; n++) {
            const auto &insn = program[n];

            if (insn.opcode() == Opcode::wrdep)
                bb.wrdeps++;
            auto pipes = getPipeMask(hw, insn);
            for (int p = 0; p < NPipes; p++)
                if (pipes & (1 << p)) bb.lengths[p]++;
        }

        // Identify successor BBs from final instruction.
        auto ntail = bb.iend - 1;
        const auto &insn = program[ntail];
        int jip = 0, uip = 0;
        auto dests = insn.destinations(jip, uip);

        auto addSuccessor = [&](int inum) {
            if ((inum >= 0) && (inum < icount)) bb.succ.push_back(&list[heads[inum]]);
        };

        if (dests & DestNextIP) addSuccessor(bb.iend);
        if (dests & DestJIP)    addSuccessor(jip + ntail);
        if (dests & DestUIP)    addSuccessor(uip + ntail);

        // Add predecessor links to every successor.
        for (auto succ : bb.succ)
            succ->pred.push_back(&bb);
    }

    return list;
}

template <typename Instruction>
inline bool canDefaultPipe(HW hw, const Instruction &insn)
{
    if (hw >= HW::XeHP && insn.opcode() == Opcode::mov_gen12 && (insn.dstTypecode() ^ insn.src0Typecode()) & 0x8)
        return false;
    return true;
}

// Read SWSB from instruction and output:
//  * token dependency it produces, if any
//  * dependencies it consumes
//  * whether auto SWSB requested (bool return value)
// Assumes pipe information for this instruction already set up in consume dependency.
template <typename Instruction>
inline bool getSWSBDependencies(HW hw, const Instruction &insn, Dependency<false> &produce, Dependency<true> &consume)
{
    produce.token = 0;
    produce.tokenSrc = false;
    produce.tokenDst = false;
    consume.depPipe = PipeMaskNone;
    consume.dist = 0;
    consume.token = 0;
    consume.tokenSrc = false;
    consume.tokenDst = false;
    consume.swsb = true;

    SWSBInfo swsb = insn.swsb();
    bool autoSWSB = insn.autoSWSB();

    if (swsb.hasDist()) {
        auto pipe = swsb.getPipe();
        consume.depPipe =     (hw == HW::Gen12LP) ? PipeMaskA :
                          (pipe == Pipe::Default) ? getPipe(hw, insn).inOrderPipe()
                                                  : toMask(pipe);
        if (consume.depPipe) {      // if is here to ignore default pipe deps for OOO instructions.
            consume.dist = swsb.parts.dist;
            autoSWSB = false;
        }
    }
    if (swsb.hasToken()) {
        consume.token = swsb.parts.token;
        consume.tokenSrc = swsb.parts.src;
        consume.tokenDst = swsb.parts.dst;
        if (swsb.hasTokenSet()) {
            produce.token = consume.token;
            produce.tokenSrc = consume.tokenSrc;
            produce.tokenDst = consume.tokenDst;
        }
    }

    return autoSWSB;
}

// Encode SWSB information.
template <typename Instruction>
inline SWSBInfo encodeSWSB(HW hw, const Instruction &insn, const Dependency<false> &produce, const Dependency<true> &consume)
{
    SWSBInfo swsb{};

    if (produce.hasToken()) {
        swsb.parts.token = produce.token;
        swsb.parts.src = swsb.parts.dst = true;
    } else if (consume.tokenSrc) {
        swsb.parts.token = consume.token;
        swsb.parts.src = true;
    } else if (consume.tokenDst) {
        swsb.parts.token = consume.token;
        swsb.parts.dst = true;
    }

    if (consume.hasDist()) {
        if (canDefaultPipe(hw, insn) && ((hw == HW::Gen12LP) || (GeneralizedPipe(consume.depPipe) == consume.pipe)))
            swsb.setPipe(Pipe::Default);
        else
            swsb.setPipe(fromMask(consume.depPipe));
        swsb.parts.dist = std::min<int>(consume.dist, 7);
    }

    return swsb;
}

// Check if ARF src/dst requires special handling
inline bool arfNeedsSync(ARFType type)
{
    return (type == ARFType::ce || type == ARFType::cr || type == ARFType::sr);
}

// Get preferred SBID for a given GRF.
inline uint8_t preferredSBID(HW hw, uint8_t base)
{
    return (base >> 3) & 0xF;
}

// Choose SBID for an OOO instruction, based on preceding OOO instructions.
template <typename Program>
inline uint8_t chooseSBID(HW hw, Program &program, int32_t inum, int32_t counterA, const DependencyTable<false> &incoming, const DependencyTable<false> &producers, uint32_t maskDst)
{
    uint32_t unclaimed = (uint64_t(1) << tokenCount(hw)) - 1;
    std::array<int32_t, 16> pastExpiration;
    constexpr int32_t infinite = std::numeric_limits<int32_t>::max();

    // Priority 1: choose SBID that is an explicit dst dependency for this instruction, if any.
    if (maskDst)
        return utils::bsf(maskDst);

    // Otherwise, look through incoming OOO producers and accumulate most recent use of each token.
    for (auto &dist : pastExpiration) dist = infinite;

    auto accumulateTokens = [&](const Dependency<false> &dep) {
        if (!dep.hasToken()) return;

        auto depSWSB = program[dep.inum()].swsb();
        if (!depSWSB.hasTokenSet()) return;

        auto token = depSWSB.parts.token;
        unclaimed &= ~(1 << token);

        int32_t pe = counterA - (dep.counters[PipeBitA] + dep.tokenTime);
        pastExpiration[token] = std::min<int32_t>(pastExpiration[token], pe);
    };

    incoming.forEach(accumulateTokens);
    producers.forEach(accumulateTokens);

    int32_t bestPE = std::numeric_limits<int32_t>::min();
    uint8_t bestPESBID = 0;
    for (int token = 0; token < tokenCount(hw); token++) {
        if (pastExpiration[token] > bestPE) {
            bestPE = pastExpiration[token];
            bestPESBID = token;
        }
    }

    // Priority 2: assign SBID based on base register of dst, src1, src0 (in that order),
    //  if it's unclaimed or expired.
    for (int opNum : {-1, 1, 0}) {
        DependencyRegion region;
        region.hw = hw;
        if (program[inum].getOperandRegion(region, opNum) && (region.size > 0)) {
            auto sbid = preferredSBID(hw, region.base);
            if (pastExpiration[sbid] >= 0)
                return sbid;
        }
    }

    // Priority 3: choose highest-numbered unclaimed SBID.
    if (unclaimed)
        return utils::bsr(unclaimed);

    // Priority 4: choose token that's longest expired or closest to expiring.
    return bestPESBID;
}

// Main dependency analysis.
// This is run three times on every BB.
// Phase 0
//   Generate dependency tables for SBID assignment:
//      - produced OOO dependencies:  outgoing dependencies from this BB (w/o final SBIDs)
//      - consumed dependencies:  incoming dependencies this BB must synchronize on
// Phase 1
//   Input:
//      - incoming OOO dependencies, with expirations.
//   Output:
//      - produced dependencies:  outgoing dependencies this BB creates and does not synchronize on
//      - consumed dependencies:  incoming dependencies this BB must synchronize on
//      - SBIDs assigned where needed.
//   Instructions whose dependencies are all inside this BB are scoreboarded now for efficiency.
// Phase 2
//   Input: complete list of live dependencies.
//   All unscoreboarded instructions are reanalyzed and scoreboarded now.
template <typename Program>
inline void analyze(HW hw, Program &program, BasicBlock &bb, int phase)
{
    const bool final = (phase == 2);
    const bool computeSWSB = (phase > 0);
    bool forceA1 = false;
    int inumChain = -1;
    uint32_t chainTokenMaskSrc = 0, chainTokenMaskDst = 0, wrdepTokenMaskDst = 0;
    Dependency<true> chainGenerated;
    std::array<int32_t, NPipes> counters;
    std::vector<Dependency<false>> depList, chainProducers;

    // Initialize "preconsumes." These are region-less consumes arising from SWSB.
    int noPreconsume = std::numeric_limits<int>::min();
    std::array<std::array<int, NPipes + 1>, NPipes> preconsumeIO;
    uint32_t preconsumeTokenSrc = 0, preconsumeTokenDst = 0;

    auto recordIOPreconsumes = [&](Dependency<true> &generated) {
        if ((phase == 1) && generated.hasDist()) {
            auto spipes = generated.pipe.syncPipes(hw);
            auto dpipes = GeneralizedPipe(generated.depPipe).syncPipes(hw);
            for (int dpidx = 0; dpidx < NPipes; dpidx++)
                if (dpipes & (1 << dpidx))
                    for (int pidx = 0; pidx <= NPipes; pidx++)
                        if (spipes & (1 << pidx))
                            preconsumeIO[dpidx][pidx] = std::max<int>(preconsumeIO[dpidx][pidx], counters[dpidx] - generated.dist);
        }
    };

    if (phase == 1)
        for (auto &pcList : preconsumeIO)
            for (auto &pc : pcList)
                pc = noPreconsume;

    // Initialize counters.
    for (auto &counter : counters)
        counter = 0;

    for (uint32_t inum = bb.istart; inum < bb.iend; inum++) {
        auto &insn = program[inum];
        bool forceA1Next = false;
        bool atChainStart = false;

        // Ignore illegal instructions.
        if (insn.opcode() == Opcode::illegal)
            continue;

        // Process wrdep pseudo-instructions, which add OOO write dependencies on the next real instruction.
        if (insn.opcode() == Opcode::wrdep) {
            DependencyRegion region;
            if (insn.getOperandRegion(region, 0))
                wrdepTokenMaskDst |= bb.producers.removeOOOWritesByRegion(region);
            continue;
        }

        // Placeholder for dependency consumers from this instruction's operands.
        Dependency<true> consumeOp;
        consumeOp.counters = counters;
        consumeOp.pipe = getPipe(hw, insn);

        // Read SWSB information for this instruction, if already present.
        Dependency<false> tokenInfo;
        Dependency<true> generated = consumeOp;
        bool autoSWSB = getSWSBDependencies(hw, insn, tokenInfo, generated);

        // Check for beginning of {Atomic} chain.
        if (insn.atomic() && inumChain < 0) {
            inumChain = inum;
            atChainStart = true;
        }

        // If token assigned, start by removing all live dependencies with this token.
        if (tokenInfo.hasToken()) {
            bb.producers.removeByTokenMask(1 << tokenInfo.token, true);
            preconsumeTokenSrc |= (1 << tokenInfo.token);
            preconsumeTokenDst |= (1 << tokenInfo.token);
        } else if (isVariableLatency(hw, insn.opcode())) {
            generated.token = tokenInfo.token = tokenInfo.tokenTBD;
            tokenInfo.tokenSrc = tokenInfo.tokenDst = true;
        }

        // For sync.allrd/sync.allwr, consume matching dependencies and add preconsumes
        //   for unmatched tokens.
        if (insn.opcode() == Opcode::sync) {
            auto fc = insn.syncFC();
            bool allrd = (fc == SyncFunction::allrd);
            bool allwr = (fc == SyncFunction::allwr);

            if (allrd || allwr) {
                uint32_t imm;
                if (!insn.getImm32(imm))
                    imm = ~0;

                auto unmatched = bb.producers.removeByTokenMask(imm, allwr);
                preconsumeTokenSrc |= unmatched;
                if (allwr) preconsumeTokenDst |= unmatched;
            }
        }

        // Decode all regions.
        DependencyRegion regions[4];
        for (int srcN = -1; srcN < 3; srcN++) {
            regions[srcN + 1].hw = hw;
            if (!insn.getOperandRegion(regions[srcN + 1], srcN))
                regions[srcN + 1].clear();
        }

        // Check for cr/ce/sr destination operand, and force A@1 on the next instruction.
        ARFType dstARFType;
        forceA1Next |= (insn.getARFType(dstARFType, -1) && arfNeedsSync(dstARFType));

        if (autoSWSB) {
            // If auto-SWSB has been requested for this instruction, analyze its source operands.
            // Start a list of dependencies for this instruction.
            depList.clear();
            bool foundAllDeps = true;
            uint32_t tokenMaskSrc = 0, tokenMaskDst = 0;
            SWSBInfo syncSWSB;

            if (!atChainStart && (inumChain >= 0)) {
                tokenMaskSrc = chainTokenMaskSrc;
                tokenMaskDst = chainTokenMaskDst;
                generated = chainGenerated;
            }

            // Add in OOO dst dependencies from previous wrdep(s).
            tokenMaskDst |= wrdepTokenMaskDst;
            wrdepTokenMaskDst = 0;

            // Jumps with unknown destination: preconsume all dependencies.
            if (inum == (bb.iend - 1)) {
                int jip, uip;
                if (insn.destinations(jip, uip) & DestUnknown) {
                    tokenMaskDst = preconsumeTokenDst = (uint64_t(1) << tokenCount(hw)) - 1;
                    for (auto &p : preconsumeIO[PipeBitA])
                        p = 0;
                    bb.producers.clear();
                    bb.consumers.clear();
                    syncSWSB = (hw == HW::Gen12LP) ? SWSB(1) : SWSB<AllPipes>(1);
                }
            }

            // Analyze operands.
            for (int srcN = -1; srcN < 3; srcN++) {
                // Skip non-GRF operands.
                // Special case: check for cr/sr/ce source operands and force A@1 if any.
                if (regions[srcN + 1].empty()) {
                    ARFType arfType;
                    if ((srcN >= 0) && insn.getARFType(arfType, srcN) && arfNeedsSync(arfType)) {
                        generated.depPipe = PipeMaskA;
                        generated.dist = 1;
                    }
                    continue;
                }

                // Create associated dependency consumer.
                consumeOp.rw = (srcN < 0);
                consumeOp.region = regions[srcN + 1];

                // Remove all intersecting live producers from the table and save them.
                auto dStart = depList.size();
                bb.producers.findAndRemoveIntersections(consumeOp, &depList, hw);
                auto dEnd = depList.size();

                // If not final, subtract each of them from original dependency region.
                // If anything remains, add to consumer table. If it is not implied
                //   by existing consumers, we didn't find all dependencies.
                if (!final) {
                    for (auto d = dStart; d < dEnd; d++)
                        consumeOp.region.subtract(depList[d].region);
                    if (!consumeOp.region.empty())
                        foundAllDeps &= !bb.consumers.insertWeak(consumeOp);
                }

                // Add dependencies to SWSB.
                if (computeSWSB) for (auto d = dStart; d < dEnd; d++) {
                    auto &dep = depList[d];
                    if (dep.pipe.inOrder()) {
                        // Accumulate in-order dependencies.
                        auto thisPipe = dep.pipe.inOrderPipe();
                        auto thisDist = distance(dep, generated, thisPipe);

                        if (generated.depPipe == PipeMaskNone)
                            generated.depPipe = thisPipe;
                        else if (generated.depPipe != thisPipe)
                            generated.depPipe = PipeMaskA;

                        if (generated.hasDist())
                            generated.dist = std::min<int>(generated.dist, thisDist);
                        else
                            generated.dist = thisDist;
                    } else if (dep.token != dep.tokenTBD) {
                        // Remember out-of-order dependencies for later.
                        if (dep.tokenSrc) tokenMaskSrc |= (1 << dep.token);
                        if (dep.tokenDst) tokenMaskDst |= (1 << dep.token);
                    }
                }
            }

            // Transfer dependencies down the {Atomic} chain (will be put later on first instruction).
            if (insn.atomic()) {
                chainTokenMaskSrc = tokenMaskSrc;
                chainTokenMaskDst = tokenMaskDst;
                chainGenerated = generated;
                tokenMaskSrc = tokenMaskDst = 0;
                generated = consumeOp;
            }

            // If token missing on OOO instruction, assign one during phase 1.
            if ((phase == 1) && isVariableLatency(hw, insn.opcode()) && (tokenInfo.token == tokenInfo.tokenTBD) && !insn.atomic()) {
                auto newToken = chooseSBID(hw, program, inum, counters[PipeBitA], bb.incoming, bb.producers, tokenMaskDst);
                generated.token = tokenInfo.token = newToken;
                generated.tokenSrc = generated.tokenDst = true;
                insn.setSWSB(SBID(generated.token).set);
                preconsumeTokenSrc |= (1 << tokenInfo.token);
                preconsumeTokenDst |= (1 << tokenInfo.token);
                tokenMaskSrc &= ~(1 << tokenInfo.token);
                tokenMaskDst &= ~(1 << tokenInfo.token);
            }

            // Finalize SWSB computation.
            if (computeSWSB) {
                bool recordSWSB = (final || foundAllDeps);
                bool tokenAssigned = tokenInfo.hasToken() && (tokenInfo.token != tokenInfo.tokenTBD);

                // If last instruction forced A@1, enforce now.
                if (forceA1) {
                    generated.depPipe = PipeMaskA;
                    generated.dist = 1;
                    if (tokenMaskSrc || tokenMaskDst) {
                        bb.producers.removeIntersections(generated, hw);
                        generated.depPipe = PipeMaskNone;
                        generated.dist = 0;
                        auto swsb = (hw == HW::Gen12LP) ? SWSB(1) : SWSB<AllPipes>(1);
                        if (recordSWSB)
                            bb.syncs.push_back({uint32_t(inum), swsb, SyncFunction::nop, 0});
                    }
                }

                // If dual dependency (token + pipe) on OOO instruction, use A pipe for send, sync for others.
                if ((generated.hasToken() || tokenAssigned) && generated.hasDist()) {
                    if (insn.opcode() == Opcode::send || insn.opcode() == Opcode::sendc) {
                            generated.depPipe = PipeMaskA;
                    } else {
                        auto distGen = generated;
                        distGen.tokenSrc = distGen.tokenDst = false;
                        syncSWSB = encodeSWSB(hw, insn, Dependency<false>(), distGen);
                        generated.dist = 0;
                        generated.depPipe = PipeMaskNone;
                    }
                }

                // Handle OOO shootdown. Unless predicate is (W), it's possible that our token won't be claimed.
                // In this case, add sync on our token as a precaution. TODO: should check producer table.
                if (tokenAssigned && (insn.predicated() || inumChain >= 0))
                    tokenMaskDst |= (1 << tokenInfo.token);

                // Handle OOO dependencies.
                //    - dst implies src
                //    - use SWSB to mark src/dst w/o dist (in-order or no token) or dst + dist (in-order only, same pipe)
                //    - add sync for any remaining dependencies.
                tokenMaskSrc &= ~tokenMaskDst;

                bool defaultPipe = generated.pipe.inOrder() && (generated.depPipe == generated.pipe.inOrderPipe())
                                                            && canDefaultPipe(hw, insn);

                bool acceptsSrc = (!tokenAssigned || generated.pipe.inOrder()) && (generated.depPipe == PipeMaskNone);
                bool acceptsDst = acceptsSrc || defaultPipe;

                if (tokenMaskDst && acceptsDst) {
                    generated.token = utils::bsr(tokenMaskDst);
                    generated.tokenDst = true;
                    tokenMaskDst &= ~(1 << generated.token);
                } else if (tokenMaskSrc && acceptsSrc) {
                    generated.token = utils::bsr(tokenMaskSrc);
                    generated.tokenSrc = true;
                    tokenMaskSrc &= ~(1 << generated.token);
                }

                bool oneSrc = tokenMaskSrc && utils::is_zero_or_pow2(tokenMaskSrc);
                bool oneDst = tokenMaskDst && utils::is_zero_or_pow2(tokenMaskDst);
                bool oneSrcSWSB = false, oneDstSWSB = false;
                auto inumSync = (inumChain >= 0) ? inumChain : inum;

                if (syncSWSB.empty()) {
                    if (oneSrc) {
                        syncSWSB = SBID(utils::bsr(tokenMaskSrc)).src;
                        oneSrcSWSB = true;
                    } else if (oneDst) {
                        syncSWSB = SBID(utils::bsr(tokenMaskDst)).dst;
                        oneDstSWSB = true;
                    }
                }
                if (tokenMaskSrc && !oneSrcSWSB) {
                    if (recordSWSB)
                        bb.syncs.push_back({uint32_t(inumSync), syncSWSB, SyncFunction::allrd, tokenMaskSrc});
                    syncSWSB = SWSBInfo();
                }
                if (tokenMaskDst && !oneDstSWSB) {
                    if (recordSWSB)
                        bb.syncs.push_back({uint32_t(inumSync), syncSWSB, SyncFunction::allwr, tokenMaskDst});
                    syncSWSB = SWSBInfo();
                }
                if (!syncSWSB.empty() && recordSWSB)
                    bb.syncs.push_back({uint32_t(inumSync), syncSWSB, SyncFunction::nop, 0});

                // If final or nothing added to consumer table, assign SWSB.
                // For {Atomic} chains, put SWSB for consumed dependencies at head of chain.
                if (recordSWSB) {
                    if (inumChain >= 0) {
                        if (!insn.atomic()) {
                            program[inumChain].setSWSB(encodeSWSB(hw, insn, Dependency<false>(), generated));
                            insn.setSWSB(encodeSWSB(hw, insn, tokenInfo, Dependency<true>()));
                        }
                    } else
                        insn.setSWSB(encodeSWSB(hw, insn, tokenInfo, generated));
                    insn.clearAutoSWSB();
                }

                // After assigning SWSB to in-order instructions, clean producer list of known SWSB and sync dependencies.
                if (tokenMaskSrc) bb.producers.removeByTokenMask(tokenMaskSrc, false);
                if (tokenMaskDst) bb.producers.removeByTokenMask(tokenMaskDst, true);
                bb.producers.removeIntersections(generated, hw);
            }
        } else {
            // SWSB specified. Consume any dependencies associated with this SWSB.
            bb.producers.removeIntersections(generated, hw);

            // Record token dependencies for populating the consumer table.
            if (!final) {
                if (generated.tokenSrc) preconsumeTokenSrc |= (1 << tokenInfo.token);
                if (generated.tokenDst) preconsumeTokenDst |= (1 << tokenInfo.token);
            }

            // Consume destination dependencies too.
            consumeOp.region.hw = hw;
            if (insn.getOperandRegion(consumeOp.region, -1)) {
                consumeOp.rw = true;
                bb.producers.removeIntersections(consumeOp, hw);
            }

            // Clear auto-SWSB bit if it was set.
            if (phase == 2)
                insn.clearAutoSWSB();
        }

        // First pass: record pipeline SWSB dependencies for later entry into consumer table.
        recordIOPreconsumes(generated);

        // Add producer dependencies for all operands.
        // Also record instruction number and token timeout.
        // During phase 0, only do this for OOO instructions, and if dst not null, only dst.
        if ((phase > 0) || tokenInfo.hasToken()) {
            auto produceOp = consumeOp.cast();
            if (tokenInfo.hasToken()) {
                produceOp.token = tokenInfo.token;
                produceOp.tokenTime = estimateLatency(hw, insn);
                produceOp.inum() = inum;
            }

            for (int srcN = -1; srcN < 3; srcN++) {
                if (!regions[srcN + 1].empty()) {
                    produceOp.rw = (srcN < 0);
                    if (tokenInfo.hasToken()) {
                        produceOp.tokenSrc = (srcN >= 0);
                        produceOp.tokenDst = (srcN < 0);
                    }
                    produceOp.region = regions[srcN + 1];
                    if (insn.atomic())
                        chainProducers.push_back(produceOp);
                    else
                        bb.producers.insertStrong(produceOp);
                    if (phase == 0 && srcN == -1) break;
                }
            }

            // Add producers for previous instructions in {Atomic} chain.
            if (!insn.atomic()) {
                for (auto &op : chainProducers) {
                    if (!op.pipe.inOrder() || op.hasToken())
                        op.token = tokenInfo.token;
                    bb.producers.insertStrong(op);
                }
                chainProducers.clear();
            }
        }

        // Check for end of {Atomic} chain.
        if (!insn.atomic())
            inumChain = -1;

        // Increment counters.
        auto pipeMask = getPipeMask(hw, insn);
        for (int pidx = 0; pidx < NPipes; pidx++)
            if (pipeMask & (1 << pidx))
                counters[pidx]++;

        forceA1 = forceA1Next;
    }

    // Add preconsume dependencies to consume list.
    if (!final) {
        // In-order preconsumes.
        if (phase == 1) for (int pOld = 0; pOld < NPipes; pOld++) {
            for (int pNew = 0; pNew <= NPipes; pNew++) {
                auto pc = preconsumeIO[pOld][pNew];
                if (pc != noPreconsume) {
                    Dependency<true> preconsume;
                    preconsume.swsb = true;
                    preconsume.counters[pOld] = pc + 1;
                    preconsume.dist = 1;
                    preconsume.pipe = (1 << pNew);
                    preconsume.depPipe = (1 << pOld);
                    bb.consumers.insertStrong(preconsume);
                }
            }
        }
        // Out of order preconsumes.
        auto preconsumeToken = preconsumeTokenSrc | preconsumeTokenDst;
        for (int token = 0; token < tokenCount(hw); token++) {
            if (preconsumeToken & (1 << token)) {
                Dependency<true> preconsume;
                preconsume.swsb = true;
                preconsume.token = token;
                preconsume.tokenSrc = (preconsumeTokenSrc & (1 << token)) != 0;
                preconsume.tokenDst = (preconsumeTokenDst & (1 << token)) != 0;
                bb.consumers.insertStrong(preconsume);
            }
        }
    }
}

// Loop optimization. Add synchronizations before entering suspected loops to allow
//  weaker SWSB inside the loop.
inline void loopOptimize(BasicBlock &bb)
{
    // Loop through successors to this BB, looking for ones with
    //   exactly one incoming backedge, not from this BB.
    // If any found, for every dep in produce table:
    //   For each selector successor:
    //     If backedge pred's produce table doesn't imply this dep,
    //     add syncs to consume it.
}

// Propagate live dependencies forward through BB flow graph.
inline void propagate(std::vector<BasicBlock> &BBs)
{
    auto bbCount = int(BBs.size());
    bool done = false;
    std::vector<Dependency<true>> consumerList;

    // Mark all incoming dependencies as new.
    for (auto &bb : BBs) {
        bb.label = 0;
        bb.producers.forEach([](Dependency<false> &dep) {
            dep.label = 0;
        });
    }

    // Main loop: propagate live dependencies until all live tables stabilize.
    // This should require no more than bbCount loops.
    for (int age = 0; (age < bbCount) && !done; age++) {
        done = true;
        for (auto &bb : BBs) {
            // Examine each predecessor of this BB.
            for (auto pred : bb.pred) {
                if (pred->label < age) continue;

                pred->producers.forEach([&](const Dependency<false> &dep) {
                    // New incoming dependency? If not, skip it.
                    if (dep.label != age) return;

#ifdef NGEN_DEBUG_PROPAGATE
                    std::cerr << "Prop BB " << pred->id << " -> " << bb.id << ": ";
                    dep.dump();
#endif

                    // Adjust counters. Exception: OOO tokenless dependencies: counter[0] stores instruction #.
                    auto newDep = dep;
                    if (newDep.tokenTime == 0)
                        for (int p = 0; p < NPipes; p++)
                            newDep.counters[p] -= pred->lengths[p];

                    // If an in-order dependency, check for timeout, and skip it if so.
                    if (newDep.pipe.inOrder()) {
                        auto pidx = utils::log2(newDep.pipe.inOrderPipe());
                        if (newDep.counters[pidx] <= -timeout(dep.pipe)) {
#ifdef NGEN_DEBUG_PROPAGATE
                            std::cerr << " timeout\n";
#endif
                            return;
                        }
                    }

                    // Intersect new dependency (producer) with killed (consumer) table.
                    // Subtract all intersections from dependency.
                    consumerList.clear();
                    bb.consumers.findIntersections(newDep, consumerList);

                    for (auto &consumer : consumerList) {
                        newDep.region.subtract(consumer.region);
                        if (newDep.region.empty()) {
#ifdef NGEN_DEBUG_PROPAGATE
                            std::cerr << " killed\n";
#endif
                            return;
                        }
                    }

#ifdef NGEN_DEBUG_PROPAGATE
                    std::cerr << " propagated\n";
#endif

                    // Dependency is new and was not consumed.
                    // Add to produce table unless it's already implied by existing producers.
                    newDep.label = age + 1;
                    if (bb.producers.insert(newDep)) {
                        done = false;
                        bb.label = age + 1;
                    }
                });
            }
        }
    }

#ifdef NGEN_SAFE
    if (!done) throw std::runtime_error("nGEN internal error: propagation failed.");
#endif

    // Perform final half-propagation step (tail-to-head) to accumulate incoming producers
    //  for each BB.
    for (auto &bb : BBs) {
        for (auto pred : bb.pred) {
            pred->producers.forEach([&](const Dependency<false> &dep) {
                // Adjust counters, except for OOO tokenless dependencies.
                auto newDep = dep;
                if (newDep.tokenTime == 0)
                    for (int p = 0; p < NPipes; p++)
                        newDep.counters[p] -= pred->lengths[p];

                // If an in-order dependency, check for timeout, and skip it if so.
                if (newDep.pipe.inOrder()) {
                    auto pidx = utils::log2(newDep.pipe.inOrderPipe());
                    if (newDep.counters[pidx] <= -timeout(dep.pipe))
                        return;
                }

                bb.incoming.insert(newDep);
            });
        }
    }
}

// Adjust jump targets for sync instruction insertions.
template <typename Program>
inline void adjustTargets(Program &program, BasicBlockList &list)
{
    std::map<int32_t, int32_t> shifts;

    int32_t shift = 0;
    for (auto &bb : list) {
        shifts.insert({bb.istart, shift});
        shift += int32_t(bb.syncs.size()) - bb.wrdeps;
    }

    shift = 0;
    for (auto &bb : list) {
        shift += int32_t(bb.syncs.size()) - bb.wrdeps;
        auto ntail = bb.iend - 1;
        auto &insn = program[ntail];
        int jip = -1, uip = -1;
        auto dests = insn.destinations(jip, uip);
        if (dests & DestJIP) insn.shiftJIP(shifts[ntail + jip] - shift);
        if (dests & DestUIP) insn.shiftUIP(shifts[ntail + uip] - shift);
    }
}

// Entrypoint for automatic software scoreboarding.
// Returns the list of basic blocks, containing information on sync instructions to insert.
template <typename Program>
inline BasicBlockList autoSWSB(HW hw, Program &program)
{
    if (!hasAutoSWSB(hw, program))
        return BasicBlockList();

    // Find basic blocks.
    BasicBlockList bbList = getBasicBlocks(hw, program);

#ifdef NGEN_DEBUG
    std::cerr << "BASIC BLOCKS\n";
    std::cerr << "------------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id;
        if (!bb.pred.empty()) {
            std::cerr << " <-";
            for (auto &pred : bb.pred)
                std::cerr << ' ' << pred->id;
        }
        if (!bb.succ.empty()) {
            std::cerr << " ->";
            for (auto &succ : bb.succ)
                std::cerr << ' ' << succ->id;
        }
        std::cerr << std::endl;
    }
    std::cerr << std::endl;
#endif

    // Analysis round 0: gather OOO instruction usage.
    for (auto &bb : bbList)
        analyze(hw, program, bb, 0);

#ifdef NGEN_DEBUG
    std::cerr << "ANALYZE PHASE 0\n";
    std::cerr << "---------------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id << std::endl;
        bb.consumers.dump();
        bb.producers.dump();
        std::cerr << std::endl;
    }
#endif

    // Propagate OOO dependency producers through BB graph.
    propagate(bbList);
    for (auto &bb : bbList) {
        bb.producers.clear();
        bb.consumers.clear();
    }

#ifdef NGEN_DEBUG
    std::cerr << "PROPAGATE\n";
    std::cerr << "---------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id << std::endl;
        bb.incoming.dump();
        std::cerr << std::endl;
    }
#endif

    // Analysis round 1: assign SBIDs and perform intra-BB analysis.
    for (auto &bb : bbList) {
        analyze(hw, program, bb, 1);
        bb.incoming.clear();
    }

#ifdef NGEN_DEBUG
    std::cerr << "ANALYZE PHASE 1\n";
    std::cerr << "---------------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id << std::endl;
        bb.consumers.dump();
        bb.producers.dump();
        std::cerr << std::endl;
    }
#endif

    // Loop optimization.
    for (auto &bb : bbList)
        loopOptimize(bb);

    // Propagate live dependency producers through BB graph.
    propagate(bbList);

    for (auto &bb : bbList) {
        std::swap(bb.incoming, bb.producers);
        bb.incoming.clear();
    }

#ifdef NGEN_DEBUG
    std::cerr << "PROPAGATE\n";
    std::cerr << "---------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id << std::endl;
        bb.producers.dump();
        std::cerr << std::endl;
    }
#endif

    // Analysis round 2: final SWSB assignment.
    for (auto &bb : bbList)
        analyze(hw, program, bb, 2);

#ifdef NGEN_DEBUG
    std::cerr << "ANALYZE PHASE 2\n";
    std::cerr << "---------------\n";
    for (auto &bb : bbList) {
        std::cerr << "Basic Block " << bb.id << std::endl;
        bb.consumers.dump();
        bb.producers.dump();
        std::cerr << std::endl;
    }
#endif

    // Adjust jump targets after sync insertions.
    adjustTargets(program, bbList);

    return bbList;
}

} /* namespace autoswsb */
} /* namespace ngen */

// Instruction interface:
// 	SWSBInfo swsb() const;
// 	void setSWSB(SWSBInfo swsb) const;
// 	Opcode opcode() const;
// 	SyncFunction syncFC() const;
//  SharedFunction sfid() const;
// 	DestinationMask destinations(int &jip, int &uip) const;
// 	bool getOperandRegion(DependencyRegion &region, int opNum) const; // returns false if no such operand.
// 	bool getImm32(uint32_t &imm) const;
//
// Program interface:
// 	Instruction operator[](int inum);
// 	size_t size() const;

#endif /* NGEN_AUTOSWSB_HPP */
