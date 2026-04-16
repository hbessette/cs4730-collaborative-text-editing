BINARY  ?= build/p2p-editor
CONF    ?= scripts/cluster.conf
RESULTS ?= logs
OPS     ?= 1000
PEERS   ?=

_PEERS_FLAG = $(if $(PEERS),--peers $(PEERS),)
_OPS_FLAG   = $(if $(OPS),--ops $(OPS),)

.PHONY: build build-debug test eval eval-latency eval-convergence eval-concurrent \
        eval-scalability analyze-scalability collect analyze clean clean-eval

build:
	cmake -B build && cmake --build build -j$(shell nproc)

build-debug:
	cmake -B build -DENABLE_DEBUG_LOG=ON && cmake --build build -j$(shell nproc)

test:
	./build/tests

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

eval-concurrent:
	scripts/run_eval.sh --eval concurrent \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS) \
		$(_PEERS_FLAG) $(_OPS_FLAG)

eval-scalability:
	scripts/run_eval.sh --eval scalability \
		--binary $(BINARY) --config $(CONF) --results $(RESULTS)

analyze-scalability:
	python3 scripts/analyze_scalability.py $(RESULTS)/scalability

collect:
	scripts/collect_results.sh --results $(RESULTS) --config $(CONF)

analyze:
	python3 scripts/analyze_results.py $(RESULTS)

clean:
	rm -rf build/

clean-eval:
	rm -rf logs/
