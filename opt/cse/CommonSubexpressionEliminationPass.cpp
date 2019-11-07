/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CommonSubexpressionEliminationPass.h"

#include "CommonSubexpressionElimination.h"
#include "CopyPropagation.h"
#include "DexUtil.h"
#include "LocalDce.h"
#include "Purity.h"
#include "Walkers.h"

using namespace cse_impl;

namespace {

constexpr const char* METRIC_RESULTS_CAPTURED = "num_results_captured";
constexpr const char* METRIC_STORES_CAPTURED = "num_stores_captured";
constexpr const char* METRIC_ARRAY_LENGTHS_CAPTURED =
    "num_array_lengths_captured";
constexpr const char* METRIC_ELIMINATED_INSTRUCTIONS =
    "num_eliminated_instructions";
constexpr const char* METRIC_MAX_VALUE_IDS = "max_value_ids";
constexpr const char* METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT =
    "methods_using_other_tracked_location_bit";
constexpr const char* METRIC_INSTR_PREFIX = "instr_";
constexpr const char* METRIC_METHOD_BARRIERS = "num_method_barriers";
constexpr const char* METRIC_METHOD_BARRIERS_ITERATIONS =
    "num_method_barriers_iterations";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS =
    "num_conditionally_pure_methods";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS_ITERATIONS =
    "num_conditionally_pure_methods_iterations";
constexpr const char* METRIC_SKIPPED_DUE_TO_TOO_MANY_REGISTERS =
    "num_skipped_due_to_too_many_registers";
constexpr const char* METRIC_MAX_ITERATIONS = "num_max_iterations";

} // namespace

void CommonSubexpressionEliminationPass::bind_config() {
  bind("debug", false, m_debug);
  bind("runtime_assertions", false, m_runtime_assertions);
}

void CommonSubexpressionEliminationPass::run_pass(DexStoresVector& stores,
                                                  ConfigFiles& conf,
                                                  PassManager& mgr) {
  const auto scope = build_class_scope(stores);

  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ true);
  });

  auto pure_methods = /* Android framework */ get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());

  auto shared_state = SharedState(pure_methods);
  shared_state.init_scope(scope);

  // The following default 'features' of copy propagation would only
  // interfere with what CSE is trying to do.
  copy_propagation_impl::Config copy_prop_config;
  copy_prop_config.eliminate_const_classes = false;
  copy_prop_config.eliminate_const_strings = false;
  copy_prop_config.static_finals = false;

  auto aggregate_stats = [](Stats a, Stats b) {
    a.results_captured += b.results_captured;
    a.stores_captured += b.stores_captured;
    a.array_lengths_captured += b.array_lengths_captured;
    a.instructions_eliminated += b.instructions_eliminated;
    a.max_value_ids = std::max(a.max_value_ids, b.max_value_ids);
    a.methods_using_other_tracked_location_bit +=
        b.methods_using_other_tracked_location_bit;
    for (auto& p : b.eliminated_opcodes) {
      a.eliminated_opcodes[p.first] += p.second;
    }
    a.skipped_due_to_too_many_registers += b.skipped_due_to_too_many_registers;
    a.max_iterations = std::max(a.max_iterations, b.max_iterations);
    return a;
  };
  const auto stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [&](DexMethod* method) {
        const auto code = method->get_code();
        if (code == nullptr) {
          return Stats();
        }

        if (method->rstate.no_optimizations()) {
          code->clear_cfg();
          return Stats();
        }

        Stats stats;
        while (true) {
          stats.max_iterations++;
          TRACE(CSE, 3, "[CSE] processing %s", SHOW(method));
          always_assert(code->editable_cfg_built());
          CommonSubexpressionElimination cse(&shared_state, code->cfg());
          bool any_changes = cse.patch(is_static(method), method->get_class(),
                                       method->get_proto()->get_args(),
                                       copy_prop_config.max_estimated_registers,
                                       m_runtime_assertions);
          stats = aggregate_stats(stats, cse.get_stats());
          code->clear_cfg();

          if (!any_changes) {
            return stats;
          }

          // TODO: CopyPropagation will separately construct
          // an editable cfg. Don't do that, and fully convert that passes
          // to be cfg-based.

          copy_propagation_impl::CopyPropagation copy_propagation(
              copy_prop_config);
          copy_propagation.run(code, method);

          code->build_cfg(/* editable */ true);

          auto local_dce = LocalDce(shared_state.get_pure_methods());
          local_dce.dce(code);

          if (traceEnabled(CSE, 5)) {
            TRACE(CSE, 5, "[CSE] end of iteration:\n%s", SHOW(code->cfg()));
          }
        }
      },
      aggregate_stats,
      Stats{},
      m_debug ? 1 : redex_parallel::default_num_threads());
  mgr.incr_metric(METRIC_RESULTS_CAPTURED, stats.results_captured);
  mgr.incr_metric(METRIC_STORES_CAPTURED, stats.stores_captured);
  mgr.incr_metric(METRIC_ARRAY_LENGTHS_CAPTURED, stats.array_lengths_captured);
  mgr.incr_metric(METRIC_ELIMINATED_INSTRUCTIONS,
                  stats.instructions_eliminated);
  mgr.incr_metric(METRIC_MAX_VALUE_IDS, stats.max_value_ids);
  mgr.incr_metric(METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT,
                  stats.methods_using_other_tracked_location_bit);
  auto& shared_state_stats = shared_state.get_stats();
  mgr.incr_metric(METRIC_METHOD_BARRIERS, shared_state_stats.method_barriers);
  mgr.incr_metric(METRIC_METHOD_BARRIERS_ITERATIONS,
                  shared_state_stats.method_barriers_iterations);
  mgr.incr_metric(METRIC_CONDITIONALLY_PURE_METHODS,
                  shared_state_stats.conditionally_pure_methods);
  mgr.incr_metric(METRIC_CONDITIONALLY_PURE_METHODS_ITERATIONS,
                  shared_state_stats.conditionally_pure_methods_iterations);
  for (auto& p : stats.eliminated_opcodes) {
    std::string name = METRIC_INSTR_PREFIX;
    name += SHOW(static_cast<IROpcode>(p.first));
    mgr.incr_metric(name, p.second);
  }
  mgr.incr_metric(METRIC_SKIPPED_DUE_TO_TOO_MANY_REGISTERS,
                  stats.skipped_due_to_too_many_registers);
  mgr.incr_metric(METRIC_MAX_ITERATIONS, stats.max_iterations);

  shared_state.cleanup();
}

static CommonSubexpressionEliminationPass s_pass;
