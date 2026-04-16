BINARY  ?= build/p2p-editor
CONF    ?= scripts/cluster.conf
RESULTS ?= logs
OPS     ?= 1000
PEERS   ?=

_PEERS_FLAG = $(if $(PEERS),--peers $(PEERS),)
_OPS_FLAG   = $(if $(OPS),--ops $(OPS),)

.PHONY: build build-debug test \
        eval eval-latency eval-convergence eval-scalability \
        analyze \
        analyze-latency analyze-convergence analyze-scalability \
        collect clean clean-eval

# ── Build ─────────────────────────────────────────────────────────────────────

build:
	cmake -B build && cmake --build build -j$(shell nproc)

build-debug:
	cmake -B build -DENABLE_DEBUG_LOG=ON && cmake --build build -j$(shell nproc)

test:
	./build/tests

# ── Evaluation runs ───────────────────────────────────────────────────────────

eval:
	scripts/run_eval.sh \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS) \
		$(_PEERS_FLAG) $(_OPS_FLAG)

eval-latency:
	scripts/run_eval.sh --eval latency \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS) \
		$(_PEERS_FLAG) $(_OPS_FLAG)

eval-convergence:
	scripts/run_eval.sh --eval convergence \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS) \
		$(_PEERS_FLAG) $(_OPS_FLAG)

eval-scalability:
	scripts/run_eval.sh --eval scalability \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS)

# Run all analyses in one shot
analyze:
	python3 scripts/analyze_results.py $(RESULTS) \
		--csv $(RESULTS)/summary.csv \
		--chart $(RESULTS)/scalability/scalability_chart.png

# Latency only: CSV + per-config latency chart
analyze-latency:
	python3 scripts/analyze_latency.py $(RESULTS) \
		--csv $(RESULTS)/latency.csv \
		--chart $(RESULTS)/latency_chart.png

# Convergence only: CSV + pass/fail chart
analyze-convergence:
	python3 scripts/analyze_convergence.py $(RESULTS)/convergence \
		--csv $(RESULTS)/convergence.csv \
		--chart $(RESULTS)/convergence_chart.png

# Scalability only: CSV + 3-panel line chart
analyze-scalability:
	python3 scripts/analyze_scalability.py $(RESULTS)/scalability \
		--csv $(RESULTS)/scalability/scalability.csv \
		--chart $(RESULTS)/scalability/scalability_chart.png

# ── Utilities ─────────────────────────────────────────────────────────────────

collect:
	scripts/collect_results.sh --results $(RESULTS) --config $(CONF)

clean:
	rm -rf build/

clean-eval:
	rm -rf logs/
