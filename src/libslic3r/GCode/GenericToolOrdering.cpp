#include "ToolOrdering.hpp"

#include "../FilamentGroupUtils.hpp"
#include "../I18N.hpp"
#include "../ParameterUtils.hpp"
#include "../Print.hpp"
#include "../Utils.hpp"
#include "ToolOrderUtils.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace Slic3r
{
#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif

namespace
{
std::vector<FlushMatrix> prepare_generic_flush_matrices(const PrintConfig &print_config)
{
    const size_t nozzle_count   = print_config.nozzle_diameter.values.size();
    const size_t filament_count = print_config.filament_colour.values.size();
    if (filament_count > 0 && filament_count > std::numeric_limits<size_t>::max() / filament_count)
        throw Slic3r::RuntimeError(_L("Invalid filament count while preparing the flushing-volume matrix."));

    const size_t matrix_size_per_nozzle = filament_count * filament_count;
    if (nozzle_count > 0 && matrix_size_per_nozzle > std::numeric_limits<size_t>::max() / nozzle_count)
        throw Slic3r::RuntimeError(_L("Invalid nozzle count while preparing the flushing-volume matrix."));

    const size_t expected_matrix_size = matrix_size_per_nozzle * nozzle_count;
    if (print_config.flush_volumes_matrix.values.size() != expected_matrix_size)
        throw Slic3r::RuntimeError(
            _L("The flushing-volume matrix size does not match the configured filament and nozzle counts."));

    std::vector<FlushMatrix> flush_matrices;
    flush_matrices.reserve(nozzle_count);
    for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id) {
        std::vector<float> matrix(cast<float>(
            get_flush_volumes_matrix(print_config.flush_volumes_matrix.values, nozzle_id, nozzle_count)));
        FlushMatrix wipe_volumes;
        wipe_volumes.reserve(filament_count);
        for (size_t filament_id = 0; filament_id < filament_count; ++filament_id) {
            wipe_volumes.emplace_back(
                matrix.begin() + filament_id * filament_count,
                matrix.begin() + (filament_id + 1) * filament_count);
        }
        flush_matrices.emplace_back(std::move(wipe_volumes));
    }

    auto multipliers = print_config.prime_volume_mode == PrimeVolumeMode::pvmFast ?
                           print_config.flush_multiplier_fast.values :
                           print_config.flush_multiplier.values;
    multipliers.resize(nozzle_count, 1.0);
    for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id)
        for (std::vector<float> &row : flush_matrices[nozzle_id])
            for (float &value : row)
                value *= multipliers[nozzle_id];

    return flush_matrices;
}
} // namespace

MultiNozzleUtils::LayeredNozzleGroupResult ToolOrdering::get_recommended_generic_filament_maps(
    const std::vector<std::vector<unsigned int>> &layer_filaments,
    const Print                                  *print,
    FilamentMapMode                              mode,
    const std::vector<std::set<int>>             &geometric_unprintables)
{
    using namespace MultiNozzleUtils;

    const PrintConfig &config = print->config();
    const size_t filament_count = config.filament_colour.values.size();
    const size_t nozzle_count = config.nozzle_diameter.values.size();
    if (nozzle_count == 0)
        return LayeredNozzleGroupResult();
    if (has_different_nozzle_diameters(config) && mode != fmmManual && mode != fmmNozzleManual)
        throw Slic3r::RuntimeError(
            _L("Automatic filament grouping is unavailable when nozzle diameters differ. Please use manual grouping."));

    const std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(layer_filaments);
    const auto nozzle_list = build_default_nozzle_list(config, nozzle_count);
    auto validate_map = [&used_filaments, &geometric_unprintables, nozzle_count](const std::vector<int> &filament_map) {
        for (unsigned int filament_id : used_filaments) {
            if (filament_id >= filament_map.size())
                throw Slic3r::RuntimeError(_L("Filament grouping contains a missing nozzle assignment."));

            const int nozzle_id = filament_map[filament_id];
            if (nozzle_id < 0 || size_t(nozzle_id) >= nozzle_count)
                throw Slic3r::RuntimeError(_L("Filament grouping contains an invalid nozzle assignment."));
            if (size_t(nozzle_id) < geometric_unprintables.size() &&
                geometric_unprintables[nozzle_id].count(int(filament_id)) > 0)
                throw Slic3r::RuntimeError(_L("The assigned nozzle cannot print one or more filaments. Please regroup the filaments."));
        }
    };

    if (mode == FilamentMapMode::fmmManual || mode == FilamentMapMode::fmmNozzleManual) {
        std::vector<int> manual_map = config.filament_map.values;
        manual_map.resize(filament_count, 1);
        std::transform(manual_map.begin(), manual_map.end(), manual_map.begin(), [](int value) { return value - 1; });
        validate_map(manual_map);
        auto result = LayeredNozzleGroupResult::create(manual_map, nozzle_list, used_filaments);
        return result ? *result : LayeredNozzleGroupResult();
    }

    std::vector<std::set<int>> unprintable_filaments(nozzle_count);
    for (size_t nozzle_id = 0; nozzle_id < nozzle_count && nozzle_id < geometric_unprintables.size(); ++nozzle_id)
        unprintable_filaments[nozzle_id] = geometric_unprintables[nozzle_id];

    std::vector<int> preferred_map = config.filament_map.values;
    for (int &nozzle_id : preferred_map)
        --nozzle_id;

    // A configured print sequence is an execution constraint, not a hint for the flush-order
    // optimizer. Feed its effective per-layer order into the generic grouping scorer so only the
    // filament-to-nozzle mapping is optimized. This stays in the generic path; BBL grouping keeps
    // its existing implementation.
    std::vector<std::vector<unsigned int>> grouping_sequences = layer_filaments;
    std::vector<bool> preserve_layer_sequence(grouping_sequences.size(), false);
    auto apply_configured_sequence = [](std::vector<unsigned int>& layer, const std::vector<int>& configured) {
        if (layer.empty() || configured.empty())
            return false;

        const std::unordered_set<unsigned int> present(layer.begin(), layer.end());
        std::unordered_set<unsigned int> appended;
        std::vector<unsigned int> ordered;
        ordered.reserve(layer.size());
        for (int configured_id : configured) {
            if (configured_id <= 0)
                continue;
            const unsigned int filament_id = unsigned(configured_id - 1);
            if (present.count(filament_id) > 0 && appended.insert(filament_id).second)
                ordered.emplace_back(filament_id);
        }
        // Be tolerant of a sequence saved by an older project or created before filaments were
        // added: retain every used filament instead of silently dropping an unlisted one.
        for (unsigned int filament_id : layer)
            if (appended.insert(filament_id).second)
                ordered.emplace_back(filament_id);

        if (!ordered.empty()) {
            layer = std::move(ordered);
            return true;
        }
        return false;
    };

    if (mode == FilamentMapMode::fmmAutoForFlush) {
        if (!grouping_sequences.empty()) {
            const auto& first_layer_sequence = config.first_layer_print_sequence.values;
            if (std::any_of(first_layer_sequence.begin(), first_layer_sequence.end(), [](int id) { return id > 0; }))
                preserve_layer_sequence.front() =
                    apply_configured_sequence(grouping_sequences.front(), first_layer_sequence);
        }

        const std::vector<LayerPrintSequence> other_layer_sequences =
            get_other_layers_print_sequence(config.other_layers_print_sequence_nums.value, config.other_layers_print_sequence.values);
        for (size_t layer_idx = 1; layer_idx < grouping_sequences.size(); ++layer_idx) {
            const std::vector<int>* configured = nullptr;
            for (const LayerPrintSequence& candidate : other_layer_sequences) {
                if (layer_idx + 1 >= size_t(candidate.first.first) && layer_idx + 1 <= size_t(candidate.first.second))
                    configured = &candidate.second;
            }
            if (configured)
                preserve_layer_sequence[layer_idx] =
                    apply_configured_sequence(grouping_sequences[layer_idx], *configured);
        }
    }

    const int master_nozzle = std::clamp(config.master_extruder_id.value - 1, 0, int(nozzle_count) - 1);
    std::vector<int> grouped_map = auto_group_filaments_for_minimum_flush(
        filament_count,
        nozzle_count,
        grouping_sequences,
        prepare_generic_flush_matrices(config),
        unprintable_filaments,
        preferred_map,
        master_nozzle,
        preserve_layer_sequence);

    validate_map(grouped_map);
    auto result = LayeredNozzleGroupResult::create(grouped_map, nozzle_list, used_filaments);
    return result ? *result : LayeredNozzleGroupResult();
}

void ToolOrdering::reorder_generic_extruders_for_minimum_flush_volume(bool reorder_first_layer)
{
    const PrintConfig* print_config = m_print_config_ptr;
    if (!print_config && m_print_object_ptr) {
        print_config = &(m_print_object_ptr->print()->config());
    }

    if (!print_config || m_layer_tools.empty())
        return;

    const unsigned int number_of_extruders = (unsigned int)(print_config->filament_colour.values.size() + EPSILON);

    size_t             nozzle_nums = print_config->nozzle_diameter.values.size();
    // A generic multi-filament-per-nozzle printer still flushes through its filament-change
    // G-code when purge_in_prime_tower is disabled. Use the real transition matrix for its
    // layer ordering and comparison statistics.

    std::vector<FlushMatrix> nozzle_flush_mtx = prepare_generic_flush_matrices(*print_config);

    std::vector<int>filament_maps(number_of_extruders, 0);
    FilamentMapMode map_mode = FilamentMapMode::fmmAutoForFlush;

    std::vector<std::vector<unsigned int>> layer_filaments;
    for (auto& lt : m_layer_tools) {
        layer_filaments.emplace_back(lt.extruders);
    }

    std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(layer_filaments);

    std::vector<std::set<int>>geometric_unprintables = m_print->get_geometric_unprintable_filaments();

    filament_maps = m_print->get_filament_maps();
    map_mode = m_print->get_filament_map_mode();

    // Grouping now yields a nozzle-aware LayeredNozzleGroupResult; the
    // extruder-level filament_maps that feeds the ordering/stats below is derived from it.
    MultiNozzleUtils::LayeredNozzleGroupResult grouping_result;

    // The custom-sequence machinery is built before the grouping decision so both the static reorder
    // and the dynamic per-layer plan can share it. Pure local setup (no dependency on the
    // grouping result), so hoisting it above the branch does not change the static path's output.
    std::vector<std::vector<unsigned int>>filament_sequences;
    std::vector<unsigned int>filament_lists(number_of_extruders);
    std::iota(filament_lists.begin(), filament_lists.end(), 0);

    std::vector<LayerPrintSequence> other_layers_seqs;
    const ConfigOptionInts* other_layers_print_sequence_op = print_config->option<ConfigOptionInts>("other_layers_print_sequence");
    const ConfigOptionInt* other_layers_print_sequence_nums_op = print_config->option<ConfigOptionInt>("other_layers_print_sequence_nums");
    if (other_layers_print_sequence_op && other_layers_print_sequence_nums_op) {
        const std::vector<int>& print_sequence = other_layers_print_sequence_op->values;
        int                     sequence_nums = other_layers_print_sequence_nums_op->value;
        other_layers_seqs = get_other_layers_print_sequence(sequence_nums, print_sequence);
    }

    std::vector<unsigned int>first_layer_filaments;
    if (!m_layer_tools.empty())
        first_layer_filaments = m_layer_tools[0].extruders;

    const bool use_cyclic_ordering =
        (print_config->toolchange_ordering == ToolChangeOrderingType::Cyclic);

    // other_layers_seq: the layer_idx and extruder_idx are base on 1
    auto get_custom_seq = [&other_layers_seqs, &reorder_first_layer, &first_layer_filaments, &layer_filaments, use_cyclic_ordering](int layer_idx, std::vector<int>& out_seq) -> bool {
        if (!reorder_first_layer && layer_idx == 0) {
            out_seq.resize(first_layer_filaments.size());
            std::transform(first_layer_filaments.begin(), first_layer_filaments.end(), out_seq.begin(), [](auto item) {return item + 1; });
            return true;
        }
        for (size_t idx = other_layers_seqs.size() - 1; idx != size_t(-1); --idx) {
            const auto& other_layers_seq = other_layers_seqs[idx];
            if (layer_idx + 1 >= other_layers_seq.first.first && layer_idx + 1 <= other_layers_seq.first.second) {
                out_seq = other_layers_seq.second;
                return true;
            }
        }

        if (use_cyclic_ordering && layer_idx >= 0 && size_t(layer_idx) < layer_filaments.size()) {
            std::vector<unsigned int> ordered = layer_filaments[size_t(layer_idx)];
            std::sort(ordered.begin(), ordered.end());
            out_seq.resize(ordered.size());
            std::transform(ordered.begin(), ordered.end(), out_seq.begin(), [](auto item) { return int(item) + 1; });
            return true;
        }

        return false;
        };

    auto reorder_filaments = [&](const std::vector<int> &maps,
                                 std::vector<std::vector<unsigned int>> *sequences) {
        return reorder_generic_filaments_for_minimum_flush_volume(
            filament_lists, maps, layer_filaments, nozzle_flush_mtx,
            get_custom_seq, sequences);
    };

    const bool not_sequential = print_config->print_sequence != PrintSequence::ByObject ||
                                m_print->objects().size() == 1;

    // In sequential mode the generic grouping is calculated here. By-object
    // grouping is calculated print-wide in Print.cpp and only wrapped here.
    if (not_sequential) {
        grouping_result = get_recommended_generic_filament_maps(
            layer_filaments, m_print, map_mode, geometric_unprintables);
        std::vector<int> derived_maps = grouping_result.get_extruder_map(false);

        if (map_mode < FilamentMapMode::fmmManual) {
            if (derived_maps.empty())
                return;
            filament_maps = derived_maps;
        } else if (!derived_maps.empty()) {
            filament_maps = derived_maps;
        }

        if (!derived_maps.empty()) {
            std::vector<int> base_filament_map = print_config->filament_map.values;
            if (base_filament_map.size() != derived_maps.size())
                base_filament_map.assign(derived_maps.size(), 1);
            m_print->update_filament_maps_to_config(
                FilamentGroupUtils::update_used_filament_values(
                    base_filament_map, derived_maps, used_filaments));
        }
        std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(),
                       [](int value) { return value - 1; });
    } else {
        std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(),
                       [](int value) { return value - 1; });
        grouping_result = build_group_result_from_map(
            *print_config, filament_maps, used_filaments);
    }

    // Publish the generic nozzle assignment for downstream G-code generation.
    m_nozzle_group_result = grouping_result;
    // Orca: the ToolOrdering member is stored unconditionally, but the Print-level store is gated
    // behind the not-sequential check hoisted above.
    if (m_print != nullptr && not_sequential)
        m_print->set_nozzle_group_result(std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(m_nozzle_group_result));

    auto maps_without_group = filament_maps;
    for (auto& item : maps_without_group)
        item = 0;

    reorder_filaments(filament_maps, &filament_sequences);

    // The three-mode flush-stat caches are now computed from the nozzle-aware grouping result via the
    // nozzle-aware calc_filament_change_info_by_toolorder. Stats are GUI-only (surfaced by
    // get_filament_change_stats for the mode comparison); they never feed g-code, so this block is
    // byte-inert. For single-nozzle-per-extruder printers (H2D/X1/...) nozzle_id == extruder_id, so
    // every cached value equals the extruder-level stats.
    auto curr_flush_info = calc_filament_change_info_by_toolorder(print_config, grouping_result, nozzle_flush_mtx, filament_sequences);
    if (nozzle_nums <= 1)
        m_stats_by_single_extruder = curr_flush_info;
    else {
        m_stats_by_multi_extruder_curr = curr_flush_info;
        if (map_mode == fmmAutoForFlush)
            m_stats_by_multi_extruder_best = curr_flush_info;
    }

    // in multi extruder mode, collect data under the other modes (for the GUI mode comparison)
    if (nozzle_nums > 1) {
        // always calculate the info as if a single extruder were used
        {
            std::vector<std::vector<unsigned int>> single_extruder_sequences;
            reorder_filaments(maps_without_group, &single_extruder_sequences);
            // One logical nozzle (extruder 0, nozzle 0); every filament resolves to it.
            // diameter/volume_type are unused by the stat calc.
            MultiNozzleUtils::NozzleInfo single_nozzle;
            single_nozzle.volume_type = NozzleVolumeType::nvtStandard;
            single_nozzle.extruder_id = 0;
            single_nozzle.group_id    = 0;
            auto single_result = MultiNozzleUtils::LayeredNozzleGroupResult::create(maps_without_group, {single_nozzle}, used_filaments);
            if (single_result)
                m_stats_by_single_extruder = calc_filament_change_info_by_toolorder(print_config, *single_result, nozzle_flush_mtx, single_extruder_sequences);
        }
        // if not already in best-for-flush mode, also calculate the info under best-for-flush grouping
        if (map_mode != fmmAutoForFlush && !has_different_nozzle_diameters(*print_config)) {
            std::vector<std::vector<unsigned int>> best_sequences;
            auto best_group_result = get_recommended_generic_filament_maps(
                layer_filaments, m_print, fmmAutoForFlush, geometric_unprintables);
            std::vector<int> best_maps = best_group_result.get_extruder_map();
            reorder_filaments(best_maps, &best_sequences);
            m_stats_by_multi_extruder_best = calc_filament_change_info_by_toolorder(
                print_config, best_group_result, nozzle_flush_mtx, best_sequences);
        }
    }

    for (size_t i = 0; i < filament_sequences.size(); ++i)
        m_layer_tools[i].extruders = std::move(filament_sequences[i]);
}

} // namespace Slic3r
