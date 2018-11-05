#pragma once

#include "Promise.h"
#include "SequenceOperators.h"
#include "WatchSequence.h"

namespace AxiomCommon {

    template<class InternalSequence, class FilterMapFunctor>
    using FilterMapWatchSequence =
        WatchSequence<FilterMapSequence<typename InternalSequence::Sequence, FilterMapFunctor>,
                      typename InternalSequence::Events>;

    template<class InternalSequence, class FilterFunctor>
    using FilterWatchSequence = WatchSequence<FilterSequence<typename InternalSequence::Sequence, FilterFunctor>,
                                              typename InternalSequence::Events>;

    template<class InternalSequence, class MapFunctor>
    using MapWatchSequence =
        WatchSequence<MapSequence<typename InternalSequence::Sequence, MapFunctor>, typename InternalSequence::Events>;

    template<class Output, class InternalSequence>
    using CastWatchSequence =
        WatchSequence<CastSequence<Output, typename InternalSequence::Sequence>, typename InternalSequence::Events>;

    template<class Sequence, class FilterMapFunctor>
    FilterMapWatchSequence<Sequence, FilterMapFunctor> filterMapWatch(Sequence sequence, FilterMapFunctor functor) {
        return WatchSequence(filterMap(std::move(sequence.sequence()), std::move(functor)),
                             std::move(sequence.events()));
    }

    template<class Sequence, class FilterFunctor>
    FilterWatchSequence<Sequence, FilterFunctor> filterWatch(Sequence sequence, FilterFunctor functor) {
        return WatchSequence(filter(std::move(sequence.sequence()), std::move(functor)), std::move(sequence.events()));
    }

    template<class Sequence, class MapFunctor>
    MapWatchSequence<Sequence, MapFunctor> mapWatch(Sequence sequence, MapFunctor functor) {
        return WatchSequence(map(std::move(sequence.sequence()), std::move(functor)), std::move(sequence.events()));
    }

    template<class Output, class Sequence>
    CastWatchSequence<Output, Sequence> dynamicCastWatch(Sequence sequence) {
        return WatchSequence(dynamicCast<Output>(std::move(sequence.sequence())), std::move(sequence.events()));
    }

    template<class Output, class Sequence>
    CastWatchSequence<Output, Sequence> staticCastWatch(Sequence sequence) {
        return WatchSequence(staticCast<Output>(std::move(sequence.sequence())), std::move(sequence.events()));
    }

    template<class Output, class Sequence>
    CastWatchSequence<Output, Sequence> reinterpretCastWatch(Sequence sequence) {
        return WatchSequence(reinterpretCast<Output>(std::move(sequence.sequence())), std::move(sequence.events()));
    }

    template<class Sequence>
    std::shared_ptr<AxiomCommon::Promise<typename Sequence::Sequence::value_type>> getFirst(Sequence input) {
        using ValueType = typename Sequence::Sequence::value_type;

        auto result = std::make_shared<AxiomCommon::Promise<ValueType>>();

        // if there is a first item, return that
        auto firstItem = takeAt(AxiomCommon::refSequence(&input.sequence()), 0);
        if (firstItem) {
            result->resolve(*firstItem);
            return result;
        }

        auto heapSequence = std::make_shared<Sequence>(std::move(input));
        heapSequence->events().itemAdded().once(Sequence::Events::ItemEvent::to(
            [heapSequence, result](ValueType item) { result->resolve(std::move(item)); }));

        return result;
    }
}
