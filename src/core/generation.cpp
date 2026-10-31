// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/generation.h"

#include <cassert>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "word_math.h"

namespace blockchronicle {

// ---------------------------------------------------------------------------
// ActiveBitmap
// ---------------------------------------------------------------------------

ActiveBitmap::ActiveBitmap(const Geometry& geometry, Generation generation)
    : geometry_(geometry)
    , generation_(generation)
    , wordCount_(internal::wordCountFor(geometry.regionCount()))
    , words_(new std::atomic<Word>[wordCount_])
{
    // std::atomic's default constructor does not initialise the value before
    // C++20, so initialise explicitly rather than relying on value-init.
    for (std::size_t i = 0; i < wordCount_; ++i) {
        words_[i].store(0u, std::memory_order_relaxed);
    }
}

bool ActiveBitmap::markRegion(std::uint32_t region)
{
    if (region >= geometry_.regionCount()) {
        return false;
    }
    const std::size_t wordIndex = static_cast<std::size_t>(region) / kBitsPerWord;
    const std::uint32_t bit = region % kBitsPerWord;
    words_[wordIndex].fetch_or(Word{1} << bit, std::memory_order_relaxed);
    return true;
}

bool ActiveBitmap::markRegionRange(std::uint32_t firstRegion, std::uint32_t lastRegion)
{
    if (firstRegion > lastRegion || lastRegion >= geometry_.regionCount()) {
        return false;
    }

    const std::uint32_t firstWord = firstRegion / kBitsPerWord;
    const std::uint32_t lastWord = lastRegion / kBitsPerWord;

    for (std::uint32_t wordIndex = firstWord; wordIndex <= lastWord; ++wordIndex) {
        const std::uint32_t firstBit = (wordIndex == firstWord)
            ? (firstRegion % kBitsPerWord)
            : 0u;
        const std::uint32_t lastBit = (wordIndex == lastWord)
            ? (lastRegion % kBitsPerWord)
            : (kBitsPerWord - 1u);
        words_[wordIndex].fetch_or(internal::maskInclusive(firstBit, lastBit),
                                   std::memory_order_relaxed);
    }
    return true;
}

Status ActiveBitmap::markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    if (lengthBytes == 0u) {
        return Status::Ok;
    }

    RegionSpan span;
    const Status status = geometry_.spanOf(offsetBytes, lengthBytes, span);
    if (status != Status::Ok) {
        return status;
    }

    markRegionRange(span.firstRegion, span.lastRegion);
    return Status::Ok;
}

bool ActiveBitmap::test(std::uint32_t region) const
{
    if (region >= geometry_.regionCount()) {
        return false;
    }
    const std::size_t wordIndex = static_cast<std::size_t>(region) / kBitsPerWord;
    const std::uint32_t bit = region % kBitsPerWord;
    return (words_[wordIndex].load(std::memory_order_relaxed) & (Word{1} << bit)) != 0u;
}

std::uint64_t ActiveBitmap::countDirty() const
{
    // Approximate under concurrent marking: a mark that lands in a word already
    // counted is missed. Callers must not treat this as a durability signal, and
    // seal() re-reads every word under the exclusive lock.
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < wordCount_; ++i) {
        total += internal::popcountOfWord(words_[i].load(std::memory_order_relaxed));
    }
    return total;
}

bool ActiveBitmap::any() const
{
    for (std::size_t i = 0; i < wordCount_; ++i) {
        if (words_[i].load(std::memory_order_relaxed) != 0u) {
            return true;
        }
    }
    return false;
}

void ActiveBitmap::freeze(DirtyBitmap& out) const
{
    DirtyBitmap snapshot(geometry_);
    std::vector<Word> words(wordCount_, 0u);
    for (std::size_t i = 0; i < wordCount_; ++i) {
        words[i] = words_[i].load(std::memory_order_relaxed);
    }
    if (!words.empty()) {
        words.back() &= internal::tailMaskFor(geometry_.regionCount());
    }

    const Status status = snapshot.setWords(std::move(words));
    // Unreachable: markRegion/markRegionRange never set a bit above the tail.
    assert(status == Status::Ok);
    (void)status;

    out = std::move(snapshot);
}

// ---------------------------------------------------------------------------
// GenerationManager
// ---------------------------------------------------------------------------

GenerationManager::GenerationManager(const Geometry& geometry,
                                     Generation startGeneration,
                                     SealSink& sink)
    : geometry_(geometry)
    , sink_(sink)
    , active_(new ActiveBitmap(geometry, startGeneration))
{
}

bool GenerationManager::markRegion(std::uint32_t region)
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    return active_->markRegion(region);
}

Status GenerationManager::markWrite(std::uint64_t offsetBytes, std::uint64_t lengthBytes)
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    return active_->markWrite(offsetBytes, lengthBytes);
}

Status GenerationManager::seal()
{
    std::unique_lock<std::shared_mutex> guard(stateMutex_);
    return sealLocked();
}

Status GenerationManager::sealIfDirtyAtLeast(std::uint64_t threshold)
{
    std::unique_lock<std::shared_mutex> guard(stateMutex_);
    if (active_->countDirty() < threshold) {
        return Status::Ok;
    }
    return sealLocked();
}

Status GenerationManager::sealLocked()
{
    const Generation closing = active_->generation();

    // Refuse to wrap rather than reuse a generation number (invariant I2).
    if (closing == std::numeric_limits<Generation>::max()) {
        return Status::InvalidState;
    }

    DirtyBitmap frozen(geometry_);
    active_->freeze(frozen);

    // Durable before visible (invariant I3). If this fails, the active bitmap is
    // untouched and still collecting: the caller can retry without loss.
    const Status status = sink_.commitDelta(closing, frozen);
    if (status != Status::Ok) {
        return status;
    }

    history_[closing] = std::make_shared<const DirtyBitmap>(std::move(frozen));
    lastSealed_ = closing;

    // Only after publication does the new generation start accepting writes.
    active_.reset(new ActiveBitmap(geometry_, closing + 1u));
    return Status::Ok;
}

Generation GenerationManager::activeGeneration() const
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    return active_->generation();
}

Generation GenerationManager::lastSealedGeneration() const
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    return lastSealed_;
}

std::uint64_t GenerationManager::activeDirtyCount() const
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    return active_->countDirty();
}

Status GenerationManager::delta(Generation generation, SealedDelta& out) const
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    const std::map<Generation, SealedDelta>::const_iterator it = history_.find(generation);
    if (it == history_.end()) {
        return Status::NotFound;
    }
    out = it->second;
    return Status::Ok;
}

std::vector<Generation> GenerationManager::sealedGenerations() const
{
    std::shared_lock<std::shared_mutex> guard(stateMutex_);
    std::vector<Generation> generations;
    generations.reserve(history_.size());
    for (std::map<Generation, SealedDelta>::const_iterator it = history_.begin();
         it != history_.end();
         ++it) {
        generations.push_back(it->first);
    }
    return generations;
}

bool GenerationManager::eraseGeneration(Generation generation)
{
    std::unique_lock<std::shared_mutex> guard(stateMutex_);
    return history_.erase(generation) != 0u;
}

} // namespace blockchronicle
