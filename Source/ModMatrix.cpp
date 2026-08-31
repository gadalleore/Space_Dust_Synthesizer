#include "ModMatrix.h"

#include <algorithm>
#include <cmath>

namespace
{
    float clampf (float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

namespace spacedust
{
    const ModRouting* ModMatrix::find (int lfoIndex, const std::string& destination) const
    {
        for (const auto& r : list)
            if (r.lfoIndex == lfoIndex && r.destination == destination)
                return &r;

        return nullptr;
    }

    void ModMatrix::setRouting (int lfoIndex, const std::string& destination, float amount)
    {
        if (lfoIndex < 0 || lfoIndex >= numLfos || destination.empty())
            return;

        const float a = clampf (amount, -maxRoutingAmount, maxRoutingAmount);

        // Zero means "not assigned". Storing it would leave an entry that draws
        // an indicator bar and moves nothing.
        if (a == 0.0f)
        {
            clearRouting (lfoIndex, destination);
            return;
        }

        for (auto& r : list)
        {
            if (r.lfoIndex == lfoIndex && r.destination == destination)
            {
                r.amount = a;
                return;
            }
        }

        list.push_back (ModRouting { lfoIndex, destination, a });
    }

    void ModMatrix::clearRouting (int lfoIndex, const std::string& destination)
    {
        list.erase (std::remove_if (list.begin(), list.end(),
                                    [&] (const ModRouting& r)
                                    {
                                        return r.lfoIndex == lfoIndex
                                            && r.destination == destination;
                                    }),
                    list.end());
    }

    void ModMatrix::clear()
    {
        list.clear();
    }

    float ModMatrix::amountFor (int lfoIndex, const std::string& destination) const
    {
        if (const auto* r = find (lfoIndex, destination))
            return r->amount;

        return 0.0f;
    }

    bool ModMatrix::hasAnyRouting (const std::string& destination) const
    {
        for (const auto& r : list)
            if (r.destination == destination)
                return true;

        return false;
    }

    float ModMatrix::applyByName (const std::string& destination, float base,
                                  DestRange range, const float* lfoValues) const noexcept
    {
        float sum = base;

        for (const auto& r : list)
        {
            if (r.destination != destination)
                continue;

            if (r.lfoIndex < 0 || r.lfoIndex >= numLfos)
                continue;

            sum += r.amount * lfoValues[r.lfoIndex] * range.halfRange();
        }

        return clampf (sum, range.start, range.end);
    }

    void ModMatrix::applyCompiled (const CompiledRouting* compiled, int numCompiled,
                                   const float* bases, const DestRange* ranges,
                                   int numDests, const float* lfoValues,
                                   float* out) noexcept
    {
        for (int i = 0; i < numDests; ++i)
            out[i] = bases[i];

        for (int i = 0; i < numCompiled; ++i)
        {
            const auto& c = compiled[i];

            if (c.destSlot < 0 || c.destSlot >= numDests)
                continue;

            out[c.destSlot] += c.scale * lfoValues[c.lfoIndex];
        }

        for (int i = 0; i < numDests; ++i)
            out[i] = clampf (out[i], ranges[i].start, ranges[i].end);
    }
}
