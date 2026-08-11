#include "ToolOrderUtils.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r
{
namespace
{
template<typename T>
std::vector<T> collect_filaments_in_group(const std::unordered_set<unsigned int> &group,
                                          const std::vector<unsigned int>         &filament_list)
{
    std::vector<T> result;
    for (unsigned int filament : filament_list)
        if (group.count(filament) != 0)
            result.emplace_back(static_cast<T>(filament));
    return result;
}
} // namespace

    std::vector<int> auto_group_filaments_for_minimum_flush(
        size_t filament_count,
        size_t nozzle_count,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<FlushMatrix>& flush_matrices,
        const std::vector<std::set<int>>& unprintable_filaments,
        const std::vector<int>& preferred_map,
        int default_nozzle,
        const std::vector<bool>& preserve_layer_sequence)
    {
        nozzle_count = std::min(nozzle_count, flush_matrices.size());
        if (filament_count == 0 || nozzle_count == 0)
            return {};

        default_nozzle = std::clamp(default_nozzle, 0, int(nozzle_count) - 1);
        std::vector<bool> used(filament_count, false);
        std::vector<std::vector<unsigned int>> normalized_layers;
        normalized_layers.reserve(layer_filaments.size());
        for (const std::vector<unsigned int>& layer : layer_filaments) {
            std::vector<unsigned int> normalized;
            std::vector<bool> layer_used(filament_count, false);
            for (unsigned int filament_id : layer) {
                if (filament_id < filament_count && !layer_used[filament_id]) {
                    normalized.emplace_back(filament_id);
                    layer_used[filament_id] = true;
                }
            }
            const bool preserve_sequence =
                normalized_layers.size() < preserve_layer_sequence.size() &&
                preserve_layer_sequence[normalized_layers.size()];
            if (!preserve_sequence)
                std::sort(normalized.begin(), normalized.end());
            for (unsigned int filament_id : normalized)
                used[filament_id] = true;
            normalized_layers.emplace_back(std::move(normalized));
        }

        std::vector<unsigned int> used_filaments;
        for (size_t filament_id = 0; filament_id < filament_count; ++filament_id)
            if (used[filament_id])
                used_filaments.emplace_back(unsigned(filament_id));

        std::vector<int> result(filament_count, default_nozzle);
        if (used_filaments.empty())
            return result;

        auto is_allowed = [&unprintable_filaments, nozzle_count](unsigned int filament_id, size_t nozzle_id) {
            return nozzle_id < nozzle_count &&
                (nozzle_id >= unprintable_filaments.size() || unprintable_filaments[nozzle_id].count(int(filament_id)) == 0);
        };

        // Match the BBL grouping cost model: filaments which coexist frequently
        // and require a large flush are expensive to leave on the same nozzle.
        std::vector<std::vector<unsigned int>> coexistence(filament_count, std::vector<unsigned int>(filament_count, 0));
        for (size_t layer_idx = 0; layer_idx < normalized_layers.size(); ++layer_idx) {
            if (layer_idx < preserve_layer_sequence.size() && preserve_layer_sequence[layer_idx])
                continue;
            const std::vector<unsigned int>& layer = normalized_layers[layer_idx];
            for (size_t i = 0; i < layer.size(); ++i)
                for (size_t j = i + 1; j < layer.size(); ++j) {
                    ++coexistence[layer[i]][layer[j]];
                    ++coexistence[layer[j]][layer[i]];
                }
        }

        std::vector<std::vector<std::vector<double>>> pair_cost(
            nozzle_count, std::vector<std::vector<double>>(filament_count, std::vector<double>(filament_count, 0.)));
        for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id) {
            const FlushMatrix& matrix = flush_matrices[nozzle_id];
            for (size_t from = 0; from < filament_count; ++from) {
                if (from >= matrix.size())
                    continue;
                for (size_t to = from + 1; to < filament_count; ++to) {
                    if (to >= matrix.size() || to >= matrix[from].size() || from >= matrix[to].size())
                        continue;
                    const double larger = std::max(matrix[from][to], matrix[to][from]);
                    const double smaller = std::min(matrix[from][to], matrix[to][from]);
                    pair_cost[nozzle_id][from][to] = pair_cost[nozzle_id][to][from] =
                        (larger * 0.65 + smaller * 0.35) * coexistence[from][to];
                }
            }
        }

        struct Score {
            double cost {0.};
            size_t tool_changes {0};
            size_t max_group_size {0};
        };
        auto score = [&](const std::vector<int>& map) {
            Score value;
            std::vector<size_t> group_sizes(nozzle_count, 0);
            for (unsigned int filament_id : used_filaments)
                if (map[filament_id] >= 0 && size_t(map[filament_id]) < nozzle_count)
                    ++group_sizes[map[filament_id]];
            value.max_group_size = *std::max_element(group_sizes.begin(), group_sizes.end());

            for (size_t i = 0; i < used_filaments.size(); ++i)
                for (size_t j = i + 1; j < used_filaments.size(); ++j) {
                    const unsigned int first = used_filaments[i];
                    const unsigned int second = used_filaments[j];
                    if (map[first] == map[second] && map[first] >= 0 && size_t(map[first]) < nozzle_count)
                        value.cost += pair_cost[map[first]][first][second];
                }

            // Replay only layers whose sequence is explicitly constrained by the user. Layers
            // without a custom sequence remain represented by the reorderable coexistence cost
            // above. Reset the persistent state across an unconstrained gap because that gap's
            // optimized ending filament is not known at grouping time.
            std::vector<int> loaded_filament(nozzle_count, -1);
            int active_nozzle = -1;
            bool previous_layer_preserved = false;
            for (size_t layer_idx = 0; layer_idx < normalized_layers.size(); ++layer_idx) {
                const bool preserve_sequence =
                    layer_idx < preserve_layer_sequence.size() && preserve_layer_sequence[layer_idx];
                if (!preserve_sequence) {
                    previous_layer_preserved = false;
                    continue;
                }
                if (!previous_layer_preserved) {
                    std::fill(loaded_filament.begin(), loaded_filament.end(), -1);
                    active_nozzle = -1;
                }
                for (unsigned int filament_id : normalized_layers[layer_idx]) {
                    const int nozzle_id = map[filament_id];
                    if (nozzle_id < 0 || size_t(nozzle_id) >= nozzle_count)
                        continue;

                    if (active_nozzle >= 0 && active_nozzle != nozzle_id)
                        ++value.tool_changes;

                    const int previous = loaded_filament[nozzle_id];
                    const FlushMatrix& matrix = flush_matrices[nozzle_id];
                    if (previous >= 0 && previous != int(filament_id) &&
                        size_t(previous) < matrix.size() && filament_id < matrix[size_t(previous)].size())
                        value.cost += matrix[size_t(previous)][filament_id];

                    loaded_filament[nozzle_id] = int(filament_id);
                    active_nozzle = nozzle_id;
                }
                previous_layer_preserved = true;
            }
            return value;
        };
        auto is_better = [](const Score& candidate, const Score& current) {
            constexpr double tolerance = 1e-6;
            if (candidate.cost + tolerance < current.cost)
                return true;
            if (std::abs(candidate.cost - current.cost) > tolerance)
                return false;
            if (candidate.max_group_size != current.max_group_size)
                return candidate.max_group_size < current.max_group_size;
            return candidate.tool_changes < current.tool_changes;
        };

        auto make_valid = [&](std::vector<int> map) {
            map.resize(filament_count, default_nozzle);
            for (unsigned int filament_id : used_filaments) {
                if (map[filament_id] >= 0 && size_t(map[filament_id]) < nozzle_count && is_allowed(filament_id, map[filament_id]))
                    continue;
                map[filament_id] = -1;
                for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id)
                    if (is_allowed(filament_id, nozzle_id)) {
                        map[filament_id] = int(nozzle_id);
                        break;
                    }
            }
            return map;
        };

        auto improve = [&](std::vector<int> map) {
            map = make_valid(std::move(map));
            for (size_t iteration = 0; iteration < filament_count * 2; ++iteration) {
                Score best_score = score(map);
                std::vector<int> best_map = map;
                for (unsigned int filament_id : used_filaments) {
                    for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id) {
                        if (int(nozzle_id) == map[filament_id] || !is_allowed(filament_id, nozzle_id))
                            continue;
                        std::vector<int> candidate = map;
                        candidate[filament_id] = int(nozzle_id);
                        const Score candidate_score = score(candidate);
                        if (is_better(candidate_score, best_score)) {
                            best_score = candidate_score;
                            best_map = std::move(candidate);
                        }
                    }
                }
                for (size_t i = 0; i < used_filaments.size(); ++i)
                    for (size_t j = i + 1; j < used_filaments.size(); ++j) {
                        const unsigned int first = used_filaments[i];
                        const unsigned int second = used_filaments[j];
                        if (map[first] == map[second] || !is_allowed(first, map[second]) || !is_allowed(second, map[first]))
                            continue;
                        std::vector<int> candidate = map;
                        std::swap(candidate[first], candidate[second]);
                        const Score candidate_score = score(candidate);
                        if (is_better(candidate_score, best_score)) {
                            best_score = candidate_score;
                            best_map = std::move(candidate);
                        }
                    }
                if (best_map == map)
                    break;
                map = std::move(best_map);
            }
            return map;
        };

        std::vector<std::vector<int>> seeds;
        if (!preferred_map.empty())
            seeds.emplace_back(preferred_map);
        for (size_t offset = 0; offset < nozzle_count; ++offset) {
            std::vector<int> round_robin(filament_count, default_nozzle);
            size_t next_nozzle = offset;
            for (unsigned int filament_id : used_filaments) {
                for (size_t attempt = 0; attempt < nozzle_count; ++attempt) {
                    const size_t nozzle_id = (next_nozzle + attempt) % nozzle_count;
                    if (is_allowed(filament_id, nozzle_id)) {
                        round_robin[filament_id] = int(nozzle_id);
                        next_nozzle = (nozzle_id + 1) % nozzle_count;
                        break;
                    }
                }
            }
            seeds.emplace_back(std::move(round_robin));
        }

        result = improve(std::move(seeds.front()));
        Score best_score = score(result);
        for (size_t seed_id = 1; seed_id < seeds.size(); ++seed_id) {
            std::vector<int> candidate = improve(std::move(seeds[seed_id]));
            const Score candidate_score = score(candidate);
            if (is_better(candidate_score, best_score)) {
                best_score = candidate_score;
                result = std::move(candidate);
            }
        }
        return result;
    }

    int reorder_generic_filaments_for_minimum_flush_volume(const std::vector<unsigned int>& filament_lists,
        const std::vector<int>& filament_maps,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<FlushMatrix>& flush_matrix,
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq,
        std::vector<std::vector<unsigned int>>* filament_sequences,
        const std::unordered_map<int, int>& nozzle_status)
    {
        //only when layer filament num <= 5,we do forcast
        constexpr int max_n_with_forcast = 5;
        int cost = 0;
        const size_t group_count = flush_matrix.size();
        if (group_count == 0)
            return 0;

        std::vector<std::unordered_set<unsigned int>> groups(group_count); // save the grouped filaments
        std::vector<std::vector<std::vector<unsigned int>>> layer_sequences(group_count); // reordered sequence by group
        std::map<size_t, std::vector<unsigned int>> custom_layer_sequence_map; // save the filament sequences of custom layer

        // group the filament
        const size_t mapped_filament_count = std::min(filament_maps.size(), filament_lists.size());
        for (size_t i = 0; i < mapped_filament_count; ++i) {
            const int group_id = filament_maps[i];
            if (group_id >= 0 && size_t(group_id) < group_count)
                groups[size_t(group_id)].insert(filament_lists[i]);
        }

        auto find_group_id = [&groups](unsigned int filament_id) -> size_t {
            for (size_t group_id = 0; group_id < groups.size(); ++group_id)
                if (groups[group_id].count(filament_id) != 0)
                    return group_id;
            return 0;
        };

        // store custom layer sequence
        for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
            const auto& curr_lf = layer_filaments[layer];

            std::vector<int>custom_filament_seq;
            if (get_custom_seq && (*get_custom_seq)(layer, custom_filament_seq) && !custom_filament_seq.empty()) {
                std::vector<unsigned int> unsign_custom_extruder_seq;
                for (int extruder : custom_filament_seq) {
                    unsigned int unsign_extruder = static_cast<unsigned int>(extruder) - 1;
                    auto it = std::find(curr_lf.begin(), curr_lf.end(), unsign_extruder);
                    if (it != curr_lf.end())
                        unsign_custom_extruder_seq.emplace_back(unsign_extruder);
                }
                assert(curr_lf.size() == unsign_custom_extruder_seq.size());

                custom_layer_sequence_map[layer] = unsign_custom_extruder_seq;
            }
        }
        using uint128_t = boost::multiprecision::uint128_t;
        auto extruders_to_hash_key = [](const std::vector<unsigned int>& curr_layer_extruders,
           const std::vector<unsigned int>& next_layer_extruders,
           const std::optional<unsigned int>& prev_extruder,
           bool use_forcast)->uint128_t
           {
               uint128_t hash_key = 0;
               //31-0 bit define current layer extruder,63-32 bit define next layer extruder,95~64 define prev extruder
               if (prev_extruder)
                   hash_key |= (uint128_t(1) << (64 + *prev_extruder));

               if (use_forcast) {
                   for (auto item : next_layer_extruders)
                       hash_key |= (uint128_t(1) << (32 + item));
               }

               for (auto item : curr_layer_extruders)
                   hash_key |= (uint128_t(1) << item);
               return hash_key;
           };


        // get best layer sequence by group
        for (size_t idx = 0; idx < groups.size(); ++idx) {
            // case with one group
            if (groups[idx].empty())
                continue;
            std::optional<unsigned int>current_extruder_id;
            // seed the group (nozzle) with the filament already loaded, if nozzle_status supplies one
            if (auto it = nozzle_status.find(static_cast<int>(idx)); it != nozzle_status.end() && it->second >= 0) {
                unsigned int initial_fil = static_cast<unsigned int>(it->second);
                if (initial_fil < flush_matrix[idx].size())
                    current_extruder_id = initial_fil;
            }

            std::unordered_map<uint128_t, std::pair<float, std::vector<unsigned int>>> caches;

            for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
                const auto& curr_lf = layer_filaments[layer];

                if (auto iter = custom_layer_sequence_map.find(layer); iter != custom_layer_sequence_map.end()) {
                    auto sequence_in_group = collect_filaments_in_group<unsigned int>(groups[idx], iter->second);

                    float tmp_cost = 0;
                    std::optional<unsigned int>prev = current_extruder_id;
                    for (auto& f : sequence_in_group) {
                        if (prev) { tmp_cost += flush_matrix[idx][*prev][f]; }
                        prev = f;
                    }
                    cost += tmp_cost;

                    if (!sequence_in_group.empty())
                        current_extruder_id = sequence_in_group.back();
                    //insert an empty array
                    if (filament_sequences)
                        layer_sequences[idx].emplace_back(std::vector<unsigned int>());

                    continue;
                }

                std::vector<unsigned int>filament_used_in_group = collect_filaments_in_group<unsigned int>(groups[idx], curr_lf);

                std::vector<unsigned int>next_lf;
                if (layer + 1 < layer_filaments.size())
                    next_lf = layer_filaments[layer + 1];
                std::vector<unsigned int>filament_used_in_group_next_layer = collect_filaments_in_group<unsigned int>(groups[idx], next_lf);

                bool use_forcast = (filament_used_in_group.size() <= max_n_with_forcast && filament_used_in_group_next_layer.size() <= max_n_with_forcast);
                float tmp_cost = 0;
                std::vector<unsigned int>sequence;
                uint128_t hash_key = extruders_to_hash_key(filament_used_in_group, filament_used_in_group_next_layer, current_extruder_id, use_forcast);
                if (auto iter = caches.find(hash_key); iter != caches.end()) {
                    tmp_cost = iter->second.first;
                    sequence = iter->second.second;
                }
                else {
                    sequence = get_extruders_order(flush_matrix[idx], filament_used_in_group, filament_used_in_group_next_layer, current_extruder_id, use_forcast, &tmp_cost);
                    caches[hash_key] = { tmp_cost,sequence };
                }

                assert(sequence.size() == filament_used_in_group.size());

                if (filament_sequences)
                    layer_sequences[idx].emplace_back(sequence);

                if (!sequence.empty())
                    current_extruder_id = sequence.back();
                cost += tmp_cost;
            }
        }

        // get the final layer sequences
        // if only have one group,we need to check whether layer sequence[idx] is valid
        if (filament_sequences) {
            filament_sequences->clear();
            filament_sequences->resize(layer_filaments.size());
            size_t last_group_id = 0;
            if (!custom_layer_sequence_map.empty()) {
                const auto& first_layer = custom_layer_sequence_map.begin()->first;
                const auto& first_layer_filaments = custom_layer_sequence_map.begin()->second;
                assert(!first_layer_filaments.empty());

                const size_t first_group = find_group_id(first_layer_filaments.front());
                last_group_id = (first_group + group_count - first_layer % group_count) % group_count;
            }

            for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
                auto& curr_layer_seq = (*filament_sequences)[layer];
                if (custom_layer_sequence_map.find(layer) != custom_layer_sequence_map.end()) {
                    curr_layer_seq = custom_layer_sequence_map[layer];
                    if (!curr_layer_seq.empty())
                        last_group_id = find_group_id(curr_layer_seq.back());
                    continue;
                }

                // Reuse the nozzle which finished the preceding layer first,
                // then visit all remaining non-empty nozzle groups cyclically.
                const size_t start_group_id = last_group_id;
                for (size_t offset = 0; offset < group_count; ++offset) {
                    const size_t group_id = (start_group_id + offset) % group_count;
                    if (!layer_sequences[group_id].empty() && !layer_sequences[group_id][layer].empty()) {
                        const auto &sequence = layer_sequences[group_id][layer];
                        curr_layer_seq.insert(curr_layer_seq.end(), sequence.begin(), sequence.end());
                        last_group_id = group_id;
                    }
                }
            }
        }

        return cost;
    }

} // namespace Slic3r
